#include "MySqlRepository.hpp"
#include "DeviceCalibration.hpp"
#include "DeviceCodec.hpp"
#include "InspectionService.hpp"
#include "DeviceConfigManager.hpp"
#include "CameraIdentity.hpp"
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
    auto fail=[](int line){std::cerr<<"Input acquisition check failed at line "<<line<<"\n";return 1;};
    DiscoveredCameraItem blank;
    if (deviceFromJson(QJsonObject()).hasCalibration || deviceToJson(blank).contains("calibration")) return fail(__LINE__);
    QJsonObject cal{{"perspective_correction_enabled",true}, {"calibrated_width",800}, {"calibrated_height",448},
        {"pt_0_x",400}, {"pt_0_y",20}, {"pt_1_x",700}, {"pt_1_y",224},
        {"pt_2_x",400}, {"pt_2_y",420}, {"pt_3_x",100}, {"pt_3_y",224},
        {"center_x",400}, {"center_y",224}, {"pointer_base_radius",47}};
    if (deviceFromJson(QJsonObject{{"calibration",cal}}).hasCalibration) return fail(__LINE__);
    cal["scale_points"]=QJsonArray{QJsonObject{{"x",200},{"y",350},{"ratio",0}},QJsonObject{{"x",600},{"y",350},{"ratio",1}}};
    cal["calibration_confirmed"]=true;
    QJsonObject obsoleteRegistry{{"system",QJsonObject{{"auto_connect_on_startup",true},{"last_active_device_id","old"}}},
        {"devices",QJsonArray{QJsonObject{{"camera_id","old"},{"is_auto_connect",true}}}}};
    auto cleanedRegistry=MySqlRepository::sanitizedRegistry(obsoleteRegistry);
    if (cleanedRegistry["system"].toObject().contains("auto_connect_on_startup") || cleanedRegistry["system"].toObject().contains("last_active_device_id") ||
        cleanedRegistry["devices"].toArray()[0].toObject().contains("is_auto_connect")) return fail(__LINE__);
    auto oldConnection=deviceFromJson(QJsonObject{{"camera_id","old"},{"is_auto_connect",true}});
    if (deviceToJson(oldConnection).contains("is_auto_connect")) return fail(__LINE__);
    auto calibrated = deviceFromJson(QJsonObject{{"calibration",cal}});
    if (!calibrated.hasCalibration || !deviceFromJson(deviceToJson(calibrated)).hasCalibration) return fail(__LINE__);
    auto invalid = cal; invalid.remove("pt_1_y");
    if (hasSavedDeviceCalibration(invalid)) return fail(__LINE__);
    invalid = cal; invalid["pt_2_x"] = 400; invalid["pt_2_y"] = 20;
    if (validDeviceCalibration(invalid)) return fail(__LINE__);
    invalid = cal; invalid["center_x"] = 900;
    if (validDeviceCalibration(invalid)) return fail(__LINE__);
    auto legacy=cal; legacy.remove("scale_points");
    if (hasSavedDeviceCalibration(legacy)) return fail(__LINE__);
    cal["calibration_confirmed"] = false;
    if (hasSavedDeviceCalibration(cal)) return fail(__LINE__);

    QTemporaryDir directory(QDir::currentPath() + "/capture-test-XXXXXX");
    if (!directory.isValid()) return fail(__LINE__);
    // Keep legacy local-registry synchronization away from repository configs.
    QDir().mkpath(directory.path() + "/work/deep");
    if (!QDir::setCurrent(directory.path() + "/work/deep")) return fail(__LINE__);
    if (!app.arguments().contains("--capture-only")) {
    // An empty registry must stay empty; local analysis has its own task identity.
    const auto registryPath = directory.path() + "/devices.json";
    QFile registry(registryPath);
    if (!registry.open(QIODevice::WriteOnly)) return fail(__LINE__);
    registry.write("{\"devices\":[],\"system\":{}}");
    registry.close();
    auto& manager = DeviceConfigManager::instance();
    if (!manager.load(registryPath) || !manager.getAllDevices().isEmpty()) { std::cerr << "Empty registry failed: " << manager.lastError().toStdString() << " devices=" << manager.getAllDevices().size() << "\n"; return fail(__LINE__); }
    QJsonArray legacyDevices;
    for (const auto& id : {"CAM_20", "CAM_5-1-2", "CAM_5-3-1", "CAM_5-3-4", "CAM_5-4-1"})
        legacyDevices.append(QJsonObject{{"camera_id", id}, {"protocol_type", "LOCAL_FILE"}});
    legacyDevices.append(QJsonObject{{"camera_id", "REAL_CAMERA"}, {"protocol_type", "ONVIF_RTSP_STREAM"}});
    legacyDevices.append(QJsonObject{{"camera_id", "USER_IMAGE"}, {"protocol_type", "LOCAL_FILE"}});
    if (!registry.open(QIODevice::WriteOnly | QIODevice::Truncate)) return fail(__LINE__);
    registry.write(QJsonDocument(QJsonObject{{"devices", legacyDevices},
        {"system", QJsonObject{{"last_active_device_id", "CAM_20"}}}}).toJson());
    registry.close();
    if (!manager.load(registryPath) || manager.getAllDevices().size() != 2) return fail(__LINE__);
    if (!registry.open(QIODevice::ReadOnly)) return fail(__LINE__);
    if (QJsonDocument::fromJson(registry.readAll()).object()["devices"].toArray().size() != 2) return fail(__LINE__);
    registry.close();
    if (!manager.load(registryPath) || manager.getAllDevices().size() != 2) return fail(__LINE__);
    // A later ONVIF scan must resolve to the existing physical camera archive.
    DiscoveredCameraItem configured;
    configured.cameraId = "CAM_IP_192_168_110_11";
    configured.hardwareId = "11111111-1111-4111-8111-111111111111";
    configured.rtspUrl = "rtsp://operator:secret@192.168.110.11:8554/vendor/path?channel=2";
    configured.ipOrSn = "192.168.110.11";
    configured.protocolType = CameraProtocolType::SDK_JLINK;
    configured.customName = "Configured gauge";
    configured.username = "operator";
    configured.password = "test-secret";
    configured.hasCalibration = configured.perspectiveEnabled = true;
    configured.calibratedWidth = 2000; configured.calibratedHeight = 1200;
    configured.pt0_x = 999; configured.pt0_y = 100;
    configured.pt1_x = 1500; configured.pt1_y = 600;
    configured.pt2_x = 999; configured.pt2_y = 1100;
    configured.pt3_x = 500; configured.pt3_y = 600;
    configured.centerY = 600;
    configured.centerX = 999;
    configured.scalePoints=QJsonArray{QJsonObject{{"x",650},{"y",950},{"ratio",0}},QJsonObject{{"x",1350},{"y",950},{"ratio",1}}};
    manager.addOrUpdateDevice(configured, false);
    auto discovered = configured;
    discovered.cameraId = "CAM_ONVIF_192_168_110_11";
    discovered.protocolType = CameraProtocolType::ONVIF_RTSP_STREAM;
    discovered.customName = "ONVIF camera";
    discovered.password.clear();
    discovered.hasCalibration = false;
    discovered.centerX = -1;
    const auto merged = manager.registerDiscoveredDevice(discovered, false);
    if (merged.cameraId != configured.cameraId || merged.centerX != 999 ||
        merged.password != configured.password || merged.customName != configured.customName ||
        merged.protocolType != configured.protocolType ||
        manager.getAllDevices().size() != 3) return fail(__LINE__);
    discovered.cameraId = "CAM_ONVIF_192_168_110_12";
    discovered.ipOrSn = "192.168.110.12";
    discovered.hardwareId = "22222222-2222-4222-8222-222222222222";
    manager.registerDiscoveredDevice(discovered, false);
    if (manager.getAllDevices().size() != 4) return fail(__LINE__);
    // IP changes update the endpoint, never the archive key or saved calibration.
    auto moved = configured;
    moved.ipOrSn = "10.10.10.21";
    moved.cameraId = "different-discovery-label";
    const auto relocated = manager.registerDiscoveredDevice(moved, false);
    if (relocated.cameraId != configured.cameraId || relocated.centerX != 999 ||
        relocated.password != configured.password || relocated.ipOrSn != moved.ipOrSn ||
        relocated.rtspUrl != "rtsp://operator:secret@10.10.10.21:8554/vendor/path?channel=2" ||
        manager.getAllDevices().size() != 4) return fail(__LINE__);
    auto unknown = moved;
    unknown.hardwareId.clear();
    if (!manager.registerDiscoveredDevice(unknown, false).cameraId.isEmpty()) return fail(__LINE__);
    // A different physical camera reusing an IP cannot inherit the original calibration.
    auto replacement = discovered;
    replacement.ipOrSn = moved.ipOrSn;
    const auto separate = manager.registerDiscoveredDevice(replacement, false);
    if (separate.cameraId == configured.cameraId || separate.hasCalibration) return fail(__LINE__);
    // Simulate a legacy registry containing both protocol identities.
    discovered.cameraId = "CAM_ONVIF_192_168_110_11";
    discovered.ipOrSn = configured.ipOrSn;
    discovered.hardwareId = configured.hardwareId;
    manager.addOrUpdateDevice(discovered, false);
    if (!manager.save(registryPath) || !manager.load(registryPath)) return fail(__LINE__);
    const auto restored = manager.getDevice(configured.cameraId);
    if (manager.getAllDevices().size() != 4 || manager.hasDevice(discovered.cameraId) ||
        restored.centerX != 999 || restored.password != configured.password ||
        restored.ipOrSn != moved.ipOrSn || restored.hardwareId != configured.hardwareId) return fail(__LINE__);
    QFile savedRegistry(registryPath);
    if (!savedRegistry.open(QIODevice::ReadOnly)) return fail(__LINE__);
    const auto savedRoot=QJsonDocument::fromJson(savedRegistry.readAll()).object();
    if (savedRoot["system"].toObject().contains("last_active_device_id") || savedRoot["system"].toObject().contains("auto_connect_on_startup")) return fail(__LINE__);
    for (const auto& value:savedRoot["devices"].toArray()) if(value.toObject().contains("is_auto_connect")) return fail(__LINE__);
    if (!manager.load(registryPath) || manager.getAllDevices().size() != 4) return fail(__LINE__);
    savedRegistry.close();
    int localFailures=0, mysqlFailures=0;
    QString failurePath, failureReason, failureStage;
    const auto localSignal=QObject::connect(&manager,&DeviceConfigManager::archiveSaveFailed,&app,
        [&](const QString& stage,const QString& path,const QString& reason) {
            ++localFailures; failureStage=stage; failurePath=path; failureReason=reason;
        });
    const auto dbSignal=QObject::connect(&manager,&DeviceConfigManager::databaseStatus,&app,
        [&](const QString& error) { if(!error.isEmpty()) ++mysqlFailures; });
    const auto blocked=directory.path()+"/blocked";
    QFile blocker(blocked); if(!blocker.open(QIODevice::WriteOnly)) return fail(__LINE__);
    blocker.write("block directory creation"); blocker.close();
    const auto blockedTarget=blocked+"/devices.json";
    if(manager.save(blockedTarget) || localFailures!=1 || mysqlFailures!=0 ||
       failurePath!=blockedTarget || failureReason.isEmpty() ||
       !failureStage.startsWith("LOCAL_ARCHIVE_") || !manager.lastError().contains(blockedTarget)) return fail(__LINE__);
    // A good retry clears stale errors and pins relative paths to their absolute location.
    if(!manager.save(registryPath) || !manager.lastError().isEmpty()) return fail(__LINE__);
#ifdef Q_OS_WIN
    // A Windows reader that holds the destination open prevents atomic replacement.
    QFile heldRegistry(registryPath);
    if(!heldRegistry.open(QIODevice::ReadOnly)) return fail(__LINE__);
    if(manager.save(registryPath) || failureStage!="LOCAL_ARCHIVE_COMMIT_FAILED" ||
       failurePath!=registryPath || failureReason.isEmpty() || mysqlFailures!=0) return fail(__LINE__);
    heldRegistry.close();
    manager.registerDiscoveredDevice(configured,true);
    if(manager.hasPendingRegistrySave() || !manager.lastError().isEmpty()) return fail(__LINE__);
#endif
    const auto oldWorkingDirectory=QDir::currentPath();
    if(!manager.save("relative-devices.json")) return fail(__LINE__);
    if(manager.getConfigPath()!=oldWorkingDirectory+"/relative-devices.json") return fail(__LINE__);
    if(!QDir::setCurrent(oldWorkingDirectory) || !manager.save() || !manager.lastError().isEmpty()) return fail(__LINE__);
    QObject::disconnect(localSignal); QObject::disconnect(dbSignal);

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
    if (stale.errorCode != "STALE_FRAME" || !stale.capturedFrame.empty() || !stale.originalPath.isEmpty()) return fail(__LINE__);
    task.device.cameraId = "first";
    auto live = run(task);
    if (!live.errorCode.isEmpty() || live.originalPath.isEmpty() || live.recognitionPerformed) return fail(__LINE__);
    const int beforeCalibrationCheck = acquisitions;
    auto uncalibrated = task;
    uncalibrated.captureOnly = false;
    auto rejectedCalibration = run(uncalibrated);
    if (rejectedCalibration.errorCode != "CALIBRATION_REQUIRED" || rejectedCalibration.recognitionPerformed ||
        !rejectedCalibration.originalPath.isEmpty() || acquisitions != beforeCalibrationCheck) return fail(__LINE__);
    // A local image attributed to an uncalibrated camera must not bypass the gate.
    uncalibrated.inputKind = InspectionInputKind::LocalImage;
    uncalibrated.localImagePath = live.originalPath;
    rejectedCalibration = run(uncalibrated);
    if (rejectedCalibration.errorCode != "CALIBRATION_REQUIRED" || acquisitions != beforeCalibrationCheck) return fail(__LINE__);
    uncalibrated.device.hasCalibration = true; // A flag without valid saved geometry is insufficient.
    rejectedCalibration = run(uncalibrated);
    if (rejectedCalibration.errorCode != "CALIBRATION_REQUIRED" || acquisitions != beforeCalibrationCheck) return fail(__LINE__);
    uncalibrated.device.protocolType=CameraProtocolType::LOCAL_FILE;
    rejectedCalibration=run(uncalibrated);
    if (rejectedCalibration.errorCode!="CALIBRATION_REQUIRED" || rejectedCalibration.recognitionPerformed || acquisitions!=beforeCalibrationCheck) return fail(__LINE__);
    uncalibrated.device=deviceFromJson(QJsonObject{{"calibration",legacy}});
    uncalibrated.device.cameraId="first";
    rejectedCalibration=run(uncalibrated);
    if (rejectedCalibration.errorCode!="CALIBRATION_REQUIRED" || rejectedCalibration.recognitionPerformed) return fail(__LINE__);
    const int initialAcquisitions = acquisitions;
    task.inputKind = InspectionInputKind::HistoricalImage;
    task.parentTaskId = live.taskId;
    task.originalCapturedAt = live.capturedAt;
    task.localImagePath = live.originalPath;
    const auto history = run(task);
    if (!history.errorCode.isEmpty() || history.parentTaskId != live.taskId || history.capturedAt != live.capturedAt || acquisitions != initialAcquisitions) return fail(__LINE__);
    if (cv::norm(live.capturedFrame, history.capturedFrame, cv::NORM_INF) != 0) return fail(__LINE__);
    auto uncalibratedHistory = task; uncalibratedHistory.captureOnly = false;
    const auto historyDenied = run(uncalibratedHistory);
    if (historyDenied.errorCode != "HISTORICAL_CALIBRATION_REQUIRED" || historyDenied.recognitionPerformed ||
        !historyDenied.originalPath.isEmpty() || acquisitions != initialAcquisitions) return fail(__LINE__);
    uncalibratedHistory.historicalCalibration=QJsonObject{{"calibration_confirmed",true}};
    const auto invalidHistoryCalibration=run(uncalibratedHistory);
    if (invalidHistoryCalibration.errorCode!="HISTORICAL_CALIBRATION_REQUIRED" || invalidHistoryCalibration.recognitionPerformed) return fail(__LINE__);
    QJsonObject historyCalibration{{"calibration_confirmed",true},{"perspective_correction_enabled",true},
        {"calibrated_width",24},{"calibrated_height",24},{"center_x",12},{"center_y",12},{"pointer_base_radius",2},
        {"scale_points",QJsonArray{QJsonObject{{"x",3},{"y",20},{"ratio",0}},QJsonObject{{"x",20},{"y",20},{"ratio",1}}}}};
    const double historyRim[8]={12,1,23,12,12,23,1,12};
    for(int i=0;i<4;++i) {historyCalibration[QString("pt_%1_x").arg(i)]=historyRim[i*2];historyCalibration[QString("pt_%1_y").arg(i)]=historyRim[i*2+1];}
    uncalibratedHistory.historicalCalibration=historyCalibration;
    uncalibratedHistory.historicalProfile=GaugeProfile::createDefault().toJson();
    const auto calibratedHistory=run(uncalibratedHistory);
    if (calibratedHistory.deviceSnapshot["calibration"].toObject()["center_x"].toDouble()!=12 ||
        calibratedHistory.errorCode=="CALIBRATION_REQUIRED" || calibratedHistory.errorCode=="HISTORICAL_CALIBRATION_REQUIRED" ||
        acquisitions!=initialAcquisitions || task.device.centerX==12) return fail(__LINE__);
    uncalibratedHistory.historicalCalibration["calibrated_width"]=25;
    const auto wrongHistorySize=run(uncalibratedHistory);
    if (wrongHistorySize.errorMessage!="HISTORICAL_CALIBRATION_DIMENSIONS_MISMATCH" || wrongHistorySize.recognitionPerformed ||
        !wrongHistorySize.originalPath.isEmpty()) return fail(__LINE__);
    task.parentTaskId = "wrong-task";
    const auto wrongIdentity = run(task);
    if (wrongIdentity.errorCode != "HISTORICAL_IDENTITY_MISMATCH" || !wrongIdentity.originalPath.isEmpty() || acquisitions != initialAcquisitions) return fail(__LINE__);
    task.parentTaskId = live.taskId;
    task.localImagePath = directory.path() + "/deleted.png";
    const auto missingHistory = run(task);
    if (missingHistory.errorCode != "IMAGE_READ_FAILED" || !missingHistory.originalPath.isEmpty() || acquisitions != initialAcquisitions) return fail(__LINE__);
    task.inputKind = InspectionInputKind::LocalImage;
    task.localImagePath = live.originalPath;
    task.parentTaskId.clear();
    const auto local = run(task);
    if (!local.errorCode.isEmpty() || local.inputKind != InspectionInputKind::LocalImage || acquisitions != initialAcquisitions) return fail(__LINE__);
    QFile metadata(directory.path() + "/" + local.taskId + "/result.json");
    if (!metadata.open(QIODevice::ReadOnly) || QJsonDocument::fromJson(metadata.readAll()).object()["input_kind"].toString() != "local_image") return fail(__LINE__);
    task.inputKind = InspectionInputKind::CameraSnapshot;
    task.device.protocolType = CameraProtocolType::LOCAL_FILE;
    const auto invalidLive = run(task);
    if (invalidLive.errorCode != "INVALID_INPUT" || acquisitions != initialAcquisitions) return fail(__LINE__);

    // Legacy APIs also cannot return stale caller buffers or sample/diagnostic images.
    if (!app.arguments().contains("--capture-only")) {
    cv::Mat frame(16, 16, CV_8UC3);
    FileCameraSource file(directory.path().toStdString() + "/missing.png");
    if (file.grabFrame(frame) || !frame.empty() || file.captureSnapshot().failure != CaptureFailure::OpenFailed) return fail(__LINE__);
    ONVIFDeviceInfo info;
    info.rtspUri.clear();
    ONVIFDeviceManager onvif(info);
    onvif.setSampleFrame(live.capturedFrame);
    frame = live.capturedFrame;
    if (onvif.grabFrame(frame) || !frame.empty() || onvif.readFrame().failure != CaptureFailure::NotOpen || onvif.captureSnapshot().failure != CaptureFailure::OpenFailed) return fail(__LINE__);
    SDKCameraSourceStub sdk("demo");
    sdk.setSampleFrame(live.capturedFrame);
    if (sdk.captureSnapshot().failure != CaptureFailure::UnsupportedProtocol || sdk.grabFrame(frame) || !frame.empty()) return fail(__LINE__);
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
    if (!received || oldDelivered || ticks < 2 || !TestSource::workerOnly.load()) return fail(__LINE__);
    if (preview.snapshot("first").ok()) return fail(__LINE__);
    auto snapshot = preview.snapshot("second");
    if (!snapshot.ok()) return fail(__LINE__);
    snapshot.frame.setTo(cv::Scalar(12, 34, 56));
    if (preview.snapshot("second").frame.at<cv::Vec3b>(0, 0)[0] != 90) return fail(__LINE__);
    task.device.protocolType = CameraProtocolType::ONVIF_RTSP_STREAM;
    task.previewSnapshot = snapshot.frame;
    task.previewCapturedAt = QDateTime::fromMSecsSinceEpoch(
        std::chrono::duration_cast<std::chrono::milliseconds>(snapshot.capturedAt.time_since_epoch()).count(), Qt::UTC);
    const auto capturedPreview = run(task);
    if (!capturedPreview.errorCode.isEmpty() || acquisitions != initialAcquisitions ||
        capturedPreview.capturedAt != task.previewCapturedAt ||
        cv::norm(capturedPreview.capturedFrame, snapshot.frame, cv::NORM_INF) != 0 ||
        cv::norm(cv::imread(capturedPreview.originalPath.toStdString()), snapshot.frame, cv::NORM_INF) != 0) return fail(__LINE__);
    task.previewCapturedAt = QDateTime::currentDateTimeUtc().addSecs(-3);
    const auto expiredPreview = run(task);
    if (expiredPreview.errorCode != "STALE_FRAME" || !expiredPreview.originalPath.isEmpty() ||
        acquisitions != initialAcquisitions) return fail(__LINE__);
    // Disconnect while a source is still opening: no late frame or cached snapshot may survive.
    task.device.cameraId = "first";
    preview.select(task);
    preview.select(InspectionTask());
    preview.requestFrame();
    if (preview.snapshot("second").ok()) return fail(__LINE__);
    received = false;
    QTimer::singleShot(160, &loop, &QEventLoop::quit);
    loop.exec();
    if (received || oldDelivered || preview.snapshot("first").ok()) return fail(__LINE__);
    // Disconnect is reversible; it must not permanently stop the preview backend.
    task.device.cameraId = "second";
    preview.select(task);
    timeout.start(5000);
    loop.exec();
    if (!received || !preview.snapshot("second").ok()) return fail(__LINE__);
    preview.stop();
    std::cout << "Explicit inputs, no fallback, typed failures, historical identity and nonblocking preview switching passed\n";
    return 0;
}
