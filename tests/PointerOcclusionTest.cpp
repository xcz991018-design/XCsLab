#include "GaugeReaderEngine.hpp"
#include "DeviceCodec.hpp"
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <iostream>
#include <stdexcept>
static void require(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    try {
        const QString root=QString(REPO_SOURCE_DIR)+"/tests/fixtures/";
        QFile file(root+"pointer_occlusion.json"); require(file.open(QIODevice::ReadOnly),"Missing fixture metadata");
        const auto record=QJsonDocument::fromJson(file.readAll()).object();
        const auto device=deviceFromJson(record["device"].toObject());
        const auto frame=cv::imread((root+"pointer_occlusion.jpg").toStdString()); require(!frame.empty(),"Missing fixture original");
        GaugeProfile base; require(base.loadFromJson(record["profile_snapshot"].toObject(),device.defaultProfileId.toStdString()),"Invalid frozen template");
        base.perspective_correction_enabled=device.perspectiveEnabled; base.auto_detect_roi=false;
        base.perspective_pts={{float(device.pt0_x),float(device.pt0_y)},{float(device.pt1_x),float(device.pt1_y)},
            {float(device.pt2_x),float(device.pt2_y)},{float(device.pt3_x),float(device.pt3_y)}};
        base.center={float(device.centerX),float(device.centerY)}; base.pointerBaseRadius=device.pointerBaseRadius;
        base.calibrated_width=device.calibratedWidth; base.calibrated_height=device.calibratedHeight;
        base.scalePoints=device.scalePoints; base.save_debug_images=false;
        GaugeReaderEngine engine; GaugeResult reference;
        for(auto mode:{GaugeRunMode::Production,GaugeRunMode::Evidence,GaugeRunMode::Debug}) {
            auto profile=base; const auto result=engine.process(frame,profile,mode);
            require(result.success && !result.centerInconsistent,"Clear text-occluded needle was rejected");
            // The photographed needle is visibly between the 0.4 and 0.5 graduations.
            require(result.value>0.43 && result.value<0.47 && result.pointer_angle_deg>256 && result.pointer_angle_deg<261,
                "Selected a numeral, shadow or counterweight instead of the needle");
            const auto& body=result.pointerDetectorResult.headEnd=='A' ? result.pointerDetectorResult.directionA : result.pointerDetectorResult.directionB;
            require(body.validSectionCount>=20,"Insufficient recovered sections");
            for(const auto* edge:{&body.candidateRegion.leftEdge,&body.candidateRegion.rightEdge}) {
                require(edge->verifiedSupportCount>=20 && edge->maxConsecutiveGap<=16 && edge->lineFitResidual<=2.2,
                    "Recovery bypassed real-edge geometry/support checks");
            }
            if(mode==GaugeRunMode::Production) reference=result;
            else require(result.value==reference.value && result.confidence==reference.confidence && result.recognitionQuality==reference.recognitionQuality,
                         "Recovery differs across rendering modes");
        }
        PointerDetectorConfig config; config.pointerBaseRadius=20; config.maxSearchRadius=130;
        PointerDetector detector(config); cv::Mat blank(320,320,CV_8UC1,cv::Scalar(240));
        require(!detector.detect(blank,{160,160}).success,"Blank frame produced a reading");
        cv::Mat oneEdge=blank.clone(); oneEdge(cv::Rect(0,0,160,320)).setTo(20);
        require(!detector.detect(oneEdge,{160,160}).success,"A single edge was promoted to a needle");
        std::cout<<"Frozen historical occlusion, real bilateral support, mode parity, blank and unilateral rejection passed\n";
        return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<"\n";return 1;}
}
