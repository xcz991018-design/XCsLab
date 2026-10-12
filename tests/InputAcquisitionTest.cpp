#include "InspectionService.hpp"
#include "CameraPreviewService.hpp"
#include "FileCameraSource.hpp"
#include "SDKCameraSourceStub.hpp"
#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include <QTemporaryDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>
#include <thread>
#include <iostream>

class TestSource : public ICameraSource {
public:
    explicit TestSource(QString id) : id(std::move(id)) {}
    bool open() override {
        if (QThread::currentThread() == QCoreApplication::instance()->thread()) workerOnly.store(false);
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        return true;
    }
    bool isOpened() const override { return true; }
    void close() override { if (QThread::currentThread() == QCoreApplication::instance()->thread()) workerOnly.store(false); }
    bool grabFrame(cv::Mat& frame) override { const auto result = readFrame(); frame = result.frame; return result.ok(); }
    std::string getSourceName() const override { return "test"; }
    FrameCaptureResult readFrame() override {
        FrameCaptureResult result;
        result.frame = cv::Mat(24, 24, CV_8UC3, cv::Scalar(id == "second" ? 90 : 40, 0, 0));
        result.capturedAt = std::chrono::system_clock::now();
        if (id == "stale") result.failure = CaptureFailure::StaleFrame;
        return result;
    }
    static std::atomic<bool> workerOnly;
private:
    QString id;
};
std::atomic<bool> TestSource::workerOnly{true};

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir directory;
    if (!directory.isValid()) return 1;
    int acquisitions = 0;
    InspectionService service(nullptr, 1, [&](const InspectionTask& task) {
        ++acquisitions;
        return std::make_shared<TestSource>(task.device.cameraId);
    });
    auto run = [&](InspectionTask task) {
        QEventLoop loop;
        QTimer timeout;
        bool timedOut = false;
        timeout.setSingleShot(true);
        InspectionOutcome outcome;
        QObject::connect(&service, &InspectionService::taskFinished, &loop, [&](const InspectionOutcome& result) { outcome = result; });
        QObject::connect(&service, &InspectionService::finished, &loop, [&]() { loop.quit(); });
        QObject::connect(&timeout, &QTimer::timeout, &loop, [&]() { timedOut = true; loop.quit(); });
        task.outputDirectory = directory.path();
        if (!service.start({task})) throw std::runtime_error("Task refused");
        timeout.start(5000);
        loop.exec();
        if (timedOut) throw std::runtime_error("Task timed out");
        return outcome;
    };
    InspectionTask task;
    task.device.cameraId = "stale";
    task.device.protocolType = CameraProtocolType::ONVIF_RTSP_STREAM;
    task.captureOnly = true;
    task.returnImages = true;
    auto stale = run(task);
    if (stale.errorCode != "STALE_FRAME" || !stale.capturedFrame.empty() || !stale.originalPath.isEmpty()) return 1;
    task.device.cameraId = "first";
    auto live = run(task);
    if (!live.errorCode.isEmpty() || live.originalPath.isEmpty() || live.recognitionPerformed) return 1;
    const int initialAcquisitions = acquisitions;
    task.inputKind = InspectionInputKind::HistoricalImage;
    task.parentTaskId = live.taskId;
    task.originalCapturedAt = live.capturedAt;
    task.localImagePath = live.originalPath;
    const auto history = run(task);
    if (!history.errorCode.isEmpty() || history.parentTaskId != live.taskId || history.capturedAt != live.capturedAt || acquisitions != initialAcquisitions) return 1;
    if (cv::norm(live.capturedFrame, history.capturedFrame, cv::NORM_INF) != 0) return 1;
    task.parentTaskId = "wrong-task";
    const auto wrongIdentity = run(task);
    if (wrongIdentity.errorCode != "HISTORICAL_IDENTITY_MISMATCH" || !wrongIdentity.originalPath.isEmpty() || acquisitions != initialAcquisitions) return 1;
    task.parentTaskId = live.taskId;
    task.localImagePath = directory.path() + "/deleted.png";
    const auto missingHistory = run(task);
    if (missingHistory.errorCode != "IMAGE_READ_FAILED" || !missingHistory.originalPath.isEmpty() || acquisitions != initialAcquisitions) return 1;
    task.inputKind = InspectionInputKind::LocalImage;
    task.localImagePath = live.originalPath;
    task.parentTaskId.clear();
    const auto local = run(task);
    if (!local.errorCode.isEmpty() || local.inputKind != InspectionInputKind::LocalImage || acquisitions != initialAcquisitions) return 1;
    QFile metadata(directory.path() + "/" + local.taskId + "/result.json");
    if (!metadata.open(QIODevice::ReadOnly) || QJsonDocument::fromJson(metadata.readAll()).object()["input_kind"].toString() != "local_image") return 1;
    task.inputKind = InspectionInputKind::CameraSnapshot;
    task.device.protocolType = CameraProtocolType::LOCAL_FILE;
    const auto invalidLive = run(task);
    if (invalidLive.errorCode != "INVALID_INPUT" || acquisitions != initialAcquisitions) return 1;

    // Legacy APIs also cannot return stale caller buffers or sample/diagnostic images.
    cv::Mat frame(16, 16, CV_8UC3);
    FileCameraSource file(directory.path().toStdString() + "/missing.png");
    if (file.grabFrame(frame) || !frame.empty() || file.captureSnapshot().failure != CaptureFailure::OpenFailed) return 1;
    ONVIFDeviceInfo info;
    info.rtspUri.clear();
    ONVIFDeviceManager onvif(info);
    onvif.setSampleFrame(live.capturedFrame);
    frame = live.capturedFrame;
    if (onvif.grabFrame(frame) || !frame.empty() || onvif.readFrame().failure != CaptureFailure::NotOpen || onvif.captureSnapshot().failure != CaptureFailure::InvalidInput) return 1;
    SDKCameraSourceStub sdk("demo");
    sdk.setSampleFrame(live.capturedFrame);
    if (sdk.captureSnapshot().failure != CaptureFailure::UnsupportedProtocol || sdk.grabFrame(frame) || !frame.empty()) return 1;

    // Switching while an old stream connects suppresses that stream's late result.
    CameraPreviewService preview(nullptr, [](const InspectionTask& request) { return std::make_shared<TestSource>(request.device.cameraId); });
    QEventLoop loop;
    QTimer timeout, heartbeat;
    bool oldDelivered = false, received = false;
    int ticks = 0;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, [&]() { loop.quit(); });
    QObject::connect(&heartbeat, &QTimer::timeout, &loop, [&]() { ++ticks; });
    QObject::connect(&preview, &CameraPreviewService::frameReady, &loop, [&](const QString& id, const FrameCaptureResult& result) {
        oldDelivered |= id == "first";
        if (id == "second") { received = result.ok() && result.frame.at<cv::Vec3b>(0, 0)[0] == 90; loop.quit(); }
    });
    task.device.cameraId = "first";
    preview.select(task);
    task.device.cameraId = "second";
    preview.select(task);
    timeout.start(5000);
    heartbeat.start(5);
    loop.exec();
    if (!received || oldDelivered || ticks < 2 || !TestSource::workerOnly.load()) return 1;
    preview.stop();
    std::cout << "Explicit inputs, no fallback, typed failures, historical identity and nonblocking preview switching passed\n";
    return 0;
}
