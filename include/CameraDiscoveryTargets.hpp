#pragma once
#include "AutoCameraDiscoveryService.hpp"
#include "DeviceCodec.hpp"
#include <QJsonArray>
#include <QMap>
#include <QUrl>

// Local credentials contribute probe addresses only. They never become device
// archives unless a real camera responds and the database registry is ready.
inline QMap<QString, QList<int>> cameraDiscoveryTargets(const QList<DiscoveredCameraItem>& known,
                                                       const QJsonObject& credentials) {
    QMap<QString, QList<int>> targets;
    auto append = [&](const DiscoveredCameraItem& camera) {
        if (camera.protocolType == CameraProtocolType::LOCAL_FILE) return;
        const QUrl uri(camera.rtspUrl);
        const auto host = uri.host().isEmpty() ? camera.ipOrSn.trimmed() : uri.host();
        const int defaultPort = camera.rtspPort > 0 ? camera.rtspPort : 554;
        const int port = camera.rtspUrl.isEmpty() ? defaultPort : uri.port(defaultPort);
        if (host.isEmpty() || port <= 0 || port > 65535) return;
        if (!targets[host].contains(port)) targets[host].append(port);
    };
    for (const auto& camera : known) append(camera);
    for (const auto& value : credentials["devices"].toArray()) append(deviceFromJson(value.toObject()));
    return targets;
}
