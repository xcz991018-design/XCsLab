#include "GaugeReaderEngine.hpp"
#include "DeviceCodec.hpp"
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <iostream>
#include <stdexcept>
static void require(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    try {
        const QString base=QString(REPO_SOURCE_DIR)+"/tests/fixtures/";
        QFile file(base+"pointer_shadow.json"); require(file.open(QIODevice::ReadOnly),"Fixture unavailable");
        const auto record=QJsonDocument::fromJson(file.readAll()).object();
        const auto device=deviceFromJson(record["device"].toObject());
        const auto frame=cv::imread((base+"pointer_shadow.png").toStdString()); require(!frame.empty(),"Fixture image unavailable");
        GaugeProfile profile; require(profile.loadFromJson(record["profile_snapshot"].toObject(),device.defaultProfileId.toStdString()),"Fixture profile invalid");
        profile.perspective_correction_enabled=true; profile.auto_detect_roi=false;
        profile.perspective_pts={{float(device.pt0_x),float(device.pt0_y)},{float(device.pt1_x),float(device.pt1_y)},
            {float(device.pt2_x),float(device.pt2_y)},{float(device.pt3_x),float(device.pt3_y)}};
        profile.center={float(device.centerX),float(device.centerY)};
        profile.scalePoints=device.scalePoints; profile.calibrated_width=device.calibratedWidth;
        profile.calibrated_height=device.calibratedHeight; profile.pointerBaseRadius=device.pointerBaseRadius;
        profile.save_debug_images=false;
        GaugeReaderEngine engine;
        const auto invalid=engine.process(frame,profile,GaugeRunMode::Evidence);
        require(!invalid.success&&invalid.centerInconsistent&&invalid.centerOffsetDist>40,
            "Displaced hub still produced a shadow reading");
        require(invalid.errorMessage.find("中心")!=std::string::npos&&!invalid.step4_recognition.empty(),"Missing calibration diagnostic");
        cv::Mat inverse; const auto rectified=engine.rectifyPerspective(frame,profile.perspective_pts,inverse);
        cv::Mat gray; cv::cvtColor(rectified,gray,cv::COLOR_BGR2GRAY);
        PointerDetectorConfig config; config.pointerBaseRadius=47; config.minCommonLengthPx=120;
        PointerDetector detector(config);
        std::vector<cv::Point2f> wrongHub{profile.center}; cv::perspectiveTransform(wrongHub,wrongHub,inverse.inv());
        require(!detector.detect(gray,wrongHub[0]).success,"Long gray shadow remained a valid needle body");
        // Independently located circular pin, transformed back into the 2304x1296 source frame.
        profile.center={1159.67297f,742.25671f};
        const auto corrected=engine.process(frame,profile,GaugeRunMode::Evidence);
        require(corrected.success&&!corrected.centerInconsistent&&corrected.value>0.39&&corrected.value<0.41,
            "Physical needle reading differs from the visible 0.4 MPa graduation");
        const auto& pointer=corrected.pointerDetectorResult;
        const auto& body=pointer.headEnd=='A'?pointer.directionA:pointer.directionB;
        require(body.meanInsideGray<25,"Selected the gray cast shadow instead of the black needle");
        std::cout<<"Shadow rejection, displaced-hub refusal and independently verified 0.4 MPa pointer passed\n";
        return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<"\n";return 1;}
}
