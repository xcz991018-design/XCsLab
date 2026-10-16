#include "SafeWorkingRange.hpp"
#include "DeviceCodec.hpp"
#include "MySqlRepository.hpp"
#include "EvidenceArchive.hpp"
#include <QCoreApplication>
#include <iostream>
#include <stdexcept>
#include <limits>

static void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        SafeWorkingRange range{true, 1.0, 2.0, 0.25, 0.5};
        require(range.valid() && range.lower() == 0.75 && range.upper() == 2.5, "Asymmetric limits incorrect");
        for (double value : {0.75, 1.0, 2.0, 2.5})
            require(range.assess(value,true)["status"] == "normal", "Inclusive bounds failed");
        for (double value : {0.7499, 2.5001})
            require(range.assess(value,true)["status"] == "out_of_range", "Unrounded abnormal reading missed");
        require(range.assess(3,false)["status"] == "not_evaluated", "Failed recognition caused an alarm");
        require(range.assess(std::numeric_limits<double>::quiet_NaN(),true)["status"] == "not_evaluated", "NaN alarm");
        require(range.assess(std::numeric_limits<double>::infinity(),true)["status"] == "not_evaluated", "Infinity alarm");
        const SafeWorkingRange decimalBoundary{true, 0.4, 0.5, 0.1, 0.1};
        require(decimalBoundary.assess(0.3,true)["status"] == "normal", "Decimal boundary arithmetic caused a false alarm");
        auto invalid = range; invalid.normalMin = 4;
        require(invalid.assess(3,true)["status"] == "invalid_configuration", "Reversed range accepted");
        invalid = range; invalid.allowanceMinus = -1;
        require(!invalid.valid(), "Negative allowance accepted");
        require(SafeWorkingRange::fromJson({{"enabled",true}}).assess(3,true)["status"] == "invalid_configuration", "Missing limits accepted");
        require(deviceFromJson({}).safeWorkingRange.enabled &&
            deviceFromJson({}).safeWorkingRange.assess(100,true)["status"] == "invalid_configuration", "Legacy defaults must be enabled but unconfigured");
        auto previousDefault = range.toJson(); previousDefault.remove("defaults_version"); previousDefault["enabled"] = false;
        const auto upgraded = SafeWorkingRange::fromJson(previousDefault);
        require(upgraded.enabled && upgraded.lower() == range.lower(), "Former opt-in default was not migrated");
        auto optedOut = range; optedOut.enabled = false;
        require(!SafeWorkingRange::fromJson(optedOut.toJson()).enabled, "Explicit opt-out did not survive reload");
        DiscoveredCameraItem camera; camera.cameraId = "RANGE_TEST"; camera.safeWorkingRange = range;
        const auto restored = deviceFromJson(deviceToJson(camera,true));
        require(restored.safeWorkingRange.toJson() == range.toJson(), "Range archive roundtrip failed");
        InspectionTask task; task.device = camera;
        InspectionOutcome out;
        out.result.value = 2.5001; out.result.success = true; out.result.confidence = 1;
        out.recognitionPerformed = true; out.result.recognitionQuality = {{"status","trusted"}};
        out.safetyAssessment = range.assess(out.result.value,true);
        require(EvidenceArchive::abnormal(out), "Unsafe reading not archived as abnormal");
        const auto envelope = MySqlRepository::inspectionEnvelope(task,out);
        require(envelope["device"].toObject()["safe_working_range"].toObject() == range.toJson(), "Threshold snapshot lost");
        require(envelope["result"].toObject()["safety_assessment"].toObject()["status"] == "out_of_range", "Assessment snapshot lost");
        camera.safeWorkingRange.normalMax = 100;
        require(envelope["result"].toObject()["safety_assessment"].toObject()["safe_max"].toDouble() == 2.5, "Historical snapshot changed");
        out.safetyAssessment = range.assess(1.5,true);
        require(!EvidenceArchive::abnormal(out), "Normal result archived as unsafe");
        std::cout << "Safe range boundaries, invalid inputs, raw precision, archives and immutable result snapshots passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << "\n"; return 1; }
}
