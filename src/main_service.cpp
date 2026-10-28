#include "InspectionService.hpp"
#include "IsolatedCapture.hpp"
#include "DeviceConfigManager.hpp"
#include "MySqlRepository.hpp"
#include "GaugeTemplateMigration.hpp"
#include "DeviceCredentialStore.hpp"
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QTimer>
#include <QTextStream>
#include <QFileInfo>
#include <QJsonArray>
#include "DeviceCodec.hpp"
#include "OnvifSoap.hpp"
#include "PointerAlgorithmSettings.hpp"
#include <QFile>
#include <QJsonDocument>

int main(int argc, char** argv) {
    const int workerExit = runCaptureWorker(argc, argv);
    if (workerExit >= 0) return workerExit;
    QCoreApplication app(argc, argv);
    QCommandLineParser parser;
    parser.setApplicationDescription("Headless gauge inspection runtime");
    parser.addHelpOption();
    parser.addOptions({
        QCommandLineOption("export-algorithm-parameters", "Initialize independent pointer algorithm JSON from current database templates"),
        QCommandLineOption("sync-camera-clock", "Correct a registered camera to computer UTC when offset exceeds 5 seconds, then verify", "camera-id"),
        QCommandLineOption("diagnose-snapshot", "Offline replay using an exported snapshot.json and --image", "path"),
        QCommandLineOption("diagnose-task", "Read-only replay of a historical DB task; exports original, frozen config and diagnostics", "task-id"),
        QCommandLineOption("devices", "Explicit credential/registry JSON override (default: user data directory)", "path"),
        QCommandLineOption("credential-path", "Print the shared user credential path without opening a camera"),
        QCommandLineOption("migrate-config", "Migrate legacy JSON directory to user data and verify backups; no DB writes", "directory"),
        QCommandLineOption("sync-credential-cache", "Add DB archive keys to the local credential cache without changing DB data"),
        QCommandLineOption("audit-device-storage", "Print archive/template checksums without displaying credentials"),
        QCommandLineOption("import-templates", "One-time legacy template import into MySQL; exits without inspection", "path"),
        QCommandLineOption("check-database", "Check database template and device references without capture"),
        QCommandLineOption("output", "Evidence directory", "path", "inspection_snapshots"),
        QCommandLineOption("image", "Explicit image for LOCAL_FILE devices", "path"),
        QCommandLineOption("concurrency", "Maximum in-flight tasks", "count", "4"),
        QCommandLineOption("capture-timeout", "Whole network acquisition deadline in milliseconds", "ms", "8000"),
        QCommandLineOption("mode", "Diagnostic replay rendering: production, evidence, debug (inspection uses unified image retention)", "mode", "evidence"),
        QCommandLineOption("interval", "Repeat interval in seconds (0: one pass)", "seconds", "0")
    });
    parser.process(app);
    if (parser.isSet("export-algorithm-parameters")) {
        try {
            const auto templates=MySqlRepository().loadTemplates();
            QTextStream(stdout) << "Templates: " << templates.size() << "; algorithm JSON: " << PointerAlgorithmSettings::path() << "\n";
            return 0;
        } catch(const std::exception& e) { QTextStream(stderr) << e.what() << "\n"; return 2; }
    }
    if (parser.isSet("sync-camera-clock")) {
        try {
            const auto archive = MySqlRepository().loadDevice(parser.value("sync-camera-clock"));
            QJsonObject local; QString error;
            if (!DeviceCredentialStore::readRegistry(DeviceCredentialStore::defaultPath(),local,error))
                throw std::runtime_error(error.toStdString());
            const auto device = deviceFromJson(DeviceCredentialStore::withCredentials(deviceToJson(archive),local["devices"].toArray()));
            if (device.protocolType != CameraProtocolType::ONVIF_RTSP_STREAM)
                throw std::runtime_error("Camera is not ONVIF");
            QUrl endpoint; endpoint.setScheme("http"); endpoint.setHost(device.ipOrSn);
            endpoint.setPort(device.port); endpoint.setPath("/onvif/device_service");
            const auto before = OnvifSoap::readClock(endpoint,device.username,device.password,error);
            if (!before.utc.isValid()) throw std::runtime_error(error.toStdString());
            const auto beforeDelta = QDateTime::currentDateTimeUtc().msecsTo(before.utc);
            error = OnvifSoap::synchronizeClock(endpoint,device.username,device.password,true);
            if (!error.isEmpty()) throw std::runtime_error(error.toStdString());
            const auto after = OnvifSoap::readClock(endpoint,device.username,device.password,error);
            if (!after.utc.isValid()) throw std::runtime_error(error.toStdString());
            const auto afterDelta = QDateTime::currentDateTimeUtc().msecsTo(after.utc);
            QTextStream(stdout) << "Camera: " << device.cameraId << "; previous mode: " << before.type
                << "; previous offset(ms): " << beforeDelta << "; mode: " << after.type
                << "; verified offset(ms): " << afterDelta << "; UTC: " << after.utc.toString(Qt::ISODate) << "\n";
            return std::llabs(afterDelta) <= 5000 ? 0 : 2;
        } catch (const std::exception& e) { QTextStream(stderr) << e.what() << "\n"; return 2; }
    }
    if (parser.isSet("diagnose-task") || parser.isSet("diagnose-snapshot")) {
        try {
            StoredImage stored;
            if (parser.isSet("diagnose-snapshot")) {
                QFile config(parser.value("diagnose-snapshot")), image(parser.value("image"));
                if (!config.open(QIODevice::ReadOnly) || !image.open(QIODevice::ReadOnly))
                    throw std::runtime_error("Cannot read offline snapshot/image");
                QJsonParseError error;
                const auto root = QJsonDocument::fromJson(config.readAll(),&error).object();
                if (error.error != QJsonParseError::NoError || !root.contains("device") || !root.contains("profile"))
                    throw std::runtime_error("Invalid offline snapshot");
                stored.inspection.device = deviceFromJson(root["device"].toObject());
                stored.inspection.profile = root["profile"].toObject();
                stored.inspection.resultSnapshot = root["stored_result"].toObject();
                stored.bytes = image.readAll();
            } else stored = MySqlRepository().original(parser.value("diagnose-task"));
            const auto& d = stored.inspection.device;
            GaugeProfile profile;
            if (!profile.loadFromJson(stored.inspection.profile, d.defaultProfileId.toStdString()))
                throw std::runtime_error("Invalid historical template");
            profile.gauge_id = d.cameraId.toStdString();
            profile.perspective_correction_enabled = d.perspectiveEnabled;
            profile.perspective_pts = {cv::Point2f(d.pt0_x,d.pt0_y),cv::Point2f(d.pt1_x,d.pt1_y),cv::Point2f(d.pt2_x,d.pt2_y),cv::Point2f(d.pt3_x,d.pt3_y)};
            profile.center = cv::Point2f(d.centerX,d.centerY);
            profile.scalePoints = d.scalePoints;
            profile.pointerBaseRadius = d.pointerBaseRadius;
            profile.calibrated_width = d.calibratedWidth; profile.calibrated_height = d.calibratedHeight;
            profile.auto_detect_roi = false; profile.save_debug_images = false;
            const auto frame = cv::imdecode(std::vector<uchar>(stored.bytes.begin(),stored.bytes.end()), cv::IMREAD_COLOR);
            QDir dir(parser.value("output"));
            if (!dir.mkpath(".")) throw std::runtime_error("Cannot create output directory");
            const auto write = [&](const QString& name, const QByteArray& bytes) {
                QFile f(dir.filePath(name));
                if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size()) throw std::runtime_error("Cannot export diagnostic file");
            };
            write("original.jpg", stored.bytes);
            write("snapshot.json", QJsonDocument(QJsonObject{{"device",deviceToJson(d)}, {"profile",stored.inspection.profile},
                {"stored_result",stored.inspection.resultSnapshot}}).toJson());
            const auto modeName = parser.value("mode");
            if (modeName != "production" && modeName != "evidence" && modeName != "debug")
                throw std::runtime_error("Invalid diagnostic run mode");
            const auto mode = modeName == "production" ? GaugeRunMode::Production :
                modeName == "evidence" ? GaugeRunMode::Evidence : GaugeRunMode::Debug;
            const auto result = GaugeReaderEngine().process(frame,profile,mode);
            if (!result.step2_rectified.empty()) cv::imwrite(dir.filePath("rectified.png").toStdString(),result.step2_rectified);
            if (!result.overlayImage.empty()) cv::imwrite(dir.filePath("overlay.png").toStdString(),result.overlayImage);
            if (!result.diagnosticHudImage.empty()) cv::imwrite(dir.filePath("hud.png").toStdString(),result.diagnosticHudImage);
            const auto& r=result.pointerDetectorResult;
            const auto body=[](const RadialDirectionProfile& p) { return QJsonObject{{"reach",p.reach},{"width",p.medianWidth},
                {"sections",p.validSectionCount},{"coverage_length",p.validLength},{"score",p.bodyScore}}; };
            const auto tail=[](const TailDirectionProfile& p) { return QJsonObject{{"valid",p.valid},{"reach",p.reach},
                {"width",p.medianWidth},{"slope",p.widthSlope},{"widening",p.wideningScore},{"score",p.tailScore}}; };
            QJsonObject report{{"task_id",stored.inspection.taskId},{"mode",modeName},{"success",result.success},{"value",result.value},
                {"angle",result.pointer_angle_deg},{"head",QString(QChar(r.headEnd))}, {"likelihood_a",r.tipLikelihoodA},
                {"likelihood_b",r.tipLikelihoodB},{"body_a",body(r.directionA)},{"body_b",body(r.directionB)},
                {"tail_a",tail(r.tailA)},{"tail_b",tail(r.tailB)},{"error",QString::fromStdString(result.errorMessage)}};
            write("report.json",QJsonDocument(report).toJson());
            write("diagnostics.txt",QByteArray::fromStdString(result.detailedDebugInfo));
            QTextStream(stdout) << QJsonDocument(report).toJson();
            return 0;
        } catch (const std::exception& error) { QTextStream(stderr) << error.what() << "\n"; return 2; }
    }
    if (parser.isSet("credential-path")) {
        QTextStream(stdout) << DeviceCredentialStore::defaultPath() << "\n"; return 0;
    }
    if (parser.isSet("migrate-config")) {
        QDir directory(parser.value("migrate-config"));
        if (!directory.exists()) { QTextStream(stderr) << "Legacy directory does not exist\n"; return 2; }
        QStringList archives;
        for (const auto& file : directory.entryInfoList({"*.json"}, QDir::Files)) archives.append(file.absoluteFilePath());
        auto sources = DeviceCredentialStore::legacyPaths(); sources.append(directory.absoluteFilePath("devices.json"));
        QString error;
        if (!DeviceCredentialStore::migrate(sources,archives,error,DeviceCredentialStore::defaultPath(),true)) {
            QTextStream(stderr) << error << "\n"; return 2;
        }
        QJsonObject root;
        if (!DeviceCredentialStore::readRegistry(DeviceCredentialStore::defaultPath(),root,error)) return 2;
        QTextStream(stdout) << "Credential store: " << DeviceCredentialStore::defaultPath()
            << "; devices: " << root["devices"].toArray().size() << "; legacy JSON backups: " << archives.size() << "\n";
        return 0;
    }
    if (parser.isSet("sync-credential-cache")) {
        try {
            QString error; QJsonObject local;
            if (!DeviceCredentialStore::readRegistry(DeviceCredentialStore::defaultPath(),local,error)) {
                QTextStream(stderr) << error << "\n"; return 2;
            }
            const auto snapshot = local["devices"].toArray();
            auto records = snapshot;
            QSet<QString> ids;
            for (const auto& value : records) ids.insert(value.toObject()["camera_id"].toString());
            int added = 0;
            for (const auto& value : MySqlRepository().loadRegistry()["devices"].toArray()) {
                const auto device = value.toObject();
                if (!ids.contains(device["camera_id"].toString())) {
                    // Use exactly the effective credentials of the previous loader, not an IP match.
                    records.append(DeviceCredentialStore::withCredentials(device,snapshot)); ++added;
                }
            }
            if (!DeviceCredentialStore::archiveFile(DeviceCredentialStore::defaultPath(),DeviceCredentialStore::defaultPath(),error)) return 2;
            local["devices"] = records;
            if (!DeviceCredentialStore::atomicWrite(DeviceCredentialStore::defaultPath(),QJsonDocument(local).toJson(QJsonDocument::Indented),error)) {
                QTextStream(stderr) << error << "\n"; return 2;
            }
            QTextStream(stdout) << "Added database credential cache entries: " << added << "\n";
            return 0;
        } catch (const std::exception& error) { QTextStream(stderr) << error.what() << "\n"; return 2; }
    }
    if (parser.isSet("audit-device-storage")) {
        try {
            MySqlRepository repository;
            const auto registry = repository.loadRegistry();
            QJsonArray templates;
            for (const auto& record : repository.loadTemplates()) templates.append(record.definition);
            const auto digest = [](const QJsonObject& object) { return QCryptographicHash::hash(QJsonDocument(object).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex(); };
            QJsonObject credentials; QString readError;
            int matched = 0;
            if (QFileInfo::exists(DeviceCredentialStore::defaultPath()) &&
                !DeviceCredentialStore::readRegistry(DeviceCredentialStore::defaultPath(),credentials,readError)) {
                QTextStream(stderr) << readError << "\n"; return 2;
            }
            QJsonArray connections;
            QSet<QString> localIds;
            for (const auto& value : credentials["devices"].toArray()) localIds.insert(value.toObject()["camera_id"].toString());
            for (const auto& value : registry["devices"].toArray()) {
                const auto device = value.toObject();
                if (localIds.contains(device["camera_id"].toString())) ++matched;
                const auto effective = DeviceCredentialStore::withCredentials(device,credentials["devices"].toArray());
                QJsonObject connection;
                for (const auto* key : {"camera_id", "hardware_id", "username", "password", "ip_or_sn", "rtsp_url", "port", "rtsp_port"})
                    connection[key] = effective[key];
                connections.append(connection);
            }
            QTextStream(stdout) << "Connections SHA256: " << digest(QJsonObject{{"connections",connections}}) << "\n";
            for (const auto& value : registry["devices"].toArray()) {
                const auto device = value.toObject();
                const QJsonObject identity{{"camera_id",device["camera_id"]},{"hardware_id",device["hardware_id"]},
                    {"credential_ref",device["credential_ref"]},{"address",device["ip_or_sn"]},
                    {"has_calibration",device.contains("calibration")}};
                QTextStream(stdout) << "DB identity: " << QJsonDocument(identity).toJson(QJsonDocument::Compact) << "\n";
            }
            QTextStream(stdout) << "Registry SHA256: " << digest(registry) << "; templates SHA256: "
                << digest(QJsonObject{{"templates",templates}}) << "; devices: " << registry["devices"].toArray().size()
                << "; local credential matches: " << matched << "\n";
            return 0;
        } catch (const std::exception& error) { QTextStream(stderr) << error.what() << "\n"; return 2; }
    }
    if (parser.isSet("check-database")) {
        try {
            MySqlRepository repository;
            const auto templates = repository.loadTemplates();
            const auto registry = repository.loadRegistry();
            QStringList ids;
            for (const auto& record : templates) ids.append(record.id);
            int missing = 0;
            for (const auto& value : registry["devices"].toArray()) {
                const auto object = value.toObject();
                const auto id = object["default_profile_id"].toString();
                if (!id.isEmpty()&&!ids.contains(id)) {
                    ++missing;
                    QTextStream(stderr) << "Missing template: " << id << "\n";
                }
            }
            QTextStream(stdout) << "Database templates: " << templates.size() << "; archived devices: "
                << registry["devices"].toArray().size() << "; missing template references: " << missing << "\n";
            return missing ? 2 : 0;
        } catch (const std::exception& error) { QTextStream(stderr) << error.what() << "\n"; return 2; }
    }
    if (parser.isSet("import-templates")) {
        try {
            const auto definitions = materializeLegacyGaugeTemplates(parser.value("import-templates"));
            const int imported = MySqlRepository().importTemplates(definitions);
            QTextStream(stdout) << "Imported " << imported << " database templates; existing templates unchanged.\n";
            return 0;
        } catch (const std::exception& error) { QTextStream(stderr) << error.what() << "\n"; return 2; }
    }
    const auto mode = GaugeRunMode::Evidence; // Unified inspection rendering and image retention.
    bool concurrencyOk, intervalOk, captureTimeoutOk;
    const int captureTimeout = parser.value("capture-timeout").toInt(&captureTimeoutOk);
    const int concurrency = parser.value("concurrency").toInt(&concurrencyOk);
    const int interval = parser.value("interval").toInt(&intervalOk);
    if (!captureTimeoutOk || captureTimeout < 100 || captureTimeout > 120000 ||
        !concurrencyOk || concurrency < 1 || concurrency > 128 || !intervalOk || interval < 0 || interval > 2147483)
        return 2;
    auto& registry = DeviceConfigManager::instance();
    if (!DatabaseSettings::fromEnvironment().enabled)
        return 2;
    if (!registry.load(parser.value("devices"))) {
        QTextStream(stderr) << registry.lastError() << "\n";
        return 2;
    }
    const auto devices = registry.getAllDevices();
    if (devices.isEmpty()) return 2;
    QList<InspectionTask> tasks;
    for (const auto& device : devices) {
        InspectionTask task;
        task.captureTimeoutMs = captureTimeout;
        task.device = device;
        task.preferOnvifSnapshot = true;
        if (device.protocolType == CameraProtocolType::LOCAL_FILE)
            task.inputKind = InspectionInputKind::LocalImage;
        task.runMode = mode;
        task.localImagePath = parser.value("image");
        task.outputDirectory = parser.value("output");
        tasks.append(task);
    }
    InspectionService service(nullptr, concurrency);
    QObject::connect(&service, &InspectionService::evidenceMaintenanceFinished, &app,
        [](const QString& report) { QTextStream(stdout)<<"[STORAGE] "<<report<<"\n"; });
    QTimer syncTimer;
    if (DatabaseSettings::fromEnvironment().enabled) {
        QObject::connect(&syncTimer, &QTimer::timeout, &service, [&]() { service.retryDatabaseWrites(parser.value("output")); });
        syncTimer.start(30000);
        service.retryDatabaseWrites(parser.value("output"));
    }
    bool failed = false;
    QObject::connect(&service, &InspectionService::taskFinished, &app, [&](const InspectionOutcome& out) {
        failed |= !out.errorCode.isEmpty() || !out.result.success;
        if (DatabaseSettings::fromEnvironment().enabled && !out.databaseSaved) {
            failed = true;
            QTextStream(stderr) << "MYSQL_PENDING " << out.taskId << " " << out.databaseError << "\n";
        }
        QTextStream(stdout) << out.cameraId << " " << out.errorCode << " "
                           << (out.errorCode.isEmpty() && out.result.success ? QString::number(out.result.value, 'f', 3) : "--") << "\n";
    });
    QObject::connect(&service, &InspectionService::finished, &app, [&](bool cancelled) {
        if (!interval) app.exit(failed || cancelled ? 1 : 0);
        else QTimer::singleShot(interval * 1000, &app, [&]() { failed = false; service.start(tasks); });
    });
    QTimer::singleShot(0, &app, [&]() { service.start(tasks); });
    const auto exitCode = app.exec();
    registry.waitForDatabaseWrites();
    return exitCode;
}
