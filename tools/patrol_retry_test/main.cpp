#include "PatrolRetry.hpp"
#include <iostream>
#include <stdexcept>
static void check(bool ok) { if(!ok) throw std::runtime_error("retry assertion failed"); }
int main() {
    InspectionTask task;
    task.device.protocolType=CameraProtocolType::ONVIF_RTSP_STREAM;
    task.allowLowerResolutionHttpSnapshot=true;
    task.preferOnvifSnapshot=true;
    task.device.safeWorkingRange.normalMin=0;task.device.safeWorkingRange.normalMax=1;
    InspectionOutcome good;
    good.captureMethod="ONVIF_HTTP";good.recognitionPerformed=true;
    good.result.success=true;good.result.value=5;good.result.confidence=.8;
    good.result.recognitionQuality={{"status","trusted"}};
    // A real safety alarm alone must not trigger another exposure.
    check(PatrolRetry::reason(task,good).isEmpty());
    auto low=good;low.result.confidence=.3;
    check(PatrolRetry::reason(task,low)=="low_confidence");
    auto failed=good;failed.errorCode="RECOGNITION_FAILED";failed.result.success=false;
    check(PatrolRetry::reason(task,failed)=="recognition_failed");
    auto review=good;review.result.recognitionQuality["status"]="review";
    check(PatrolRetry::reason(task,review)=="quality_review");
    auto config=failed;config.errorCode="PROFILE_INVALID";
    check(PatrolRetry::reason(task,config).isEmpty());
    auto manual=task;manual.allowLowerResolutionHttpSnapshot=false;
    check(PatrolRetry::reason(manual,low).isEmpty());
    auto history=task;history.inputKind=InspectionInputKind::HistoricalImage;
    check(PatrolRetry::reason(history,low).isEmpty());
    auto rtsp=low;rtsp.captureMethod="ONVIF_RTSP";
    check(PatrolRetry::reason(task,rtsp).isEmpty());
    auto cancel=std::make_shared<std::atomic<bool>>(false);
    int reads=0;
    auto read=[&](const InspectionTask& hd) {
        ++reads;check(!hd.preferOnvifSnapshot && !hd.allowLowerResolutionHttpSnapshot && hd.requireOnvifSnapshot);
        check(hd.previewSnapshot.empty());
        auto out=rtsp;out.errorCode="CAPTURE_FAILED";out.result.success=false;return out;
    };
    auto result=PatrolRetry::retry(task,low,cancel,read);
    check(reads==1 && result.errorCode=="CAPTURE_FAILED");
    check(result.timings["hd_retry"].toObject()["attempts"].toInt()==1);
    PatrolRetry::retry(task,result,cancel,read);check(reads==1);
    PatrolRetry::retry(task,good,cancel,read);check(reads==1);
    cancel->store(true);PatrolRetry::retry(task,low,cancel,read);check(reads==1);
    std::cout<<"PASS: safety alarms excluded, quality triggers, RTSP policy, single retry, failure and cancellation\n";
}
