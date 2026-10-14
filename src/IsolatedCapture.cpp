#include "IsolatedCapture.hpp"
#include "DeviceCodec.hpp"
#include <QCoreApplication>
#include <QProcess>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>
#include <QThread>
#include <cstdio>

namespace {
FrameCaptureResult readWorkerResult(const QString& folder, const QJsonObject& json) {
    FrameCaptureResult result;
    const int failure = json.value("failure").toInt(-1);
    if (failure < 0 || failure > static_cast<int>(CaptureFailure::WorkerFailed)) {
        result.failure = CaptureFailure::WorkerFailed;
        return result;
    }
    result.failure = static_cast<CaptureFailure>(failure);
    result.message = json.value("message").toString().toStdString();
    if (result.failure == CaptureFailure::None) {
        const int sequence = json.value("sequence").toInt();
        QFile image(folder + (sequence > 0 ? QString("/frame_%1.png").arg(sequence) : QString("/frame.png")));
        if (image.open(QIODevice::ReadOnly)) {
            const auto bytes = image.readAll();
            result.frame = cv::imdecode(std::vector<uchar>(bytes.begin(), bytes.end()), cv::IMREAD_COLOR);
        }
        if (result.frame.empty()) result.failure = CaptureFailure::NoFrame;
        result.capturedAt = std::chrono::system_clock::time_point(std::chrono::milliseconds(
            json.value("captured_at_ms").toVariant().toLongLong()));
    }
    return result;
}
bool saveWorkerResult(const QString& folder, const FrameCaptureResult& result, int sequence) {
    if (result.ok()) {
        std::vector<uchar> bytes;
        if (!cv::imencode(".png", result.frame, bytes)) return false;
        QSaveFile image(folder + (sequence > 0 ? QString("/frame_%1.png").arg(sequence) : QString("/frame.png")));
        if (!image.open(QIODevice::WriteOnly) || image.write(reinterpret_cast<const char*>(bytes.data()), bytes.size()) != qint64(bytes.size()) || !image.commit()) return false;
    }
    QJsonObject json;
    json["sequence"] = sequence;
    json["failure"] = static_cast<int>(result.failure);
    json["message"] = QString::fromStdString(result.message);
    json["captured_at_ms"] = static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(result.capturedAt.time_since_epoch()).count());
    QSaveFile output(folder + (sequence > 0 ? QString("/result_%1.json").arg(sequence) : QString("/result.json")));
    const auto bytes = QJsonDocument(json).toJson();
    return output.open(QIODevice::WriteOnly) && output.write(bytes) == bytes.size() && output.commit();
}
}

struct IsolatedPreview::State {
    QTemporaryDir directory;
    QProcess worker;
    int sequence{0};
    ~State() { if (worker.state() != QProcess::NotRunning) { worker.kill(); worker.waitForFinished(1000); } }
};
IsolatedPreview::IsolatedPreview() : m_state(std::make_unique<State>()) {}
IsolatedPreview::~IsolatedPreview() = default;
FrameCaptureResult IsolatedPreview::read(const InspectionTask& task, const std::shared_ptr<std::atomic<bool>>& cancel) {
    FrameCaptureResult result;
    auto& state = *m_state;
    QElapsedTimer timer;
    timer.start();
    const int deadline = qBound(100, task.captureTimeoutMs, 120000);
    auto fail = [&](CaptureFailure failure, const char* message) {
        state.worker.kill();
        state.worker.waitForFinished(1000);
        result.failure = failure;
        result.message = message;
        return result;
    };
    if (cancel->load()) return fail(CaptureFailure::Cancelled, "Preview cancelled");
    if (!state.directory.isValid()) return fail(CaptureFailure::WorkerFailed, "Preview workspace unavailable");
    if (state.sequence == 0) {
        state.worker.setProgram(QCoreApplication::applicationFilePath());
        state.worker.setArguments({"--capture-preview-worker", state.directory.path()});
        state.worker.setStandardOutputFile(QProcess::nullDevice());
        state.worker.setStandardErrorFile(QProcess::nullDevice());
        state.worker.start();
        while (state.worker.state() == QProcess::Starting) {
            if (cancel->load()) return fail(CaptureFailure::Cancelled, "Preview cancelled");
            if (timer.elapsed() >= deadline) return fail(CaptureFailure::Timeout, "Preview startup deadline exceeded");
            state.worker.waitForStarted(25);
        }
        if (state.worker.state() != QProcess::Running) return fail(CaptureFailure::WorkerFailed, "Preview worker failed to start");
        state.worker.write(QJsonDocument(deviceToJson(task.device, true)).toJson(QJsonDocument::Compact) + '\n');
    }
    const int sequence = ++state.sequence;
    state.worker.write(QByteArray::number(sequence) + '\n');
    while (timer.elapsed() < deadline) {
        if (cancel->load()) return fail(CaptureFailure::Cancelled, "Preview cancelled");
        const QString metadataPath = state.directory.filePath(QString("result_%1.json").arg(sequence));
        QFile metadata(metadataPath);
        if (metadata.open(QIODevice::ReadOnly)) {
            const auto json = QJsonDocument::fromJson(metadata.readAll()).object();
            if (json.value("sequence").toInt() == sequence) {
                result = readWorkerResult(state.directory.path(), json);
                metadata.close();
                QFile::remove(metadataPath);
                QFile::remove(state.directory.filePath(QString("frame_%1.png").arg(sequence)));
                return result;
            }
        }
        if (state.worker.state() == QProcess::NotRunning) return fail(CaptureFailure::WorkerFailed, "Preview worker exited unexpectedly");
        state.worker.waitForFinished(10);
    }
    return fail(CaptureFailure::Timeout, "Preview frame deadline exceeded");
}

