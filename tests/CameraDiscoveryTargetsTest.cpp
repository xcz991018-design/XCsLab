#include "CameraDiscoveryTargets.hpp"
#include <QCoreApplication>
#include <iostream>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    DiscoveredCameraItem saved;
    saved.cameraId = "CAM_ONVIF_192_168_110_11";
    saved.protocolType = CameraProtocolType::ONVIF_RTSP_STREAM;
    saved.ipOrSn = "192.168.110.11";
    saved.rtspPort = 8554;
    saved.rtspUrl = "rtsp://operator:private@192.168.110.11:8554/live";
    auto localImage = saved;
    localImage.cameraId = "LOCAL";
    localImage.protocolType = CameraProtocolType::LOCAL_FILE;
    localImage.ipOrSn = "sample.png";
    const QJsonObject credentials{{"devices", QJsonArray{deviceToJson(saved, true), deviceToJson(localImage, true)}}};
    const auto unloaded = cameraDiscoveryTargets({}, credentials);
    if (!unloaded.contains("192.168.110.11") || unloaded["192.168.110.11"] != QList<int>{8554} ||
        unloaded.contains("sample.png") || unloaded.size() != 1) return 1;
    const auto restored = cameraDiscoveryTargets({saved}, credentials);
    if (restored != unloaded) return 1;
    auto hostName = saved;
    hostName.ipOrSn = "camera.example";
    hostName.rtspUrl.clear();
    hostName.rtspPort = 554;
    const auto named = cameraDiscoveryTargets({hostName}, {});
    if (!named.contains("camera.example") || named["camera.example"] != QList<int>{554}) return 1;
    std::cout << "Saved camera addresses and RTSP ports remain discoverable while the DB registry is unavailable\n";
    return 0;
}
