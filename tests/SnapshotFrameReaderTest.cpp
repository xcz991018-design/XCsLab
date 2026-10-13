#include "SnapshotFrameReader.hpp"
#include "InspectionService.hpp"
#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTimer>
#include <iostream>
#include <stdexcept>

static void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
static cv::Mat dial(int identity) {
    cv::Mat frame(96, 96, CV_8UC3, cv::Scalar(210, 210, 210));
    cv::circle(frame, {48, 48}, 35, {30, 30, 30}, 2);
    cv::line(frame, {48, 48}, {70, 25}, {20, 20, 20}, 3);
    frame.at<cv::Vec3b>(0, 0) = cv::Vec3b(identity, 0, 0);
    return frame;
}

class StartupSource : public ICameraSource {
public:
    explicit StartupSource(int identity) : m_identity(identity) {}
    bool open() override { return true; }
    void close() override {}
    bool isOpened() const override { return true; }
    std::string getSourceName() const override { return "startup stream"; }
    bool grabFrame(cv::Mat& frame) override {
        ++m_reads;
        frame = m_identity == 0 || m_reads <= 5 ? cv::Mat(96, 96, CV_8UC3, cv::Scalar(128, 128, 128)) : dial(m_identity);
        return true;
    }
    FrameCaptureResult captureSnapshot() override {
        SnapshotDecodePolicy policy;
        policy.timeout = std::chrono::milliseconds(100);
        policy.warmup = std::chrono::milliseconds(10);
        return readDecodedSnapshot([this](cv::Mat& frame) { return grabFrame(frame); }, policy);
    }
private:
    int m_identity;
    int m_reads{0};
};

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        if (app.arguments().size() == 3 && app.arguments()[1] == "--check-frame") {
            const auto frame = cv::imread(app.arguments()[2].toStdString());
            require(!frame.empty(), "Image could not be decoded");
            cv::Scalar mean, deviation;
            cv::meanStdDev(frame, mean, deviation);
            std::cout << "Frame mean: " << mean << "; deviation: " << deviation << '\n';
            require(isUniformSnapshotFrame(frame), "Image was not classified as a gray/blank frame");
            std::cout << "Observed gray capture correctly rejected\n";
            return 0;
        }
        QTemporaryDir output(QDir::currentPath() + "/snapshot-test-XXXXXX");
        require(output.isValid(), "Test directory failed");
        // All storage stays local to this test; no database connection is needed.
        const auto settingsPath = output.path() + "/disabled-mysql.json";
        QFile settings(settingsPath);
        require(settings.open(QIODevice::WriteOnly), "Settings open failed");
        settings.write("{\"version\":1,\"enabled\":false}");
        settings.close();
        qunsetenv("GAUGE_MYSQL_DATABASE");
        qputenv("GAUGE_MYSQL_CONFIG_PATH", settingsPath.toUtf8());
        require(isUniformSnapshotFrame(cv::Mat(32, 32, CV_8UC3, cv::Scalar(128, 128, 128))), "Gray frame accepted");
        require(isUniformSnapshotFrame(cv::Mat::zeros(32, 32, CV_8UC3)), "Black blank frame accepted");
        require(isUniformSnapshotFrame(cv::Mat(32, 32, CV_8UC1, cv::Scalar(255))), "White blank frame accepted");
        cv::Mat noise(32, 32, CV_8UC3);
        cv::randu(noise, 127, 130);
        require(isUniformSnapshotFrame(noise), "Near-uniform decoder noise accepted");
        require(!isUniformSnapshotFrame(dial(10)), "Grayscale dial rejected");
        require(!isUniformSnapshotFrame(cv::Mat(32, 32, CV_8UC3, cv::Scalar(0, 0, 200))), "Colored image mistaken for gray placeholder");

        SnapshotDecodePolicy policy;
        policy.timeout = std::chrono::milliseconds(100);
        policy.warmup = std::chrono::milliseconds(0);
        int reads = 0;
        const auto recovered = readDecodedSnapshot([&](cv::Mat& frame) {
            ++reads;
            frame = reads <= 5 ? cv::Mat(96, 96, CV_8UC3, cv::Scalar(128, 128, 128)) : dial(reads);
            return true;
        }, policy);
        require(recovered.ok() && reads == 8 && recovered.frame.at<cv::Vec3b>(0, 0)[0] == 8,
                "Startup placeholder or first partial frame was returned");
        const auto captured = recovered.capturedAt;
        require(captured.time_since_epoch().count() > 0, "Valid capture timestamp missing");
        reads = 0;
        const auto keyFrame = readDecodedSnapshot([&](cv::Mat& frame) {
            frame = dial(++reads); return true;
        }, policy, [&]() { return reads < 5 ? 'P' : reads == 5 ? 'I' : 'P'; });
        require(keyFrame.ok() && reads == 7, "Partial P frames accepted before key frame");
        const auto missingKey = readDecodedSnapshot([](cv::Mat& frame) { frame = dial(12); return true; },
                                                   SnapshotDecodePolicy{std::chrono::milliseconds(20), std::chrono::milliseconds(0), 3},
                                                   []() { return 'P'; });
        require(!missingKey.ok() && missingKey.frame.empty(), "Stream without key frames accepted");
        reads = 0;
        const auto interrupted = readDecodedSnapshot([&](cv::Mat& frame) {
            ++reads;
            frame = dial(reads);
            return reads != 3;
        }, policy);
        require(interrupted.ok() && reads == 6, "Failed read did not reset startup sequence");
        policy.warmup = std::chrono::milliseconds(20);
        const auto started = std::chrono::steady_clock::now();
        require(readDecodedSnapshot([](cv::Mat& frame) { frame = dial(20); return true; }, policy).ok() &&
                std::chrono::steady_clock::now() - started >= policy.warmup, "Warm-up duration ignored");
        policy.timeout = std::chrono::milliseconds(20);
        policy.warmup = std::chrono::milliseconds(0);
        const auto blank = readDecodedSnapshot([](cv::Mat& frame) {
            frame = cv::Mat(96, 96, CV_8UC3, cv::Scalar(128, 128, 128)); return true;
        }, policy);
        require(!blank.ok() && blank.frame.empty() && blank.failure == CaptureFailure::NoFrame &&
                blank.message.find("uniform") != std::string::npos, "Permanent gray stream became evidence");
        const auto failed = readDecodedSnapshot([](cv::Mat& frame) { frame = dial(50); return false; }, policy);
        require(!failed.ok() && failed.frame.empty(), "Failed read returned stale output buffer");

        // Exercise multiple devices through the same capture/storage scheduler
        // used by batch inspection. Gray devices must not produce original.png.
        InspectionService service(nullptr, 2, [](const InspectionTask& task) -> CameraSourcePtr {
            return std::make_shared<StartupSource>(task.device.cameraId.toInt());
        });
        QList<InspectionTask> tasks;
        for (int id : {11, 0, 22}) {
            InspectionTask task;
            task.device.cameraId = QString::number(id);
            task.device.protocolType = CameraProtocolType::ONVIF_RTSP_STREAM;
            task.captureOnly = id != 0;
            task.returnImages = true;
            task.outputDirectory = output.path();
            tasks.append(task);
        }
        bool valid = true, finished = false;
        int outcomes = 0;
        QEventLoop loop;
        QTimer guard;
        guard.setSingleShot(true);
        QObject::connect(&service, &InspectionService::taskFinished, &loop, [&](const InspectionOutcome& result) {
            ++outcomes;
            const int id = result.cameraId.toInt();
            if (id == 0) {
                valid &= result.errorCode == "NO_FRAME" && !result.recognitionPerformed &&
                         result.originalPath.isEmpty() && result.capturedFrame.empty();
            } else {
                const auto saved = cv::imread(result.originalPath.toStdString());
                valid &= result.errorCode.isEmpty() && !saved.empty() &&
                         cv::norm(saved, dial(id), cv::NORM_INF) == 0 &&
                         result.capturedAt.isValid();
            }
        });
        QObject::connect(&service, &InspectionService::finished, &loop, [&](bool) { finished = true; loop.quit(); });
        QObject::connect(&guard, &QTimer::timeout, &loop, &QEventLoop::quit);
        require(service.start(tasks), "Batch startup refused");
        guard.start(5000);
        loop.exec();
        require(valid && finished && outcomes == 3, "Batch saved startup gray frames or mixed device images");
        std::cout << "Decoder warm-up, gray rejection, read failures and batch evidence integrity passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
