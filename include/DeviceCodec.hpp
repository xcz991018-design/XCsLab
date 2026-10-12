#pragma once
#include "AutoCameraDiscoveryService.hpp"
#include <QJsonObject>
DiscoveredCameraItem deviceFromJson(const QJsonObject& obj);
QJsonObject deviceToJson(const DiscoveredCameraItem& item, bool includeSecrets = false);
