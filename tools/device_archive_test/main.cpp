#include "InspectionCalibration.hpp"
#include "DeviceArchiveSummary.hpp"
#include <iostream>
#include <stdexcept>

static void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

int main() {
    try {
        const SafeWorkingRange bounds{true, 0.2, 0.8};
        require(bounds.assess(0.2,true)["status"]=="normal" && bounds.assess(0.8,true)["status"]=="normal",
                "normal range must include both endpoints");
        require(bounds.assess(0.19,true)["status"]=="out_of_range" && bounds.assess(0.81,true)["status"]=="out_of_range",
                "readings beyond the normal bounds must be abnormal");
        auto oldRange=bounds.toJson(); oldRange["allowance_minus"]=0.1; oldRange["allowance_plus"]=0.1;
        const auto loaded=SafeWorkingRange::fromJson(oldRange);
        require(loaded.lower()==0.2 && loaded.upper()==0.8 && loaded.assess(0.85,true)["status"]=="out_of_range",
                "legacy allowances must not expand the normal range");
        require(!loaded.toJson().contains("allowance_minus") && !loaded.toJson().contains("allowance_plus"),
                "saved settings must omit allowances");
        require(!SafeWorkingRange{true,0.8,0.2}.valid(),"reversed bounds must remain invalid");
        DiscoveredCameraItem device;
        device.defaultProfileId = "selected";
        device.meterRange={{"min_val",-.1},{"max_val",1.6},{"unit","MPa"}};
        device.defaultProfileId.clear();
        require(!recognitionMissingRequirements(device).contains(QStringLiteral(u"请识别并保存表计量程")),"OCR range removes template selection requirement");
        auto snapshot=OcrDialRange::withRange(QJsonObject{{"profiles",QJsonArray{QJsonObject{{"gauge_id","old"},{"min_val",0},{"max_val",10}}}}},device.meterRange);
        require(snapshot["profiles"].toArray()[0].toObject()["max_val"].toDouble()==1.6,"frozen profile must use OCR range instead of template range");
        device.hasCalibration=true;device.perspectiveEnabled=true;device.calibratedWidth=device.calibratedHeight=800;
        device.pt0_x=400;device.pt0_y=40;device.pt1_x=760;device.pt1_y=400;
        device.pt2_x=400;device.pt2_y=760;device.pt3_x=40;device.pt3_y=400;
        device.centerX=device.centerY=400;
        device.scalePoints=QJsonArray{QJsonObject{{"x",200},{"y",600},{"ratio",0}},QJsonObject{{"x",600},{"y",600},{"ratio",1}}};
        const auto rangeRestored=deviceFromJson(deviceToJson(device));
        require(rangeRestored.meterRange==device.meterRange&&rangeRestored.hasCalibration,"OCR range must survive archive codec round-trip");
        auto invalid=deviceToJson(device)["calibration"].toObject();invalid["meter_range"]=QJsonObject{{"min_val",1},{"max_val",0}};
        require(!validDeviceCalibration(invalid),"invalid saved range rejected");
        device.installationLocation = "point";
        device.deviceTag = "tag";
        device.gaugeType.clear(); device.manufacturer.clear();
        device.modelName.clear(); device.accuracyClass.clear();
        const auto archiveWarning = QStringLiteral(u"请填写完整的设备安装档案");
        require(!recognitionMissingRequirements(device).contains(archiveWarning),
                "optional manufacturer/position and removed specification fields must not prevent recognition");
        device.installationPosition = "upper cabinet";
        auto restored = deviceFromJson(deviceToJson(device));
        require(restored.installationPosition == device.installationPosition &&
                restored.installationLocation == "point" && restored.deviceTag == "tag",
                "installation point, tag and optional position must round-trip");
        auto legacy = deviceToJson(device); legacy.remove("installation_position");
        restored = deviceFromJson(legacy);
        require(restored.installationPosition.isEmpty() && restored.installationLocation == "point",
                "old archives must retain their point and default optional position to empty");
        device.installationLocation.clear();
        require(recognitionMissingRequirements(device).contains(archiveWarning), "point remains mandatory");
        device.installationLocation = "point"; device.deviceTag.clear();
        require(recognitionMissingRequirements(device).contains(archiveWarning), "tag remains mandatory");
        device.deviceTag = "tag"; device.substation = "retired-station"; device.area = "retired-area";
        const auto summary = DeviceArchiveSummary::fromDevice(device).location;
        require(summary.contains("point") && summary.contains("tag") && summary.contains("upper cabinet") &&
                !summary.contains("retired-"), "installation summary must show only the three current fields");
        std::cout << "PASS: archive round-trip, legacy compatibility, optional position, required point/tag, summary\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
