#pragma once
#include "InspectionService.hpp"
#include "DeviceCodec.hpp"
#include "DeviceCalibration.hpp"

inline bool canRecognizeDevice(const DiscoveredCameraItem& device) {
    return device.hasCalibration && hasSavedDeviceCalibration(deviceToJson(device)["calibration"].toObject());
}

inline bool requiresInspectionCalibration(const InspectionTask& task) {
    return !task.captureOnly && !canRecognizeDevice(task.device);
}

// Resolve the authoritative calibration/template from the original DB task.
inline bool usesHistoricalDatabaseConfiguration(const InspectionTask& task) {
    return task.inputKind == InspectionInputKind::HistoricalImage &&
        !task.databaseOriginalTaskId.isEmpty() && task.useHistoricalConfiguration;
}
