#include "GaugeReaderEngine.hpp"
#include "DeviceCodec.hpp"
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QDir>
#include <QElapsedTimer>
#include <iostream>

int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    try {
        if(argc!=5)throw std::runtime_error("usage: exe result.json image output.json repetitions");
        QFile input(argv[1]);
        if(!input.open(QIODevice::ReadOnly))throw std::runtime_error("cannot read configuration");
        const auto root=QJsonDocument::fromJson(input.readAll()).object();
        const auto d=deviceFromJson(root["device"].toObject());
        GaugeProfile p;
        if(!p.loadFromJson(root["profile_snapshot"].toObject(),d.defaultProfileId.toStdString()))throw std::runtime_error("cannot load profile");
        p.perspective_correction_enabled=d.perspectiveEnabled;
        p.perspective_pts={cv::Point2f(d.pt0_x,d.pt0_y),cv::Point2f(d.pt1_x,d.pt1_y),cv::Point2f(d.pt2_x,d.pt2_y),cv::Point2f(d.pt3_x,d.pt3_y)};
        p.center=cv::Point2f(d.centerX,d.centerY);p.scalePoints=d.scalePoints;p.pointerBaseRadius=d.pointerBaseRadius;
        p.calibrated_width=d.calibratedWidth;p.calibrated_height=d.calibratedHeight;
        p.auto_detect_roi=false;p.save_debug_images=false;
        const auto image=cv::imread(argv[2]);
        if(image.empty())throw std::runtime_error("cannot read image");
        GaugeReaderEngine engine;
        QJsonArray runs;
        for(int i=-1;i<std::stoi(argv[4]);++i) {
            QElapsedTimer timer;timer.start();
            const auto r=engine.process(image,p,GaugeRunMode::Evidence,true);
            const double elapsed=timer.nsecsElapsed()/1e6;
            if(i<0)continue;
            runs.append(QJsonObject{{"wall_ms",elapsed},{"success",r.success},{"value",r.value},
                {"angle",r.pointer_angle_deg},{"confidence",r.confidence},{"steps",r.stageTimings},
                {"canvas_width",r.step2_rectified.cols},{"canvas_height",r.step2_rectified.rows},
                {"error",QString::fromStdString(r.errorMessage)}});
        }
        QFile output(argv[3]);
        if(!output.open(QIODevice::WriteOnly))throw std::runtime_error("cannot write report");
        output.write(QJsonDocument(QJsonObject{{"image",QString(argv[2])},{"width",image.cols},{"height",image.rows},
            {"warmups",1},{"runs",runs}}).toJson());
        return 0;
    } catch(const std::exception& error){std::cerr<<error.what()<<std::endl;return 1;}
}
