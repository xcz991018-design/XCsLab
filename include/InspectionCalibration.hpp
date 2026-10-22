#pragma once
#include "InspectionService.hpp"
#include "DeviceCodec.hpp"
#include "DeviceCalibration.hpp"

inline bool canRecognizeDevice(const DiscoveredCameraItem& device) {
    return device.hasCalibration && hasSavedDeviceCalibration(deviceToJson(device)["calibration"].toObject());
}

inline QStringList recognitionMissingRequirements(const DiscoveredCameraItem& d) {
    QStringList missing;
    if (d.defaultProfileId.trimmed().isEmpty()) missing << QStringLiteral(u"请选择表计模板");
    if (d.installationLocation.trimmed().isEmpty() || d.deviceTag.trimmed().isEmpty())
        missing << QStringLiteral(u"请填写完整的设备安装档案");
    if (!canRecognizeDevice(d)) missing << QStringLiteral(u"请完成表计标定并保存");
    return missing;
}

inline bool requiresInspectionCalibration(const InspectionTask& task) {
    return !task.captureOnly && !canRecognizeDevice(task.device);
}

// Resolve the authoritative calibration/template from the original DB task.
inline bool usesHistoricalDatabaseConfiguration(const InspectionTask& task) {
    return task.inputKind == InspectionInputKind::HistoricalImage &&
        !task.databaseOriginalTaskId.isEmpty() && task.useHistoricalConfiguration;
}
