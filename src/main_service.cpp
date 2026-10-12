#include "InspectionService.hpp"
#include "DeviceConfigManager.hpp"
#include "MySqlRepository.hpp"
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QTimer>
#include <QTextStream>
#include <QFileInfo>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCommandLineParser parser;
    parser.setApplicationDescription("Headless gauge inspection runtime");
    parser.addHelpOption();
    parser.addOptions({
        QCommandLineOption("devices", "Device registry JSON", "path", "config/devices.json"),
        QCommandLineOption("profiles", "Gauge profile JSON", "path", "config/gauge_profile.json"),
        QCommandLineOption("output", "Evidence directory", "path", "inspection_snapshots"),
        QCommandLineOption("image", "Explicit image for LOCAL_FILE devices", "path"),
        QCommandLineOption("concurrency", "Maximum in-flight tasks", "count", "4"),
        QCommandLineOption("mode", "Run mode: production, evidence, debug", "mode", "production"),
        QCommandLineOption("interval", "Repeat interval in seconds (0: one pass)", "seconds", "0")
    });
    parser.process(app);
    const QString modeName = parser.value("mode");
    GaugeRunMode mode;
    if (modeName == "production") mode = GaugeRunMode::Production;
    else if (modeName == "evidence") mode = GaugeRunMode::Evidence;
    else if (modeName == "debug") mode = GaugeRunMode::Debug;
    else return 2;
    bool concurrencyOk, intervalOk;
    const int concurrency = parser.value("concurrency").toInt(&concurrencyOk);
    const int interval = parser.value("interval").toInt(&intervalOk);
    if (!concurrencyOk || concurrency < 1 || concurrency > 128 || !intervalOk || interval < 0 || interval > 2147483)
        return 2;
    auto& registry = DeviceConfigManager::instance();
    if ((!DatabaseSettings::fromEnvironment().enabled && !QFileInfo::exists(parser.value("devices"))) || !QFileInfo::exists(parser.value("profiles")))
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
        task.device = device;
        if (device.protocolType == CameraProtocolType::LOCAL_FILE)
            task.inputKind = InspectionInputKind::LocalImage;
        task.runMode = mode;
        task.profilePath = parser.value("profiles");
        task.localImagePath = parser.value("image");
        task.outputDirectory = parser.value("output");
        tasks.append(task);
    }
    InspectionService service(nullptr, concurrency);
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
                           << (out.errorCode.isEmpty() && out.result.success ? QString::number(out.result.value) : "--") << "\n";
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
