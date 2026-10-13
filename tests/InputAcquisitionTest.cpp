#include "InspectionService.hpp"
#include "DeviceConfigManager.hpp"
#include "CameraPreviewService.hpp"
#include "FileCameraSource.hpp"
#include "SDKCameraSourceStub.hpp"
#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
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
    QTemporaryDir directory(QDir::currentPath() + "/capture-test-XXXXXX");
    if (!directory.isValid()) return 1;
    if (!app.arguments().contains("--capture-only")) {
    // An empty registry must stay empty; local analysis has its own task identity.
    const auto registryPath = directory.path() + "/devices.json";
    QFile registry(registryPath);
    if (!registry.open(QIODevice::WriteOnly)) return 1;
    registry.write("{\"devices\":[],\"system\":{}}");
    registry.close();
    auto& manager = DeviceConfigManager::instance();
    if (!manager.load(registryPath) || !manager.getAllDevices().isEmpty() ||
        !manager.getLastActiveDeviceId().isEmpty()) { std::cerr << "Empty registry failed: " << manager.lastError().toStdString() << " devices=" << manager.getAllDevices().size() << "\n"; return 1; }
    QJsonArray legacyDevices;
    for (const auto& id : {"CAM_20", "CAM_5-1-2", "CAM_5-3-1", "CAM_5-3-4", "CAM_5-4-1"})
        legacyDevices.append(QJsonObject{{"camera_id", id}, {"protocol_type", "LOCAL_FILE"}});
    legacyDevices.append(QJsonObject{{"camera_id", "REAL_CAMERA"}, {"protocol_type", "ONVIF_RTSP_STREAM"}});
    legacyDevices.append(QJsonObject{{"camera_id", "USER_IMAGE"}, {"protocol_type", "LOCAL_FILE"}});
    if (!registry.open(QIODevice::WriteOnly | QIODevice::Truncate)) return 1;
    registry.write(QJsonDocument(QJsonObject{{"devices", legacyDevices},
        {"system", QJsonObject{{"last_active_device_id", "CAM_20"}}}}).toJson());
    registry.close();
    if (!manager.load(registryPath) || manager.getAllDevices().size() != 2 ||
        manager.getLastActiveDeviceId() == "CAM_20") return 1;
    if (!registry.open(QIODevice::ReadOnly)) return 1;
    if (QJsonDocument::fromJson(registry.readAll()).object()["devices"].toArray().size() != 2) return 1;
    registry.close();
    if (!manager.load(registryPath) || manager.getAllDevices().size() != 2) return 1;
    // A later ONVIF scan must resolve to the existing physical camera archive.
    DiscoveredCameraItem configured;
    configured.cameraId = "CAM_IP_192_168_110_11";
    configured.ipOrSn = "192.168.110.11";
    configured.protocolType = CameraProtocolType::SDK_JLINK;
    configured.customName = "Configured gauge";
    configured.username = "operator";
    configured.password = "test-secret";
    configured.centerX = 999;
    configured.isAutoConnect = true;
    manager.addOrUpdateDevice(configured, false);
    auto discovered = configured;
    discovered.cameraId = "CAM_ONVIF_192_168_110_11";
    discovered.protocolType = CameraProtocolType::ONVIF_RTSP_STREAM;
    discovered.customName = "ONVIF camera";
    discovered.password.clear();
    discovered.centerX = 407.407;
    discovered.isAutoConnect = false;
    const auto merged = manager.registerDiscoveredDevice(discovered, false);
    if (merged.cameraId != configured.cameraId || merged.centerX != 999 ||
        merged.password != configured.password || merged.customName != configured.customName ||
        merged.protocolType != configured.protocolType || !merged.isAutoConnect ||
        manager.getAllDevices().size() != 3) return 1;
    discovered.cameraId = "CAM_ONVIF_192_168_110_12";
    discovered.ipOrSn = "192.168.110.12";
    manager.registerDiscoveredDevice(discovered, false);
    if (manager.getAllDevices().size() != 4) return 1;
    // Simulate a legacy registry containing both protocol identities.
    discovered.cameraId = "CAM_ONVIF_192_168_110_11";
    discovered.ipOrSn = configured.ipOrSn;
    manager.addOrUpdateDevice(discovered, false);
    manager.setLastActiveDeviceId(discovered.cameraId, false);
    if (!manager.save(registryPath) || !manager.load(registryPath)) return 1;
    const auto restored = manager.getDevice(configured.cameraId);
    if (manager.getAllDevices().size() != 4 || manager.hasDevice(discovered.cameraId) ||
        restored.centerX != 999 || restored.password != configured.password ||
        manager.getLastActiveDeviceId() != configured.cameraId) return 1;
    if (!manager.load(registryPath) || manager.getAllDevices().size() != 4) return 1;
    }
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
    if (!app.arguments().contains("--capture-only")) {
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
    }

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
    if (preview.snapshot("first").ok()) return 1;
    auto snapshot = preview.snapshot("second");
    if (!snapshot.ok()) return 1;
    snapshot.frame.setTo(cv::Scalar(12, 34, 56));
    if (preview.snapshot("second").frame.at<cv::Vec3b>(0, 0)[0] != 90) return 1;
    task.device.protocolType = CameraProtocolType::ONVIF_RTSP_STREAM;
    task.previewSnapshot = snapshot.frame;
    task.previewCapturedAt = QDateTime::fromMSecsSinceEpoch(
        std::chrono::duration_cast<std::chrono::milliseconds>(snapshot.capturedAt.time_since_epoch()).count(), Qt::UTC);
    const auto capturedPreview = run(task);
    if (!capturedPreview.errorCode.isEmpty() || acquisitions != initialAcquisitions ||
        capturedPreview.capturedAt != task.previewCapturedAt ||
        cv::norm(capturedPreview.capturedFrame, snapshot.frame, cv::NORM_INF) != 0 ||
        cv::norm(cv::imread(capturedPreview.originalPath.toStdString()), snapshot.frame, cv::NORM_INF) != 0) return 1;
    task.previewCapturedAt = QDateTime::currentDateTimeUtc().addSecs(-3);
    const auto expiredPreview = run(task);
    if (expiredPreview.errorCode != "STALE_FRAME" || !expiredPreview.originalPath.isEmpty() ||
        acquisitions != initialAcquisitions) return 1;
    preview.select(InspectionTask());
    if (preview.snapshot("second").ok()) return 1;
    preview.stop();
    std::cout << "Explicit inputs, no fallback, typed failures, historical identity and nonblocking preview switching passed\n";
    return 0;
}
