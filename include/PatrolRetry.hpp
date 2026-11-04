#pragma once
#include "InspectionService.hpp"
#include <cmath>

namespace PatrolRetry {
inline QString reason(const InspectionTask& task, const InspectionOutcome& out) {
    if (task.captureOnly || task.inputKind != InspectionInputKind::CameraSnapshot ||
        !task.allowLowerResolutionHttpSnapshot || out.captureMethod != "ONVIF_HTTP" ||
        task.device.protocolType != CameraProtocolType::ONVIF_RTSP_STREAM) return {};
    if (out.errorCode == "RECOGNITION_FAILED" || out.errorCode == "RECOGNITION_EXCEPTION")
        return "recognition_failed";
    if (!out.errorCode.isEmpty() || !out.recognitionPerformed) return {};
    if (!out.result.success) return "recognition_failed";
    if (!std::isfinite(out.result.confidence) || out.result.confidence < 0.4) return "low_confidence";
    if (!std::isfinite(out.result.value)) return "invalid_reading";
    if (out.result.centerInconsistent || !out.result.warningMessage.empty() ||
        out.result.recognitionQuality["status"].toString() != "trusted") return "quality_review";
    return {};
}

// Runs inside the existing bounded pipeline slot, never recursively retries.
template<class ReadHighResolution>
InspectionOutcome retry(const InspectionTask& task, InspectionOutcome first,
                        const std::shared_ptr<std::atomic<bool>>& cancel, ReadHighResolution read) {
    const auto why=reason(task,first);
    if (why.isEmpty() || cancel->load()) return first;
    auto hd=task;
    hd.preferOnvifSnapshot=false;
    hd.allowLowerResolutionHttpSnapshot=false;
    hd.requireOnvifSnapshot=true;
    hd.previewSnapshot.release();
    hd.previewCapturedAt={};
    if (!first.profileSnapshot.isEmpty()) hd.historicalProfile=first.profileSnapshot;
    const auto started=std::chrono::steady_clock::now();
    auto final=read(hd);
    final.timings["hd_retry"] = QJsonObject{
        {"reason",why}, {"attempts",1}, {"initial_capture_method",first.captureMethod},
        {"initial_success",first.result.success && first.errorCode.isEmpty()},
        {"initial_value",std::isfinite(first.result.value) && first.result.success ? QJsonValue(first.result.value) : QJsonValue()},
        {"initial_confidence",first.result.confidence}, {"initial_error",first.errorCode},
        {"initial_quality",first.result.recognitionQuality}, {"initial_timings",first.timings},
        {"elapsed_ms",std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count()},
        {"success",final.result.success && final.errorCode.isEmpty()}};
    return final;
}
}
