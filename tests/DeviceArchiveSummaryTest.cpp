#include "DeviceArchiveSummary.hpp"
#include <QCoreApplication>
#include <iostream>

int main(int argc, char** argv) {
    QCoreApplication app(argc,argv);
    DiscoveredCameraItem camera;
    camera.cameraId = "ARCHIVE_1"; camera.customName = QStringLiteral("一号压力表");
    camera.hardwareId = "11111111-1111-4111-8111-111111111111";
    camera.ipOrSn = "10.1.1.23"; camera.protocolName = "ONVIF"; camera.port = 8080; camera.rtspPort = 8554;
    camera.rtspUrl = "rtsp://embeddedUser:secret@10.1.1.23:8554/vendor/stream";
    camera.username = "operator"; camera.password = "archivePassword";
    camera.substation = QStringLiteral("厂区一"); camera.area = QStringLiteral("高压室");
    camera.installationLocation = QStringLiteral("东侧"); camera.deviceTag = "P-001";
    camera.defaultProfileId = "PRESSURE_CUSTOM"; camera.gaugeType = QStringLiteral("压力表");
    camera.manufacturer = QStringLiteral("生产厂家"); camera.modelName = "MODEL-100"; camera.accuracyClass = "1.6";
    camera.lastSeenTime = "2026-10-16 12:00:00";
    camera.safeWorkingRange = {true,0.5,1.0,0.1,0.2};
    const auto summary = DeviceArchiveSummary::fromDevice(camera);
    const QString all = summary.identity + summary.hardware + summary.network + summary.location + summary.specification + summary.safety + summary.calibration;
    for (const auto& field : {camera.cameraId, camera.customName, camera.hardwareId, camera.ipOrSn, camera.protocolName,
        camera.username, camera.substation, camera.area, camera.installationLocation, camera.deviceTag,
        camera.defaultProfileId, camera.gaugeType, camera.manufacturer, camera.modelName, camera.accuracyClass, camera.lastSeenTime})
        if (!all.contains(field)) { std::cerr << "Archive field missing"; return 1; }
    if (!summary.network.contains("8080") || !summary.network.contains("8554") ||
        !summary.safety.contains("0.400000") || !summary.safety.contains("1.200000")) return 1;
    if (all.contains("secret") || all.contains("embeddedUser") || all.contains("archivePassword")) return 1;
    const auto empty = DeviceArchiveSummary::fromDevice(DiscoveredCameraItem{});
    if (!empty.location.contains(QStringLiteral("未填写")) || !empty.calibration.contains(QStringLiteral("未标定"))) return 1;
    const auto second = DeviceArchiveSummary::fromDevice(DiscoveredCameraItem{});
    if (second.identity.contains(camera.cameraId) || second.network.contains(camera.ipOrSn)) return 1;
    std::cout << "Device archive fields, missing metadata, safety ranges and credential redaction passed\n";
    return 0;
}
