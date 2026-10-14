#include "ScaleCalibration.hpp"
#include "DeviceCodec.hpp"
#include "DeviceCalibration.hpp"
#include "CardinalSelectorDialog.hpp"
#include "GaugeReaderEngine.hpp"
#include <QApplication>
#include <QMouseEvent>
#include <iostream>
#include <stdexcept>

static void require(bool ok,const char* message) { if (!ok) throw std::runtime_error(message); }
static QJsonObject point(double angle,double ratio) {
    return {{"x",400+300*std::cos(angle*CV_PI/180)},{"y",400+300*std::sin(angle*CV_PI/180)},{"ratio",ratio}};
}
int main(int argc,char** argv) {
    QApplication app(argc,argv);
    try {
        QJsonArray points{point(135,0),point(220,0.3),point(310,0.7),point(45,1)};
        ScaleCalibration::Mapping mapping;
        require(ScaleCalibration::valid(points,800,800,{400,400}),"Valid points rejected");
        require(ScaleCalibration::build(points,{400,400},{},true,mapping),"Cross-zero mapping rejected");
        require(std::abs(mapping.offsets.back()-270)<1e-4,"Wrong clockwise span");
        require(std::abs(ScaleCalibration::ratio(mapping,130)-0.5)<1e-5,"Piecewise interpolation wrong");
        require(ScaleCalibration::ratio(mapping,999)==1,"Upper clamp wrong");
        require(!ScaleCalibration::build(points,{400,400},{},false,mapping),"Wrong direction accepted");
        QJsonArray reverse{point(45,0),point(310,0.3),point(220,0.7),point(135,1)};
        require(ScaleCalibration::build(reverse,{400,400},{},false,mapping),"Counterclockwise mapping rejected");
        // A projective camera view changes raw angles. Applying the inverse
        // homography must recover the original scale mapping, also after resize.
        cv::Mat camera=(cv::Mat_<double>(3,3)<<0.8,0.1,30,0.05,0.7,40,0.0003,0.0001,1);
        std::vector<cv::Point2f> src{{400,400}};
        for (const auto& value:points) {auto p=value.toObject();src.emplace_back(p["x"].toDouble(),p["y"].toDouble());}
        std::vector<cv::Point2f> projected;cv::perspectiveTransform(src,projected,camera);
        QJsonArray original;
        for(int i=0;i<points.size();++i) original.append(QJsonObject{{"x",projected[i+1].x},{"y",projected[i+1].y},{"ratio",points[i].toObject()["ratio"]}});
        auto resized=ScaleCalibration::scaled(original,2,1.5);
        cv::Mat resize=(cv::Mat_<double>(3,3)<<2,0,0,0,1.5,0,0,0,1);
        require(ScaleCalibration::build(resized,{400,400},(resize*camera).inv(),true,mapping),"Perspective mapping rejected");
        require(std::abs(mapping.offsets[1]-85)<0.001,"Perspective/resolution transformation wrong");
        require(std::abs(ScaleCalibration::ratio(mapping,130)-0.5)<0.001,"Perspective interpolation wrong");
        cv::Mat dial(800,800,CV_8UC3,cv::Scalar(240,240,240));
        cv::line(dial,{400,400},{140,400},cv::Scalar(0,0,0),8,cv::LINE_AA);
        cv::circle(dial,{400,400},25,cv::Scalar(0,0,0),-1);
        GaugeProfile profile;
        profile.perspective_correction_enabled=false; profile.auto_detect_roi=false;
        profile.center={400,400}; profile.radius=350; profile.min_val=10; profile.max_val=110;
        profile.calibrated_width=800; profile.calibrated_height=800; profile.scalePoints=points;
        GaugeReaderEngine engine;
        auto result=engine.process(dial,profile,GaugeRunMode::Production);
        require(result.errorStep!="Scale calibration","Engine rejected valid calibration");
        require(ScaleCalibration::build(points,{400,400},{},true,mapping),"Engine mapping setup failed");
        double offset=ScaleCalibration::span(135,result.pointer_angle_deg,true);
        if(offset>270 && offset>335) offset=0;
        require(std::abs(result.value-(10+100*ScaleCalibration::ratio(mapping,offset)))<1e-5,"Engine ignored piecewise device mapping");
        auto uncalibratedProfile=profile; uncalibratedProfile.scalePoints={};
        auto denied=engine.process(dial,uncalibratedProfile,GaugeRunMode::Production);
        require(!denied.success && denied.errorStep=="Scale calibration","Engine allowed missing manual endpoints");
        require(!profile.toJson().contains("min_angle_deg") && !profile.toJson().contains("max_angle_deg"),"Template serialization retained angles");
        QJsonObject cal{{"perspective_correction_enabled",true},{"calibrated_width",800},{"calibrated_height",800},
            {"center_x",400},{"center_y",400},{"pointer_base_radius",47},{"calibration_confirmed",true}};
        const double rim[8]={400,20,780,400,400,780,20,400};
        for(int i=0;i<4;++i) {cal[QString("pt_%1_x").arg(i)]=rim[i*2];cal[QString("pt_%1_y").arg(i)]=rim[i*2+1];}
        require(!validDeviceCalibration(cal),"Old calibration without endpoints accepted");
        cal["scale_points"]=points;
        require(validDeviceCalibration(cal),"Device scale calibration rejected");
        auto device=deviceFromJson(QJsonObject{{"calibration",cal}});
        require(deviceFromJson(deviceToJson(device)).scalePoints==points,"Device roundtrip lost scale points");
        cal["scale_points"]=QJsonArray{point(135,0)};
        require(!validDeviceCalibration(cal),"Incomplete endpoints accepted");
        cal["scale_points"]=QJsonArray{point(135,0),QJsonObject{{"x",400},{"y",400},{"ratio",1}}};
        require(!validDeviceCalibration(cal),"Endpoint on hub accepted");
        InteractiveImageLabel label;label.setRectifiedMode(true);label.resize(800,800);
        label.setImageAndPoints(cv::Mat(800,800,CV_8UC3,cv::Scalar(255,255,255)),{{400,20},{780,400},{400,780},{20,400}},{400,400});
        label.selectScaleRatio(0);
        QMouseEvent click(QEvent::MouseButtonPress,QPointF(180,620),Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
        QApplication::sendEvent(&label,&click);
        label.selectScaleRatio(1);
        QMouseEvent clickMax(QEvent::MouseButtonPress,QPointF(620,620),Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
        QApplication::sendEvent(&label,&clickMax);
        require(label.getScalePoints().size()==2,"Endpoint click placement failed");
        label.selectScaleRatio(0.5);QApplication::sendEvent(&label,&click);
        require(label.getScalePoints()[1].toObject()["ratio"].toDouble()==0.5,"Middle point ordering failed");
        auto roundtrip=ScaleCalibration::transformed(ScaleCalibration::transformed(points,camera),camera.inv());
        for (int i=0;i<points.size();++i) {
            require(std::abs(roundtrip[i].toObject()["x"].toDouble()-points[i].toObject()["x"].toDouble())<0.001,"Inverse mapping x lost precision");
            require(std::abs(roundtrip[i].toObject()["y"].toDouble()-points[i].toObject()["y"].toDouble())<0.001,"Inverse mapping y lost precision");
        }
        GaugeProfile dialogProfile;dialogProfile.perspective_pts={{400,20},{780,400},{400,780},{20,400}};
        CardinalSelectorDialog dialog(dial,dialogProfile,"test");
        auto* originalImage=dialog.findChild<InteractiveImageLabel*>("perspectiveImage");
        auto* correctedImage=dialog.findChild<InteractiveImageLabel*>("rectifiedCalibrationImage");
        require(originalImage->isEnabled() && !correctedImage->isEnabled(),"Stage one did not lock corrected annotations");
        QMetaObject::invokeMethod(&dialog,"onSaveAndApply",Qt::DirectConnection);
        require(dialogProfile.scalePoints.isEmpty(),"Early save modified calibration");
        QMetaObject::invokeMethod(&dialog,"onConfirmPerspective",Qt::DirectConnection);
        require(!originalImage->isEnabled() && correctedImage->isEnabled(),"Perspective confirmation failed to advance");
        correctedImage->resize(800,800);
        QMouseEvent centerClick(QEvent::MouseButtonPress,QPointF(420,390),Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
        QApplication::sendEvent(correctedImage,&centerClick);
        require(cv::norm(correctedImage->getCenterPoint()-cv::Point2f(420,390))<0.001,"Corrected center click placed wrong coordinates");
        QMouseEvent centerDrag(QEvent::MouseMove,QPointF(460,430),Qt::NoButton,Qt::LeftButton,Qt::NoModifier);
        QApplication::sendEvent(correctedImage,&centerDrag);
        require(cv::norm(correctedImage->getCenterPoint()-cv::Point2f(420,390))<0.001,"Center still responds to dragging");
        correctedImage->selectScaleRatio(0);QApplication::sendEvent(correctedImage,&click);
        correctedImage->selectScaleRatio(1);QApplication::sendEvent(correctedImage,&clickMax);
        require(correctedImage->getScalePoints().size()==2,"Corrected scale clicks failed");
        QMetaObject::invokeMethod(&dialog,"onConfirmPerspective",Qt::DirectConnection);
        require(originalImage->isEnabled() && !correctedImage->isEnabled() && correctedImage->getScalePoints().isEmpty(),"Returning to perspective preserved stale annotations");
        std::cout<<"Scale calibration checks passed\n";
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