FrameCaptureResult isolatedCapture(const InspectionTask& task,
    const std::shared_ptr<std::atomic<bool>>& cancel) {
    FrameCaptureResult result;
    if (cancel->load()) {
        result.failure = CaptureFailure::Cancelled;
        result.message = "Acquisition cancelled";
        return result;
    }
    QTemporaryDir directory;
    if (!directory.isValid()) {
        result.failure = CaptureFailure::ReadFailed;
        result.message = "Unable to create acquisition workspace";
        return result;
    }
    QProcess worker;
    worker.setProgram(QCoreApplication::applicationFilePath());
    worker.setArguments({"--capture-worker", directory.path()});
    // Backend logs can include connection details. Never forward them to the UI.
    worker.setStandardOutputFile(QProcess::nullDevice());
    worker.setStandardErrorFile(QProcess::nullDevice());
    QElapsedTimer timer;
    timer.start();
    worker.start();
    const int deadline = qBound(100, task.captureTimeoutMs, 120000);
    bool requestSent = false;
    while (worker.state() != QProcess::NotRunning) {
        if (cancel->load() || timer.elapsed() >= deadline) {
            worker.kill();
            worker.waitForFinished(1000);
            result.failure = cancel->load() ? CaptureFailure::Cancelled : CaptureFailure::Timeout;
            result.message = cancel->load() ? "Acquisition cancelled" : "Acquisition deadline exceeded";
            return result;
        }
        if (worker.state() == QProcess::Starting) {
            worker.waitForStarted(25);
        } else worker.waitForFinished(25);
        if (!requestSent && worker.state() == QProcess::Running) {
            auto request = deviceToJson(task.device, true);
            request["_prefer_onvif_snapshot"] = task.preferOnvifSnapshot;
            worker.write(QJsonDocument(request).toJson(QJsonDocument::Compact));
            worker.closeWriteChannel();
            requestSent = true;
        }
    }
    QFile metadata(directory.filePath("result.json"));
    if (worker.exitStatus() != QProcess::NormalExit || worker.exitCode() != 0 || !metadata.open(QIODevice::ReadOnly)) {
        result.failure = CaptureFailure::WorkerFailed;
        result.message = "Acquisition worker failed to start or exited unexpectedly";
        return result;
    }
    result = readWorkerResult(directory.path(), QJsonDocument::fromJson(metadata.readAll()).object());
    if (cancel->load() || timer.elapsed() >= deadline) {
        result.frame.release();
        result.failure = cancel->load() ? CaptureFailure::Cancelled : CaptureFailure::Timeout;
        result.message = cancel->load() ? "Acquisition cancelled" : "Acquisition deadline exceeded";
    }
    return result;
}

int runCaptureWorker(int argc, char** argv) {
    const bool preview = argc == 3 && QString::fromLocal8Bit(argv[1]) == "--capture-preview-worker";
    if (!preview && (argc != 3 || QString::fromLocal8Bit(argv[1]) != "--capture-worker")) return -1;
    // Worker launch precedes the GUI setup. Apply transport settings here too.
    qputenv("OPENCV_FFMPEG_CAPTURE_OPTIONS", "rtsp_transport;tcp");
    QCoreApplication app(argc, argv);
    QFile input;
    if (!input.open(stdin, QIODevice::ReadOnly)) return 2;
    const auto document = QJsonDocument::fromJson(preview ? input.readLine() : input.readAll());
    if (!document.isObject()) return 2;
    InspectionTask task;
    task.device = deviceFromJson(document.object());
    task.preferOnvifSnapshot = !preview && document.object()["_prefer_onvif_snapshot"].toBool();
    FrameCaptureResult result;
    try {
        auto source = InspectionService::createCameraSource(task);
        if (preview) {
            const bool opened = source && source->open();
            while (true) {
                bool validSequence = false;
                const auto command = input.readLine();
                // Windows QFile pipes may report no available bytes between
                // requests. The parent owns shutdown and kills this process.
                if (command.isEmpty()) { QThread::msleep(10); continue; }
                const int sequence = command.trimmed().toInt(&validSequence);
                if (!validSequence) return 2;
                FrameCaptureResult acquired;
                if (!source) acquired.failure = CaptureFailure::UnsupportedProtocol;
                else if (!opened) {
                    acquired.failure = CaptureFailure::OpenFailed;
                    if (auto* onvif = dynamic_cast<ONVIFDeviceManager*>(source.get())) acquired.message = onvif->lastError();
                }
                else {
                    // The stream opens asynchronously; let the parent's deadline
                    // govern first-frame wait and disconnected-stream recovery.
                    do {
                        acquired = source->readFrame();
                        if (acquired.ok()) break;
                        if (acquired.failure != CaptureFailure::NoFrame && acquired.failure != CaptureFailure::StaleFrame) break;
                        QThread::msleep(10);
                    } while (true);
                }
                if (!saveWorkerResult(QString::fromLocal8Bit(argv[2]), acquired, sequence)) return 3;
            }
            if (source) source->close();
            return 0;
        }
        if (!source) result.failure = CaptureFailure::UnsupportedProtocol;
        else { result = source->captureSnapshot(); source->close(); }
        if (!result.ok()) result.frame.release();
        if (result.failure == CaptureFailure::None && result.frame.empty()) result.failure = CaptureFailure::NoFrame;
        const QString folder = QString::fromLocal8Bit(argv[2]);
        if (!saveWorkerResult(folder, result, 0)) return 3;
        return 0;
    } catch (...) { return 3; }
}
