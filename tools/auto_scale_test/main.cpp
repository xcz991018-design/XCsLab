#include "PaddleScaleOcr.hpp"
#include "OcrDialRange.hpp"
#include "EndpointAngleRectification.hpp"
#include "AutoDialCalibration.hpp"
#include "CameraUndistortion.hpp"
#include <iostream>
#include <stdexcept>
using namespace AutoScaleCalibration;
static void require(bool condition,const char* message) {if(!condition)throw std::runtime_error(message);}
static cv::Point2f point(double angle,double r=260) {
    return {float(400+r*std::cos(angle*CV_PI/180)),float(400+r*std::sin(angle*CV_PI/180))};
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    try {
        {
            require(CameraUndistortion::audit({2304,1296}).contains(QStringLiteral(u"仅数值检查")),"lens audit distinguishes numerical and physical accuracy");
            require(CameraUndistortion::audit({640,480}).contains(QStringLiteral(u"宽高比变化")),"cropped aspect ratio needs explicit review");
            require(CameraUndistortion::audit({0,0}).contains(QStringLiteral(u"失败")),"invalid image extent rejected");
            for(bool clockwise:{true,false}) {
                const double sign=clockwise?1.:-1.;
                const cv::Mat projection=(cv::Mat_<double>(3,3)<<1.04,.025,-26,.01,.96,12,.00003,-.00002,1);
                std::vector<MajorAnchorRectification::Observation> observations;
                for(int i=0;i<6;++i) {
                    const double a=135+sign*270*i/5.;
                    std::vector<cv::Point2f> p{point(a,290),point(a,250)};
                    cv::perspectiveTransform(p,p,projection);
                    observations.push_back({p[0],p[1],i/5.});
                }
                const auto refined=MajorAnchorRectification::fit(observations,{400,400},360,clockwise);
                std::cout<<"Major holdout: "<<refined.message.toStdString()<<"\n";
                require(refined.valid()&&refined.after<.01,"major tips recover projective geometry with independent holdouts");
                // Entirely unseen needle directions, not used by fitting or acceptance.
                std::vector<cv::Point2f> probe{point(135,290),{400,400}};
                for(int i=1;i<20;++i)probe.push_back(point(135+sign*270*i/20.,260));
                cv::perspectiveTransform(probe,probe,refined.correction*projection);
                const auto pivot=probe[1];const auto start=probe[0]-pivot;
                const double originAngle=std::atan2(start.y,start.x)*180/CV_PI;
                for(int i=1;i<20;++i) {
                    const auto v=probe[i+1]-pivot;
                    const double measured=ScaleCalibration::span(AutoScaleCalibration::wrap(originAngle),AutoScaleCalibration::wrap(std::atan2(v.y,v.x)*180/CV_PI),clockwise);
                    require(std::abs(measured/270-i/20.)<1e-5,"unseen known readings recover the correct fraction");
                }
                auto corrupted=observations;corrupted[1].outer+=cv::Point2f(20,-20);
                require(!MajorAnchorRectification::fit(corrupted,{400,400},360,clockwise).valid(),"held-out mismatch rejects correction instead of refitting");
                observations.pop_back();
                require(!MajorAnchorRectification::fit(observations,{400,400},360,clockwise).valid(),"insufficient majors preserve original four points");
            }
        }
        // Sparse, nonuniform anchors must interpolate in either direction across zero.
        for (bool clockwise : {true, false}) {
            const double sign=clockwise?1:-1;
            QJsonArray anchors;
            for (const auto& pair : std::vector<std::pair<double,double>>{{0,0},{80,.4},{200,1}}) {
                const auto p=point(350+sign*pair.first);
                anchors.append(QJsonObject{{"x",p.x},{"y",p.y},{"ratio",pair.second}});
            }
            auto contaminated=anchors;
            const auto wrong=point(350+sign*30);
            contaminated.append(QJsonObject{{"x",wrong.x},{"y",wrong.y},{"ratio",.9},{"source","minor_tick"}});
            contaminated.append(QJsonObject{{"x",wrong.x},{"y",wrong.y},{"ratio",.8},{"source","minor_tick_inferred"}});
            const auto clean=ScaleCalibration::majorAnchors(contaminated);
            require(clean==anchors,"legacy minor ticks cannot affect sparse anchors");
            ScaleCalibration::Mapping mapping;
            require(ScaleCalibration::valid(clean,800,800,{400,400}) &&
                ScaleCalibration::buildForPointer(clean,{400,400},{},clockwise,mapping),"sparse mapping crosses zero in both directions");
            require(std::abs(ScaleCalibration::ratio(mapping,40)-.2)<1e-6,"first interval interpolation");
            require(std::abs(ScaleCalibration::ratio(mapping,140)-.7)<1e-6,"second interval interpolation");
            require(ScaleCalibration::ratio(mapping,0)==0 && ScaleCalibration::ratio(mapping,200.01)==1,"endpoint values");
        }
        {
            cv::Mat image(800,800,CV_8UC3);cv::randu(image,0,256);
            const auto bounds=PaddleScaleOcr::rectifiedDialBounds(image.size(),40);
            require(bounds==cv::Rect(40,40,721,721),"dial crop includes all four cardinal boundaries");
            const auto cropped=PaddleScaleOcr::inputRegion(image,{},bounds);
            require(cropped.left==40 && cropped.top==40 &&
                cv::norm(cropped.image,image(bounds),cv::NORM_INF)==0,"dial crop preserves native pixels");
            QJsonArray local{QJsonObject{{"text","0.2"},{"score",.99},{"polygon",QJsonArray{
                QJsonArray{10,20},QJsonArray{30,20},QJsonArray{30,40},QJsonArray{10,40}}}}};
            const auto restored=PaddleScaleOcr::restoreTokens(local,cropped.top,{},cropped.left);
            require(restored[0].toObject()["polygon"].toArray()[0].toArray()==QJsonArray{50,60},
                "OCR polygons return to full rectified coordinates without liquid");
            LiquidSurface surface;surface.found=true;surface.centerX=400;surface.radius=360;
            surface.y0=300;surface.slope=70;surface.margin=8;
            const auto fullLiquid=PaddleScaleOcr::inputRegion(image,surface);
            const auto dialLiquid=PaddleScaleOcr::inputRegion(image,surface,bounds);
            const cv::Rect equivalent(dialLiquid.left,dialLiquid.top-fullLiquid.top,dialLiquid.image.cols,dialLiquid.image.rows);
            require(cv::norm(dialLiquid.image,fullLiquid.image(equivalent),cv::NORM_INF)==0,
                "sloped liquid masking uses full-image coordinates after dial crop");
            const auto mapped=PaddleScaleOcr::restoreTokens(local,dialLiquid.top,surface,dialLiquid.left);
            require(!mapped.empty(),"visible numeric token survives combined crop");
            require(PaddleScaleOcr::rectifiedDialBounds(image.size(),500)==cv::Rect(0,0,800,800),
                "invalid margin safely preserves full image");
            require(PaddleScaleOcr::inputRegion(image,{},cv::Rect(900,900,20,20)).image.empty(),
                "outside crop must not access image memory");
        }
        if(argc==4&&(QString::fromLocal8Bit(argv[1])=="--ocr-liquid"||QString::fromLocal8Bit(argv[1])=="--ocr-liquid800"||QString::fromLocal8Bit(argv[1])=="--ocr-liquid-hd")) {
            auto image=CameraUndistortion::image(cv::imread(argv[2]));const auto dial=AutoDialCalibration::detect(image);require(dial.valid(),"liquid OCR dial");
            const int size=QString::fromLocal8Bit(argv[1])=="--ocr-liquid-hd"?std::max(image.cols,image.rows):QString::fromLocal8Bit(argv[1])=="--ocr-liquid800"?800:810;
            const float half=size*.5f,margin=size*.05f;const cv::Point2f center(half,half);const double radius=half-margin;
            auto H=cv::getPerspectiveTransform(dial.points,std::vector<cv::Point2f>{{half,margin},{size-margin,half},{half,size-margin},{margin,half}});
            cv::Mat rectified;cv::warpPerspective(image,rectified,H,{size,size},cv::INTER_CUBIC);
            const auto surface=detectLiquidSurface(rectified,center,radius);require(surface.found,"liquid surface");
            const auto bounds=PaddleScaleOcr::rectifiedDialBounds(rectified.size(),cvRound(margin));
            const auto region=PaddleScaleOcr::inputRegion(rectified,surface,bounds);cv::imwrite(std::string(argv[3])+"-input.png",region.image);
            auto cancelled=std::make_shared<std::atomic_bool>(false);QElapsedTimer timer;timer.start();
            const auto result=PaddleScaleOcr::recognize(rectified,cancelled,surface,bounds);require(result.valid(),qPrintable(result.error));
            auto tokens=PaddleScaleOcr::numericTokens(result.tokens);std::vector<double> values;
            for(auto t:tokens){auto o=t.toObject();values.push_back(o["text"].toString().toDouble());}
            const auto calibration=OcrDialRange::calibrate(rectified,result.tokens,center,radius,true,surface);
            QFile out(QString::fromLocal8Bit(argv[3])+".json");require(out.open(QIODevice::WriteOnly),"masked OCR output");
            out.write(QJsonDocument(QJsonObject{{"tokens",tokens},{"elapsed_ms",double(timer.elapsed())},{"crop_top",region.top},{"points",calibration.points}}).toJson());
            out.close();cv::imwrite(std::string(argv[3])+"-full.png",rectified);
            for(double expected:{0.,.2,.8,1.})require(std::any_of(values.begin(),values.end(),[&](double v){return std::abs(v-expected)<1e-6;}),"masked OCR must retain endpoints and lower major labels");
            require(calibration.valid(),qPrintable(calibration.error));
            std::cout<<"PASS cropped OCR: "<<region.image.cols<<"x"<<region.image.rows<<" top="<<region.top<<" tokens="<<tokens.size()<<" elapsed_ms="<<timer.elapsed()<<'\n';return 0;
        }
        {
            cv::Mat image(100,120,CV_8UC3,cv::Scalar(80,80,80));LiquidSurface surface;
            surface.found=true;surface.centerX=60;surface.radius=60;surface.y0=30;surface.slope=6;surface.curve=4;surface.margin=2;
            const auto region=PaddleScaleOcr::inputRegion(image,surface);require(region.top>0&&region.image.rows<100,"crop must reduce OCR input height");
            for(int y=0;y<region.image.rows;++y)for(int x=0;x<region.image.cols;++x)
                if(surface.excludes(x,y+region.top))require(region.image.at<cv::Vec3b>(y,x)==cv::Vec3b(255,255,255),"sloped and curved masked pixels must be blank");
            QJsonArray raw{QJsonObject{{"text","1"},{"polygon",QJsonArray{QJsonArray{60,40},QJsonArray{65,40},QJsonArray{65,60},QJsonArray{60,60}}}},
                QJsonObject{{"text","0.4"},{"polygon",QJsonArray{QJsonArray{110,0},QJsonArray{115,0},QJsonArray{115,15},QJsonArray{110,15}}}}};
            const auto restored=PaddleScaleOcr::restoreTokens(raw,region.top,surface);
            require(restored.size()==1&&restored[0].toObject()["polygon"].toArray()[0].toArray()[1].toDouble()==40+region.top,"coordinate restoration and boundary crossing exclusion");
            require(PaddleScaleOcr::inputRegion(image,{}).image.rows==100,"no liquid preserves full image");
            surface.y0=120;require(PaddleScaleOcr::inputRegion(image,surface).image.empty(),"fully masked image has no OCR region");
        }
        if(argc==4&&(QString::fromLocal8Bit(argv[1])=="--dial"||QString::fromLocal8Bit(argv[1])=="--dial-baseline")) {
            auto image=CameraUndistortion::image(cv::imread(argv[2]));
            const auto dial=AutoDialCalibration::detect(image,QString::fromLocal8Bit(argv[1])!="--dial-baseline");require(dial.valid(),qPrintable(dial.error));
            const auto H=cv::getPerspectiveTransform(dial.points,std::vector<cv::Point2f>{{400,40},{760,400},{400,760},{40,400}});
            cv::Mat rectified;cv::warpPerspective(image,rectified,H,{800,800},cv::INTER_CUBIC);
            cv::imwrite(std::string(argv[3])+"-rectified.png",rectified);
            for(int i=0;i<4;++i){cv::circle(image,dial.points[i],5,{0,0,255},2);cv::line(image,dial.points[i],dial.points[(i+1)%4],{255,255,0},2);}
            cv::circle(image,dial.hub,5,{0,255,0},2);cv::imwrite(std::string(argv[3])+"-source.png",image);
            QJsonArray points;for(auto p:dial.points)points.append(QJsonArray{p.x,p.y});
            QFile out(QString::fromLocal8Bit(argv[3])+".json");require(out.open(QIODevice::WriteOnly),"dial output");
            out.write(QJsonDocument(QJsonObject{{"points",points},{"hub_x",dial.hub.x},{"hub_y",dial.hub.y},{"coverage",dial.rimCoverage},
                {"graduation_support",dial.graduationSupport},{"graduation_residual",dial.graduationResidual},{"graduation_aspect",dial.graduationAspect},
                {"liquid_excluded",dial.graduationLiquidExcluded},{"liquid_ambiguous",dial.graduationLiquidAmbiguous},
                {"structural_rectification",dial.structuralRectification},{"structural_support",dial.structuralSupport},{"structural_symmetric_pairs",dial.structuralSymmetricPairs},
                {"structural_before",dial.structuralErrorBefore},{"structural_after",dial.structuralErrorAfter},
                {"span_before",dial.structuralSpanBefore},{"span_after",dial.structuralSpanAfter},
                {"scale_origin_x",dial.graduationCenter.x},{"scale_origin_y",dial.graduationCenter.y},
                {"initial_graduation_aspect",dial.initialGraduationAspect}}).toJson());
            std::cout<<"PASS dial hub="<<dial.hub<<" coverage="<<dial.rimCoverage<<" ticks="<<dial.graduationSupport<<" residual="<<dial.graduationResidual<<" aspect="<<dial.graduationAspect
                <<" structural="<<dial.structuralRectification<<" support="<<dial.structuralSupport<<" symmetric_pairs="<<dial.structuralSymmetricPairs<<" error "<<dial.structuralErrorBefore<<" -> "<<dial.structuralErrorAfter<<'\n';return 0;
        }
        if((argc==5||argc==6) && QString::fromLocal8Bit(argv[1])=="--numbers") {
            cv::Mat image=cv::imread(argv[2]);require(!image.empty(),"numeric inspection image");
            QFile input(QString::fromLocal8Bit(argv[3]));require(input.open(QIODevice::ReadOnly),"numeric tokens input");
            std::vector<double> allowed;
            if(argc==6)require(PaddleScaleOcr::parseAllowedValues(QString::fromLocal8Bit(argv[5]),allowed),"numeric candidate list");
            const auto tokens=PaddleScaleOcr::numericTokens(QJsonDocument::fromJson(input.readAll()).object()["tokens"].toArray(),allowed);
            QFile output(QString::fromLocal8Bit(argv[4])+".json");require(output.open(QIODevice::WriteOnly),"numeric output");
            const auto range=OcrDialRange::detect(tokens,{image.cols*.5f,image.rows*.5f},image.cols);
            std::cout<<"OCR range valid="<<range.valid<<" min="<<range.minimum<<" max="<<range.maximum<<'\n';
            const auto endpoints=OcrDialRange::findEndpoints(image,range,{image.cols*.5f,image.rows*.5f},image.cols*.5-40,true);
            const auto surface=detectLiquidSurface(image,{image.cols*.5f,image.rows*.5f},image.cols*.5-40);
            const auto calibration=OcrDialRange::calibrate(image,tokens,{image.cols*.5f,image.rows*.5f},image.cols*.5-40,true,surface);
            QJsonArray probes;const auto majors=OcrDialRange::majorTicks(image,tokens,{image.cols*.5f,image.rows*.5f},image.cols*.5-40,surface,&probes);
            QJsonArray strokes;for(const auto& m:majors)strokes.append(QJsonObject{{"angle",m.tick.angle},{"r",m.tick.radius},{"length",m.tick.length},{"inner_width",m.innerWidth},{"outer_width",m.outerWidth}});
            QJsonArray rawStrokes;for(const auto& t:AutoScaleCalibration::ticks(image,{image.cols*.5f,image.rows*.5f},image.cols*.5-40,.055))rawStrokes.append(QJsonObject{{"angle",t.angle},{"r",t.radius},{"length",t.length}});
            output.write(QJsonDocument(QJsonObject{{"tokens",tokens},{"endpoint_points",endpoints.points},{"endpoint_error",endpoints.error},
                {"calibration_points",calibration.points},{"unlabeled_major_points",calibration.unlabeledPoints},{"calibration_error",calibration.error},{"major_ticks",strokes},{"raw_ticks",rawStrokes},{"stroke_probes",probes},{"liquid_y",surface.y0}}).toJson());
            std::cout<<"Major calibration "<<calibration.valid()<<" strokes="<<majors.size()<<" points="<<calibration.points.size()<<" "<<calibration.error.toStdString()<<'\n';
            std::cout<<"Endpoints "<<endpoints.valid()<<" "<<endpoints.error.toStdString()<<" "<<QJsonDocument(endpoints.points).toJson(QJsonDocument::Compact).toStdString()<<'\n';
            for(const auto& value:calibration.points) {
                const auto p=value.toObject();const cv::Point end(cvRound(p["x"].toDouble()),cvRound(p["y"].toDouble()));
                cv::line(image,{image.cols/2,image.rows/2},end,{0,255,0},2);cv::circle(image,end,6,{0,0,255},2);
            }
            for(const auto& value:calibration.unlabeledPoints) {
                const auto p=value.toObject();
                const cv::Point end(cvRound(p["x"].toDouble()),cvRound(p["y"].toDouble()));
                const double reading=calibration.range.minimum+p["ratio"].toDouble()*(calibration.range.maximum-calibration.range.minimum);
                cv::circle(image,end,7,{255,160,80},2);
                cv::putText(image,QString::number(reading).toStdString(),end+cv::Point(8,-6),cv::FONT_HERSHEY_SIMPLEX,.8,{255,160,80},2);
            }
            for(const auto& token:tokens) {
                const auto item=token.toObject();std::vector<cv::Point> points;
                for(const auto& p:item["polygon"].toArray())points.emplace_back(cvRound(p.toArray()[0].toDouble()),cvRound(p.toArray()[1].toDouble()));
                cv::polylines(image,points,true,{255,255,0},2);
                cv::putText(image,item["text"].toString().toStdString(),points[0]-cv::Point(0,5),cv::FONT_HERSHEY_SIMPLEX,.65,{255,255,0},2);
            }
            cv::imwrite(std::string(argv[4])+".png",image);
            std::cout<<"PASS numeric inspection: "<<tokens.size()<<" boxes\n";return 0;
        }
        if(argc==4 && QString::fromLocal8Bit(argv[1])=="--bridge") {
            qputenv("GAUGE_OCR_PYTHON",argv[2]);qputenv("GAUGE_OCR_SCRIPT",argv[3]);
            cv::Mat image(800,800,CV_8UC3,cv::Scalar(245,245,245));
            for(int i=0;i<=8;++i)cv::line(image,point(135+270*i/8.,244),point(135+270*i/8.,276),{10,10,10},4);
            auto cancelled=std::make_shared<std::atomic_bool>(false);
            const auto result=PaddleScaleOcr::run(image,{400,400},360,0,1.6,true,cancelled);
            require(result.valid() && result.points.size()==9,"subprocess JSON protocol and real pixel matching");
            const auto cropOcr=PaddleScaleOcr::recognize(image,cancelled,{},PaddleScaleOcr::rectifiedDialBounds(image.size(),40));
            require(cropOcr.valid(),"cropped subprocess OCR succeeds");
            const auto cropMatched=AutoScaleCalibration::match(AutoScaleCalibration::numbers(cropOcr.tokens,0,1.6),
                AutoScaleCalibration::ticks(image,{400,400},360),{400,400},360,0,1.6,true);
            require(cropMatched.valid() && cropMatched.points==result.points,
                "cropped OCR coordinates produce identical full-image scale calibration");
            qputenv("GAUGE_OCR_FIXTURE_MODE","invalid");
            require(!PaddleScaleOcr::run(image,{400,400},360,0,1.6,true,cancelled).valid(),"malformed subprocess response rejected");
            qputenv("GAUGE_OCR_FIXTURE_MODE","sleep");cancelled->store(true);
            QElapsedTimer timer;timer.start();
            require(!PaddleScaleOcr::run(image,{400,400},360,0,1.6,true,cancelled).valid() && timer.elapsed()<4000,"cancellation must terminate child process promptly");
            std::cout<<"PASS: OCR subprocess, stdout isolation, invalid JSON and cancellation\n";return 0;
        }
        // Optional real-image smoke test: --image raw.png output-prefix min max.
        // Uses the production lens correction, automatic dial geometry and OCR adapter.
        const bool replay=argc==7 && QString::fromLocal8Bit(argv[1])=="--replay";
        if(replay || (argc==6 && (QString::fromLocal8Bit(argv[1])=="--image" || QString::fromLocal8Bit(argv[1])=="--rectified"))) {
            const int prefixIndex=replay?4:3;
            cv::Mat rectified;std::vector<cv::Point2f> center{{400,400}};
            if(replay || QString::fromLocal8Bit(argv[1])=="--rectified")rectified=cv::imread(argv[2]);
            else {
                const auto image=CameraUndistortion::image(cv::imread(argv[2]));
                const auto dial=AutoDialCalibration::detect(image);
                require(dial.valid(),qPrintable(dial.error));
                const std::vector<cv::Point2f> destination{{400,40},{760,400},{400,760},{40,400}};
                const auto H=cv::getPerspectiveTransform(dial.points,destination);
                cv::warpPerspective(image,rectified,H,{800,800},cv::INTER_CUBIC);
                center={dial.hub};cv::perspectiveTransform(center,center,H);
            }
            cv::imwrite(std::string(argv[prefixIndex])+"-rectified.png",rectified);
            auto cancelled=std::make_shared<std::atomic_bool>(false);
            Result result;
            if(replay) {
                QFile tokens(QString::fromLocal8Bit(argv[3]));require(tokens.open(QIODevice::ReadOnly),"tokens file");
                result=match(numbers(QJsonDocument::fromJson(tokens.readAll()).object()["tokens"].toArray(),std::stod(argv[5]),std::stod(argv[6])),
                    ticks(rectified,center[0],360),center[0],360,std::stod(argv[5]),std::stod(argv[6]),true);
            }else result=PaddleScaleOcr::run(rectified,center[0],360,std::stod(argv[4]),std::stod(argv[5]),true,cancelled);
            QFile output(QString::fromLocal8Bit(argv[prefixIndex])+".json");require(output.open(QIODevice::WriteOnly),"output file");
            output.write(QJsonDocument(QJsonObject{{"points",result.points},{"error",result.error},{"diagnostics",result.diagnostics},{"matched_numbers",result.matchedNumbers}}).toJson());
            for(const auto& p:result.points) {
                const auto value=p.toObject();const cv::Point end(cvRound(value["x"].toDouble()),cvRound(value["y"].toDouble()));
                cv::line(rectified,center[0],end,{0,220,0},1);cv::circle(rectified,end,5,{0,0,255},2);
                cv::putText(rectified,std::to_string(value["ratio"].toDouble()),end,cv::FONT_HERSHEY_SIMPLEX,.5,{0,0,255});
            }
            cv::imwrite(std::string(argv[prefixIndex])+"-overlay.png",rectified);
            require(result.valid(),qPrintable(result.error));std::cout<<"PASS real OCR: "<<result.points.size()<<" points\n";return 0;
        }
        for(bool clockwise:{true,false})for(double start:{135.,315.,15.}) {
            std::vector<Number> labels;std::vector<Tick> lines;
            const int sign=clockwise?1:-1;
            cv::Mat image(800,800,CV_8UC3,cv::Scalar(245,245,245));
            for(int i=0;i<=8;++i) {
                const double a=wrap(start+sign*270*i/8.);
                lines.push_back({point(a),a,260,32});
                cv::line(image,point(a,244),point(a,276),{10,10,10},4);
                if(i>0&&i<8)labels.push_back({i*.2,.99,point(a+2,210)}); // missing both endpoints; offset label centers
            }
            labels.push_back({.4,.95,point(start+sign*90,100)}); // central unrelated text
            labels.push_back({.2,.9,point(start+sign*180,210)}); // wrong OCR observation
            const auto result=match(labels,lines,{400,400},360,0,1.6,clockwise);
            require(result.valid() && result.points.size()==9,"rotated CW/CCW and missing endpoints");
            require(cv::norm(cv::Point2f(float(result.points[0].toObject()["x"].toDouble()),float(result.points[0].toObject()["y"].toDouble()))-lines[0].point)<1,"must use actual tick not number position");
            auto detected=ticks(image,{400,400},360);
            const auto fromPixels=match(labels,detected,{400,400},360,0,1.6,clockwise);
            if(!fromPixels.valid()) {std::cerr<<"start="<<start<<" clockwise="<<clockwise<<" ticks="<<detected.size()<<" "<<fromPixels.error.toStdString()<<'\n';for(const auto& tick:detected)std::cerr<<tick.angle<<" r="<<tick.radius<<" len="<<tick.length<<'\n';}
            require(fromPixels.valid(),"image tick extraction and matching");
            auto missing=lines;missing.erase(missing.begin());
            require(!match(labels,missing,{400,400},360,0,1.6,clockwise).valid(),"do not invent endpoint without tick evidence");
            missing.push_back({lines.front().point,lines.front().angle,260,12});
            require(!match(labels,missing,{400,400},360,0,1.6,clockwise).valid(),"minor tick cannot replace missing major endpoint");
            auto sparse=labels;sparse.resize(2);
            require(!match(sparse,lines,{400,400},360,0,1.6,clockwise).valid(),"two labels insufficient for reliable calibration");
            auto doubleRing=lines;auto doubleLabels=labels;
            for(const auto& tick:lines)doubleRing.push_back({point(tick.angle,310),tick.angle,310,32});
            for(int i=1;i<8;++i)doubleLabels.push_back({i*.2,.99,point(wrap(start+sign*270*i/8.),310)});
            require(!match(doubleLabels,doubleRing,{400,400},360,0,1.6,clockwise).valid(),"ambiguous rings must be rejected");
        }
        QJsonArray tokens;
        const auto polygon=QJsonArray{QJsonArray{10,10},QJsonArray{20,10},QJsonArray{20,20},QJsonArray{10,20}};
        for(const QString text:{"0.4","MPa","1.6MPa","1.6.0","-0.2","0.8"})tokens.append(QJsonObject{{"text",text},{"score",text=="0.8"?.3:.99},{"polygon",polygon}});
        require(numbers(tokens,-1,2).size()==2,"strict numeric parsing and confidence filter");
        auto inspection=PaddleScaleOcr::numericTokens(tokens);
        require(inspection.size()==1,"inspection must deduplicate rotated boxes at the same location");
        const QJsonArray outsideRange{QJsonObject{{"text","9999"},{"score",.99},{"polygon",polygon}}};
        require(PaddleScaleOcr::numericTokens(outsideRange).isEmpty(),"built-in vocabulary excludes serial numbers");
        const QJsonArray rotatedDecimal{
            QJsonObject{{"text","80"},{"score",.999},{"polygon",polygon},{"orientation_preferred",false}},
            QJsonObject{{"text","0.8"},{"score",.93},{"polygon",polygon},{"orientation_preferred",true}}};
        require(PaddleScaleOcr::numericTokens(rotatedDecimal)[0].toObject()["text"].toString()=="0.8",
            "upright decimal must beat higher confidence rotated integer at same position");
        std::vector<double> allowed;
        require(PaddleScaleOcr::parseAllowedValues(QStringLiteral(u"0，0.2, 0.8 1; -0.1"),allowed)&&allowed.size()==5,
            "candidate list accepts decimal, negative, Chinese separators and whitespace");
        require(PaddleScaleOcr::numericTokens(outsideRange,allowed).isEmpty(),"candidate whitelist excludes serial numbers");
        require(PaddleScaleOcr::numericTokens(rotatedDecimal,allowed).size()==1,"whitelist accepts the actual decimal");
        require(!PaddleScaleOcr::parseAllowedValues("0,abc,1",allowed),"invalid candidate list rejected");
        const auto label=[](QString text,double x,double y){return QJsonObject{{"text",text},{"score",.98},
            {"polygon",QJsonArray{QJsonArray{x-10,y-20},QJsonArray{x+10,y-20},QJsonArray{x+10,y+20},QJsonArray{x-10,y+20}}}};};
        QJsonArray vocabulary;
        for(int i=0;i<=20;++i)vocabulary.append(label(QString::number(i/10.0),20+i*30,80));
        for(const QString value:{"-0.1","2.1","0.05","80","90"})vocabulary.append(label(value,30+vocabulary.size()*30,180));
        require(PaddleScaleOcr::builtInDialValues().size()==21&&PaddleScaleOcr::numericTokens(vocabulary).size()==21,
            "all 21 built-in values accepted; negatives, out-of-range and off-step values excluded");
        QJsonArray endpoints{label("0.1",240,550),label("1.6",560,550),label("9999",400,630),label("90",500,180)};
        auto range=OcrDialRange::detect(endpoints,{400,400},800);
        require(range.valid&&range.minimum==.1&&range.maximum==1.6,"range uses lower left/right OCR positions, not overall min/max text");
        endpoints.removeAt(1);require(!OcrDialRange::detect(endpoints,{400,400},800).valid,"missing endpoint cannot invent a range");
        endpoints.append(label("0",560,550));require(!OcrDialRange::detect(endpoints,{400,400},800).valid,"reversed endpoint values rejected");
        endpoints=QJsonArray{label("0.1",240,550),label("1.6",560,550),label("0.4",265,550)};
        require(!OcrDialRange::detect(endpoints,{400,400},800).valid,"conflicting labels at an endpoint must require review");
        const auto endpointRange=OcrDialRange::detect(QJsonArray{label("0",240,550),label("1",550,550)},{400,400},800);
        std::vector<Tick> endpointTicks{{point(135,280),135,280,38},{point(45,280),45,280,38},
            {point(141,285),141,285,18},{point(40,285),40,285,18},{point(135,215),135,215,50}};
        auto matchedEndpoints=OcrDialRange::matchEndpoints(endpointRange,endpointTicks,{400,400},360,true);
        require(matchedEndpoints.valid()&&cv::norm(cv::Point2f(matchedEndpoints.points[0].toObject()["x"].toDouble(),matchedEndpoints.points[0].toObject()["y"].toDouble())-point(135,280))<.01,
            "two endpoint labels suffice; use outer major tick pixels instead of label/minor tick/inner text");
        cv::Mat endpointImage(800,800,CV_8UC3,cv::Scalar(245,245,245));
        for(const auto& t:endpointTicks)cv::line(endpointImage,point(t.angle,t.radius-t.length/2),point(t.angle,t.radius+t.length/2),{10,10,10},4);
        require(OcrDialRange::findEndpoints(endpointImage,endpointRange,{400,400},360,true).valid(),"two-label endpoint matching from actual line pixels");
        cv::Mat splitZero(800,800,CV_8UC3,cv::Scalar(245,245,245));
        cv::line(splitZero,point(135,245),point(135,272),{10,10,10},8);
        cv::line(splitZero,point(135,270),point(135,312),{10,10,10},2);
        cv::line(splitZero,point(143,282),point(143,307),{10,10,10},3);
        const auto zeroPieces=OcrDialRange::majorTicks(splitZero,{}, {400,400},360,{});
        int zeroCount=0;
        for(const auto& piece:zeroPieces)if(AutoScaleCalibration::distance(piece.tick.angle,135)<1.7) {
            ++zeroCount;require(piece.tick.length>55,"zero head and thin stem must retain their full radial extent");
        }
        require(zeroCount==1,"zero head and stem must form one major candidate rather than favor the next minor tick");
        QJsonArray merged;const QJsonObject middle{{"x",400},{"y",120},{"ratio",.5}};
        require(OcrDialRange::mergeEndpoints(QJsonArray{middle},matchedEndpoints,{400,400},800,true,merged)&&merged.size()==3&&merged[1].toObject()==middle,
            "endpoint replacement preserves existing middle points");
        const auto original=merged;
        const QJsonObject badMinor{{"x",400},{"y",680},{"ratio",.3},{"source","minor_tick"}};
        require(OcrDialRange::mergeEndpoints(QJsonArray{middle,badMinor},matchedEndpoints,{400,400},800,true,merged)&&merged==original,
            "endpoint refresh discards legacy minor ticks but preserves manual anchors");
        OcrDialRange::Calibration sparseCalibration;
        sparseCalibration.range=endpointRange;
        sparseCalibration.points=original;
        require(OcrDialRange::mergeCalibration(QJsonArray{badMinor},sparseCalibration,{400,400},800,true,merged)&&merged==original,
            "automatic calibration cannot reintroduce legacy minor ticks");
        require(!OcrDialRange::mergeEndpoints(QJsonArray{QJsonObject{{"x",400},{"y",680},{"ratio",.5}}},matchedEndpoints,{400,400},800,true,merged)&&merged==original,
            "conflicting middle point rejects update without altering existing calibration");
        endpointTicks.erase(endpointTicks.begin());
        require(!OcrDialRange::matchEndpoints(endpointRange,endpointTicks,{400,400},360,true).valid(),"missing major endpoint must not be replaced by a minor tick or predicted angle");
        require(!OcrDialRange::findEndpoints(cv::Mat(800,800,CV_8UC3,cv::Scalar(245,245,245)),endpointRange,{400,400},360,true).valid(),"blank dial must not invent endpoints");
        const std::vector<Tick> inwardMajorTicks{{point(135,243),135,243,46},{point(45,243),45,243,46}};
        require(OcrDialRange::matchEndpoints(endpointRange,inwardMajorTicks,{400,400},360,true).valid(),
            "long major ticks may have an inward midpoint; validate their outer tips instead of rejecting the midpoint");
        const double leftTip=std::sqrt(300.*300-2*25.*25),rightTip=300-25*std::sqrt(2.);
        const std::vector<Tick> offsetRingEnds{{point(135,leftTip-20),135,leftTip-20,40},{point(45,rightTip-20),45,rightTip-20,40}};
        require(!OcrDialRange::matchEndpoints(endpointRange,offsetRingEnds,{400,400},360,true).valid()&&
            OcrDialRange::matchEndpoints(endpointRange,offsetRingEnds,{400,400},360,true,{},cv::RotatedRect({375,375},{600,600},0)).valid(),
            "observed annulus support must allow unequal endpoint radii about an offset hub, while retaining the measured 270-degree span");
        const std::vector<Tick> distortedTicks{{point(146,280),146,280,38},{point(47,280),47,280,38}};
        const auto distorted=OcrDialRange::matchEndpoints(endpointRange,distortedTicks,{400,400},360,true);
        require(!distorted.valid(),"261-degree endpoints must not admit an adjacent stroke as the minimum");
        for(double end:{39.,51.}) {
            const std::vector<Tick> boundary{{point(135,280),135,280,38},{point(end,280),end,280,38}};
            require(OcrDialRange::matchEndpoints(endpointRange,boundary,{400,400},360,true).valid(),"both 6-degree tolerance boundaries are accepted");
        }
        const std::vector<Tick> excessive{{point(149,280),149,280,38},{point(46,280),46,280,38}};
        require(!OcrDialRange::matchEndpoints(endpointRange,excessive,{400,400},360,true).valid(),"13-degree span error is still rejected");
        const std::vector<Tick> adjacent{{point(144,280),144,280,38},{point(45,280),45,280,38}};
        require(!OcrDialRange::matchEndpoints(endpointRange,adjacent,{400,400},360,true).valid(),"a nearby long zero-stop stroke cannot pass with a 9-degree endpoint error");
        require(!match({}, {},{400,400},360,1,1,true).valid(),"invalid range");
        // Major heads, thin stems, shorter intervening ticks and a nearby needle.
        // Validate against drawn pixel geometry, including all lower OCR values.
        const auto majorImage=[&](bool omitMiddle,double rightMiddleShift=0) {
            cv::Mat image(800,800,CV_8UC3,cv::Scalar(240,240,240));
            for(int i=0;i<=50;++i) {
                const double a=135+i*5.4+(i==40?rightMiddleShift:0);const bool major=i%10==0&&!(omitMiddle&&i==10);
                cv::line(image,point(a,major?260:282),point(a,300),{15,15,15},major?3:2);
                if(major)cv::line(image,point(a,260),point(a,275),{15,15,15},7);
            }
            cv::line(image,{400,400},point(183,315),{10,10,10},6);return image;
        };
        QJsonArray majorLabels;for(int i=0;i<=5;++i){const auto p=point(135+i*54,220);majorLabels.append(label(QString::number(i*.2),p.x,p.y));}
        const auto fullMajors=OcrDialRange::calibrate(majorImage(false),majorLabels,{400,400},360,true);
        std::cout<<"Complete major geometry: "<<fullMajors.geometry.size()<<" / "<<fullMajors.points.size()<<"\n";
        require(fullMajors.valid()&&fullMajors.points.size()==6&&fullMajors.geometry.size()==6,
            "OCR major matches retain six measured full-stroke endpoints for refinement");
        require(fullMajors.unlabeledPoints.isEmpty(),"ordinary short ticks do not become reading anchors");
        for(bool clockwise:{true,false}) {
            cv::Mat image(800,800,CV_8UC3,cv::Scalar::all(240));
            QJsonArray labels;
            const auto angle=[&](double ratio){return clockwise?135+270*ratio:45-270*ratio;};
            for(int i=0;i<=50;++i) {
                const double ratio=i/50.;
                const bool numbered=i%10==0;
                const bool blank=i==5||i==15||i==45;
                const double a=angle(ratio)+(i==5?2.:0.);
                cv::line(image,point(a,numbered?256:blank?267:282),point(a,300),{15,15,15},3);
                if(numbered) {
                    const auto p=point(a,220);
                    labels.append(label(QString::number(.2+ratio),p.x,p.y));
                }
            }
            for(double extra:{.05,.15,.6+.2/3,.6+2*.2/3})
                cv::line(image,point(angle(extra),267),point(angle(extra),300),{15,15,15},3);
            auto cal=OcrDialRange::calibrate(image,labels,{400,400},360,clockwise);
            require(cal.valid()&&cal.points.size()==6&&cal.unlabeledPoints.size()==7,"each numbered interval uses its own count of observed long ticks");
            QJsonArray saved;
            require(OcrDialRange::mergeCalibration({},cal,{400,400},800,clockwise,saved)&&saved.size()==13,"save labeled and unlabeled anchors together");
            ScaleCalibration::Mapping reading;
            require(ScaleCalibration::buildForPointer(ScaleCalibration::majorAnchors(saved),{400,400},{},clockwise,reading),"new anchors participate in pointer mapping");
            const double expected[]={.05,.1,.15,.3,.6+.2/3,.6+2*.2/3,.9};
            for(int i=0;i<7;++i) {
                const auto p=cal.unlabeledPoints[i].toObject();
                const double a=angle(expected[i])+(i==1?2.:0.);
                require(ScaleCalibration::isUnlabeledMajor(p)&&std::abs(p["ratio"].toDouble()-expected[i])<1e-6&&
                    std::abs(ScaleCalibration::ratio(reading,ScaleCalibration::span(reading.angles.front(),a,clockwise))-expected[i])<.001,
                    "reading interpolates through measured stroke even when displaced from the ideal angle");
            }
            auto manual=cal.unlabeledPoints.first().toObject();manual.remove("source");manual["x"]=manual["x"].toDouble()+1;
            saved[1]=manual;
            QJsonArray refreshed;
            require(OcrDialRange::mergeCalibration(saved,cal,{400,400},800,clockwise,refreshed)&&refreshed==saved,"manual correction takes precedence on retry");
            cal.unlabeledPoints={};
            require(OcrDialRange::mergeCalibration(saved,cal,{400,400},800,clockwise,refreshed)&&refreshed.size()==7,"refresh drops unobserved automatic points");
            LiquidSurface liquid;liquid.found=true;liquid.centerX=400;liquid.radius=360;liquid.y0=245;liquid.margin=5;
            const auto masked=OcrDialRange::calibrate(image,labels,{400,400},360,clockwise,liquid);
            require(masked.valid()&&masked.unlabeledPoints.size()==4,"partially masked count must not renumber visible ticks");
            cv::line(image,point(angle(.25),267),point(angle(.25),300),{15,15,15},3);
            for(double nearNeedle:{.48,.52})
                cv::line(image,point(angle(nearNeedle),267),point(angle(nearNeedle),300),{15,15,15},3);
            const auto irregular=OcrDialRange::calibrate(image,labels,{400,400},360,clockwise);
            require(irregular.valid()&&irregular.unlabeledPoints.size()==6,"missing or crowded long strokes reject only the affected intervals");

            // A liquid boundary hides the central graduations; both edge runs
            // have visible ink, and each adjacent complete interval measures a 0.15 step.
            cv::Mat wet(800,800,CV_8UC3,cv::Scalar::all(240));
            QJsonArray wetLabels;
            for(int k=0;k<=50;++k) {
                const double a=angle(k/50.);
                const bool numbered=k==0||k==10||k==40||k==50;
                cv::line(wet,point(a,numbered?256:k%5==0?267:282),point(a,300),{15,15,15},3);
                if(numbered) {
                    const auto p=point(a,220);
                    wetLabels.append(label(QString::number(.2+1.5*k/50.),p.x,p.y));
                }
            }
            liquid.y0=180;
            const auto partial=OcrDialRange::calibrate(wet,wetLabels,{400,400},360,clockwise,liquid);
            require(partial.valid()&&partial.unlabeledPoints.size()==4,"visible long ticks survive a masked middle");
            const double visibleRatios[]={.1,.3,.7,.9};
            for(int k=0;k<4;++k)
                require(std::abs(partial.unlabeledPoints[k].toObject()["ratio"].toDouble()-visibleRatios[k])<1e-6,
                    "partial interval uses the neighboring measured step in reading order");
            // The label for ratio .4 is visible below the liquid, while its
            // stroke is masked. Only the right neighbor supplies a counted step.
            auto broken=wet.clone();
            cv::line(broken,point(angle(.1),264),point(angle(.1),304),cv::Scalar::all(240),9);
            auto brokenLabels=wetLabels;
            const auto unmatched=point(angle(.4),205);
            brokenLabels.append(label("0.8",unmatched.x,unmatched.y)); // range .2..1.7
            const auto edges=OcrDialRange::calibrate(broken,brokenLabels,{400,400},360,clockwise,liquid);
            require(edges.valid()&&edges.points.size()==4&&edges.unlabeledPoints.size()==3,
                    "an unmatched label must isolate the break and preserve measured edge strokes");
            for(int k=0;k<3;++k)
                require(std::abs(edges.unlabeledPoints[k].toObject()["ratio"].toDouble()-visibleRatios[k+1])<1e-6,
                        "edge recovery must use the verified increment and leave the unmatched value open");
            cv::line(broken,point(angle(.3),264),point(angle(.3),304),cv::Scalar::all(240),9);
            const auto oneSided=OcrDialRange::calibrate(broken,brokenLabels,{400,400},360,clockwise,liquid);
            require(oneSided.valid()&&oneSided.unlabeledPoints.size()==2&&
                    std::abs(oneSided.unlabeledPoints.first().toObject()["ratio"].toDouble()-.7)<1e-6,
                    "an unmatched label and a missing left stroke must preserve the independently verified right side");
            cv::line(wet,point(angle(.3),264),point(angle(.3),304),cv::Scalar::all(240),9);
            const auto missing=OcrDialRange::calibrate(wet,wetLabels,{400,400},360,clockwise,liquid);
            require(missing.valid()&&missing.unlabeledPoints.size()==3&&
                std::abs(missing.unlabeledPoints[1].toObject()["ratio"].toDouble()-.7)<1e-6,
                "a missing edge stroke is not synthesized or borrowed from the other side");
        }
        LiquidSurface majorSurface;majorSurface.found=true;majorSurface.centerX=400;majorSurface.radius=360;majorSurface.y0=240;majorSurface.margin=10;
        const auto automatic=OcrDialRange::calibrate(majorImage(false),majorLabels,{400,400},360,true,majorSurface);
        require(automatic.valid()&&automatic.points.size()==4,"lower 0,0.2,0.8,1 must all match actual long ticks; upper 0.4,0.6 excluded");
        {
            const auto unchanged=EndpointAngleRectification::closeLoop(majorImage(false),majorLabels,{400,400},360,true,majorSurface,automatic);
            require(unchanged.converged&&!unchanged.refinement.valid(),"already orthogonal endpoints do not change geometry");
            for(double stretch:{.95,1.05}) {
                const cv::Mat warp=(cv::Mat_<double>(3,3)<<1,0,0,0,stretch,400*(1-stretch),0,0,1);
                cv::Mat distortedImage;cv::warpPerspective(majorImage(false),distortedImage,warp,{800,800},cv::INTER_LINEAR,cv::BORDER_CONSTANT,cv::Scalar::all(240));
                QJsonArray labels;
                for(auto entry:majorLabels) {auto token=entry.toObject();QJsonArray polygon;
                    for(auto vertex:token["polygon"].toArray()) {const auto xy=vertex.toArray();polygon.append(QJsonArray{xy[0].toDouble(),400+stretch*(xy[1].toDouble()-400)});}
                    token["polygon"]=polygon;labels.append(token);
                }
                auto liquid=majorSurface;liquid.y0=400+stretch*(liquid.y0-400);liquid.margin*=stretch;
                const auto initial=OcrDialRange::calibrate(distortedImage,labels,{400,400},360,true,liquid);
                require(initial.valid()&&initial.points.size()==4,"liquid leaves four reliable anchors for feedback");
                const auto closed=EndpointAngleRectification::closeLoop(distortedImage,labels,{400,400},360,true,liquid,initial);
                std::cout<<"Endpoint feedback: "<<closed.refinement.message.toStdString()<<"\n";
                require(closed.converged&&closed.refinement.valid(),"four-anchor liquid scene converges from either side of 90 degrees");
                auto finalCalibration=closed.calibration;
                finalCalibration.points=ScaleCalibration::transformed(finalCalibration.points,closed.refinement.correction);
                require(std::abs(EndpointAngleRectification::sweep(finalCalibration,{400,400},true)-270)<=.5,"freshly measured endpoint angle meets tolerance");
                auto endpointsOnly=initial;endpointsOnly.points=QJsonArray{initial.points.first(),initial.points.last()};
                require(!EndpointAngleRectification::closeLoop(distortedImage,labels,{400,400},360,true,liquid,endpointsOnly).converged,"two endpoints alone cannot validate local deformation");
            }
        }
        {
            for(double shear:{0.,-3.,3.}) {
                cv::Mat wet=majorImage(false);
                cv::rectangle(wet,{0,0},{799,239},{190,190,190},-1);
                cv::line(wet,{80,224},{720,256},{70,70,70},2);
                cv::Mat warp=(cv::Mat_<double>(3,3)<<1,0,0,shear,1.025,-10-400*shear,0,0,1);
                cv::getRotationMatrix2D(cv::Point2f(400,400),shear,1).copyTo(warp.rowRange(0,2));
                cv::Mat image;cv::warpPerspective(wet,image,warp,{800,800},cv::INTER_LINEAR,cv::BORDER_CONSTANT,cv::Scalar::all(240));
                QJsonArray labels;
                for(auto entry:majorLabels) {auto token=entry.toObject();QJsonArray polygon;
                    for(auto vertex:token["polygon"].toArray()){auto p=vertex.toArray();const auto q=cv::Matx33d(warp)*cv::Vec3d(p[0].toDouble(),p[1].toDouble(),1);polygon.append(QJsonArray{q[0],q[1]});}
                    token["polygon"]=polygon;labels.append(token);
                }
                const auto liquid=detectLiquidSurface(image,{400,400},360);
                require(liquid.found,"synthetic sloped liquid must provide reliable image evidence");
                const auto initial=OcrDialRange::calibrate(image,labels,{400,400},360,true,liquid);
                require(initial.valid(),"liquid fixture has reliable initial endpoints");
                const auto closed=EndpointAngleRectification::closeLoop(image,labels,{400,400},360,true,liquid,initial);
                require(closed.converged,"tick feedback must tolerate a tilted liquid edge");
                if(shear==0)require(!closed.refinement.valid(),"liquid tilt alone cannot change calibration");
                else {
                    require(closed.refinement.valid(),"printed anchors recover camera roll");
                    const cv::Mat A=closed.refinement.correction(cv::Rect(0,0,2,2));
                    require(cv::norm(A.t()*A-cv::Mat::eye(2,2,CV_64F))<1e-6,"roll preserves angles and lengths");
                }
            }
        }
        {
            cv::Mat dial(800,800,CV_8U,cv::Scalar(235));
            cv::line(dial,{319,640},{481,640},25,2);
            double tilt=0;
            require(!EndpointAngleRectification::batchUnderline(dial,{400,400},360,tilt),"isolated line is insufficient");
            cv::putText(dial,"2606007819",{326,635},cv::FONT_HERSHEY_SIMPLEX,.73,25,2);
            for(double rotation:{-4.,0.,4.}) {
                cv::Mat rotated;
                cv::warpAffine(dial,rotated,cv::getRotationMatrix2D({400,400},rotation,1),dial.size());
                require(EndpointAngleRectification::batchUnderline(rotated,{400,400},360,tilt)&&std::abs(tilt+rotation)<1,
                        "printed batch number identifies the signed underline angle");
            }
        }
        if(argc==6&&QString::fromLocal8Bit(argv[1])=="--batch-underline") {
            const auto photo=cv::imread(argv[2]);
            const cv::Point2f pivot(QString(argv[3]).toFloat(),QString(argv[4]).toFloat());
            double tilt=0;
            const bool found=EndpointAngleRectification::batchUnderline(photo,pivot,QString(argv[5]).toDouble(),tilt);
            std::cout<<"Batch underline found="<<found<<" angle="<<tilt<<" degrees\n";
            return found?0:1;
        }
        {
            QJsonArray visible;
            for(double r:{0.,.2,.5,.8,1.}) {
                const auto p=point(135+270*r+3,280);
                visible.append(QJsonObject{{"ratio",r},{"x",p.x},{"y",p.y}});
            }
            auto mirror=EndpointAngleRectification::mirrorEvidence(visible,{400,400});
            require(mirror.left==2&&mirror.right==2&&mirror.axisOffsets.size()==2&&
                std::abs(mirror.axisOffsets[0]-3)<.01&&std::abs(mirror.maxMismatch-6)<.01,"mirror pairs recover signed roll and exclude center mark");
            visible.removeAt(1);
            mirror=EndpointAngleRectification::mirrorEvidence(visible,{400,400});
            require(mirror.left==1&&mirror.right==2&&mirror.axisOffsets.size()==1,"occlusion must not create a mirror partner");
        }
        {
            const QJsonObject anchor{{"x",700},{"y",400}};
            const auto along=EndpointAngleRectification::displacement(anchor,QJsonObject{{"x",682},{"y",401}},{400,400});
            const auto across=EndpointAngleRectification::displacement(anchor,QJsonObject{{"x",700},{"y",417}},{400,400});
            require(along.consistent(360)&&!across.consistent(360),"along-stroke midpoint shift and neighboring ray must be distinguished");
        }
        ScaleCalibration::Mapping majorMapping;require(ScaleCalibration::build(automatic.points,{400,400},{},true,majorMapping),"automatic intermediate mapping");
        require(std::abs(automatic.points[1].toObject()["ratio"].toDouble()-.2)<1e-6&&distance(majorMapping.angles[1],189)<1.5,
            "0.2 must match its long tick rather than the nearby needle or shorter tick");
        require(std::abs(automatic.points[2].toObject()["ratio"].toDouble()-.8)<1e-6&&distance(majorMapping.angles[2],351)<1.5,"0.8 major tick must use measured angle");
        auto wideLabels=majorLabels;auto wide=wideLabels[4].toObject();
        wide["raw_text"]="0.8-";wide["polygon"]=QJsonArray{QJsonArray{607,339},QJsonArray{700,339},QJsonArray{700,394},QJsonArray{607,394}};wideLabels[4]=wide;
        auto wideImage=majorImage(false);
        // Match the long exposed stem measured in 12.png; the OCR box covers
        // most of that stem but its outer tip remains independently visible.
        cv::line(wideImage,point(351,275),point(351,320),{15,15,15},3);
        const auto recovered=OcrDialRange::calibrate(wideImage,wideLabels,{400,400},360,true,majorSurface);
        require(recovered.valid()&&recovered.points.size()==4,"OCR box containing 0.8 and part of its long stroke must not erase the graduation");
        require(OcrDialRange::exteriorStrokeBoundary({point(351,220),351,220,40},wideLabels,{400,400},360)<0,
            "glyph inside its own OCR box must not be restored as a graduation stem");
        const auto shiftedLabels=[&](double shift){auto labels=majorLabels;const auto p=point(351+shift,220);labels[4]=label("0.8",p.x,p.y);return labels;};
        require(OcrDialRange::calibrate(majorImage(false,-5.2),shiftedLabels(-5.2),{400,400},360,true,majorSurface).points.size()==4,
            "thick stroke edge uncertainty must be included near the five-degree model boundary");
        require(OcrDialRange::calibrate(majorImage(false,-9),shiftedLabels(-9),{400,400},360,true,majorSurface).points.size()==3,
            "stroke-width allowance must not admit a clearly inconsistent major angle");
        const auto withoutMajor=OcrDialRange::calibrate(majorImage(true),majorLabels,{400,400},360,true,majorSurface);
        require(withoutMajor.valid()&&withoutMajor.points.size()==3,"missing long tick must not be replaced by a short tick or a predicted angle");
        {
            // Oblique 0.8 has a thick head and a separate thin stem. The
            // short graduation below it is closer to the nominal angular grid.
            const cv::Point2f center(100,250),mid(855,70);
            auto splitMajor=[&](int lateralShift) {
                cv::Mat patch(350,1050,CV_8U,cv::Scalar(220));
                cv::fillConvexPoly(patch,std::vector<cv::Point>{{766,75},{852,64},{854,77},{768,88}},25);
                std::vector<cv::Point> stem{{861,66},{943,58},{944,65},{862,73}};
                for(auto& p:stem)p.y+=lateralShift;
                cv::fillConvexPoly(patch,stem,25);
                cv::line(patch,{858,161},{944,160},25,6);
                std::vector<OcrDialRange::MajorTick> ticks;
                OcrDialRange::majorTicks(patch,{},center,1037,{},nullptr,false,nullptr,&ticks);
                return std::any_of(ticks.begin(),ticks.end(),[&](const OcrDialRange::MajorTick& m){
                    return m.complete&&m.tick.length>160&&cv::norm(m.tick.point-mid)<8;
                });
            };
            require(splitMajor(0),"aligned head and stem must supply one measured long graduation");
            require(!splitMajor(18),"laterally displaced ink must not extend a short graduation");
        }
        majorSurface.ambiguous=true;
        require(!OcrDialRange::calibrate(majorImage(false),majorLabels,{400,400},360,true,majorSurface).valid(),"ambiguous liquid must prevent automatic scale matching");
        majorSurface.ambiguous=false;majorSurface.y0=370;
        require(OcrDialRange::calibrate(majorImage(false),majorLabels,{400,400},360,true,majorSurface).points.size()==2,"labels and ticks near or above liquid boundary must be excluded");
        std::vector<OcrDialRange::MajorTick> eccentricRing;
        for(int i=0;i<=50;++i) {
            const double a=(135+i*5.4)*CV_PI/180;const cv::Point2f tip(float(400+300*std::cos(a)),float(350+300*std::sin(a)));
            const auto d=tip-cv::Point2f(400,400);const double r=cv::norm(d),angle=wrap(std::atan2(d.y,d.x)*180/CV_PI);
            eccentricRing.push_back({{{400+float(d.x*(r-20)/r),400+float(d.y*(r-20)/r)},angle,r-20,40},6,3});
        }
        const auto observedRing=OcrDialRange::printedRing(eccentricRing,{400,400},360);
        require(observedRing.size.width>0&&cv::norm(observedRing.center-cv::Point2f(400,350))<.1,
            "local printed annulus must be fitted independently of the raised cap center");
        require(OcrDialRange::printedRing({}, {400,400},360).size.width==0,"without distributed stroke evidence, annulus fitting must remain disabled");
        const auto restoredDecimal=PaddleScaleOcr::numericTokens(QJsonArray{label("02",200,400)});
        require(restoredDecimal.size()==1&&restoredDecimal[0].toObject()["text"].toString()=="0.2"&&restoredDecimal[0].toObject()["raw_text"].toString()=="02","missing decimal dot must preserve raw OCR while selecting the built-in dial value");
        // Known circle and graduations: perturb the rim fit while retaining
        // independent gradient evidence, then measure physical point recovery.
        // A dark decorative band lies between the paper face and a thin metal
        // lip. The old fixed inner sample rejects this otherwise complete rim.
        cv::Mat decorative(800,800,CV_8U,cv::Scalar(20));
        cv::circle(decorative,{400,400},300,cv::Scalar(85),-1);
        cv::circle(decorative,{400,400},294,cv::Scalar(25),-1);
        cv::circle(decorative,{400,400},276,cv::Scalar(120),-1);
        cv::Mat decorativeGx,decorativeGy;cv::Scharr(decorative,decorativeGx,CV_32F,1,0);cv::Scharr(decorative,decorativeGy,CV_32F,0,1);
        require(AutoDialCalibration::rimEdgeCoverage(cv::RotatedRect({400,400},{600,600},0),decorativeGx,decorativeGy)>.9,
            "decorative-lip fixture must retain independently measured circular edge support");
        int oldContrast=0,bandContrast=0;
        for(int i=0;i<72;++i) {
            const cv::Vec2d v(300*std::cos(i*CV_PI/36),300*std::sin(i*CV_PI/36));
            oldContrast+=decorative.at<uchar>(cvRound(400+v[1]*.96),cvRound(400+v[0]*.96))-20>12;
            bandContrast+=AutoDialCalibration::rimInteriorBrightness(decorative,{400,400},v)-20>12;
        }
        require(oldContrast<22&&bandContrast>=60,"interior band must recover a complete rim obscured only by the dark decorative lip");
        cv::Mat singleReflection(100,100,CV_8U,cv::Scalar(20));singleReflection.at<uchar>(50,82)=240;
        require(AutoDialCalibration::rimInteriorBrightness(singleReflection,{10,50},{80,0})==20,
            "one isolated interior reflection must not provide valid face contrast");
        cv::Mat synthetic(800,800,CV_8UC3,cv::Scalar(45,45,45));
        cv::circle(synthetic,{400,400},300,{10,10,10},-1);
        cv::circle(synthetic,{400,400},292,{235,235,235},-1);
        std::vector<cv::Vec4f> graduationLines;
        for(int i=0;i<=60;++i) {
            const double angle=135+i*4.5;const auto a=point(angle,245),b=point(angle,275);
            graduationLines.push_back({a.x,a.y,b.x,b.y});cv::line(synthetic,a,b,{15,15,15},3);
        }
        cv::Mat synthGray,synthGx,synthGy;cv::cvtColor(synthetic,synthGray,cv::COLOR_BGR2GRAY);
        cv::Scharr(synthGray,synthGx,CV_32F,1,0);cv::Scharr(synthGray,synthGy,CV_32F,0,1);
        const cv::RotatedRect brokenSeed({378,400},{550,570},0);
        const auto recoveredRim=AutoDialCalibration::recoverRim(brokenSeed,synthGx,synthGy);
        require(AutoDialCalibration::rimEdgeCoverage(recoveredRim,synthGx,synthGy)>.9&&cv::norm(recoveredRim.center-cv::Point2f(400,400))<3,
            "nearby multi-arc recovery must recover independently drawn circular boundary evidence");
        const cv::Mat blankGradient=cv::Mat::zeros(synthGray.size(),CV_32F);
        require(AutoDialCalibration::recoverRim(brokenSeed,blankGradient,blankGradient).center==brokenSeed.center,
            "without circular gradient evidence, recovery must not invent a boundary");
        const std::vector<cv::Point2f> rimHandles{{400,108},{692,400},{400,692},{108,400}};
        const cv::RotatedRect measuredRim({400,400},{584,584},0);
        require(AutoDialCalibration::supportedMappingHandles(rimHandles,rimHandles,measuredRim,{800,800}),"measured supported handles must remain valid");
        auto extrapolated=rimHandles;extrapolated[0].y=1;
        require(!AutoDialCalibration::supportedMappingHandles(extrapolated,rimHandles,measuredRim,{800,800}),
            "a top handle extrapolated to the image border must be rejected even when inside the image");
        const cv::RotatedRect biasedRim({401,400},{584,600},0);
        const auto enclosingH=AutoDialCalibration::metricHomography(cv::RotatedRect({400,400},{600,600},0),{400,400});
        const auto observedTips=AutoDialCalibration::graduationEnvelope(graduationLines,enclosingH);
        const auto cuttingH=AutoDialCalibration::metricHomography(cv::RotatedRect({400,400},{548,548},0),{400,400});
        require(observedTips.size()>=24&&AutoDialCalibration::graduationEnvelopeRadius(observedTips,enclosingH)<.95&&
            AutoDialCalibration::graduationEnvelopeRadius(observedTips,cuttingH)>.97,
            "the same frozen graduation tips must distinguish the enclosing rim from a smaller boundary that cuts through ticks");
        const auto initialFit=AutoDialCalibration::graduationFit(graduationLines,AutoDialCalibration::metricHomography(biasedRim,{400,400}));
        const auto betterRim=AutoDialCalibration::refinePrintedGeometry(biasedRim,{400,400},synthGx,synthGy,graduationLines);
        const auto betterFit=AutoDialCalibration::graduationFit(graduationLines,AutoDialCalibration::metricHomography(betterRim,{400,400}));
        require(betterFit.aspect>initialFit.aspect+.008&&betterFit.aspect>.985,"printed circle evidence must correct biased rim aspect without changing pivot");
        cv::circle(synthetic,{400,400},15,{20,20,20},-1);cv::circle(synthetic,{400,400},10,{120,120,120},-1);
        cv::line(synthetic,{165,400},{387,400},{5,5,5},5);cv::line(synthetic,{415,400},{480,400},{5,5,5},14);
        const cv::Mat knownProjection=(cv::Mat_<double>(3,3)<<1,.05,0,.02,.65,40,.00015,.00035,1);
        const cv::Mat normalize=(cv::Mat_<double>(3,3)<<1./300,0,-400./300,0,1./300,-400./300,0,0,1);
        const cv::Mat bias=(cv::Mat_<double>(3,3)<<.95,.02,.015,.005,1.04,.025,.015,-.02,1);
        const cv::Mat initial=bias*normalize*knownProjection.inv();
        // A raised hub and a printed origin may be distinct in an oblique view.
        const cv::Point2f raisedHub(413,388),printedOrigin(400,400);
        std::vector<cv::Point2f> centers{raisedHub,printedOrigin};
        cv::perspectiveTransform(centers,centers,knownProjection);
        const cv::Mat printWarp=normalize*knownProjection.inv();
        const auto hubWarp=AutoDialCalibration::anchorHomography(printWarp,centers[0]);
        const std::vector<cv::Point2f> unitHandles{{0,-1},{1,0},{0,1},{-1,0}};
        std::vector<cv::Point2f> handles;
        cv::perspectiveTransform(unitHandles,handles,hubWarp.inv());
        require(AutoDialCalibration::diametersMeetAt(handles,centers[0]) &&
                !AutoDialCalibration::diametersMeetAt(handles,centers[1]),
                "both source diameters must cross the measured hub under perspective");
        for(int i=0;i<2;++i) {
            auto shifted=handles;
            shifted[i]+=cv::Point2f(9,11);
            require(!AutoDialCalibration::diametersMeetAt(shifted,centers[0]),
                    "one accurate diameter cannot hide an off-center opposite pair");
        }
        cv::perspectiveTransform(centers,centers,hubWarp);
        require(cv::norm(centers[0])<1e-5 && cv::norm(centers[1]-(printedOrigin-raisedHub)/300)<1e-5,
                "centering the handles must preserve the independent printed origin");
        auto projectedLines=graduationLines;
        for(auto& line:projectedLines) {
            std::vector<cv::Point2f> p{{line[0],line[1]},{line[2],line[3]}};cv::perspectiveTransform(p,p,knownProjection);
            line={p[0].x,p[0].y,p[1].x,p[1].y};
        }
        const auto structural=GraduationRectification::fit(projectedLines,initial,
            [](const cv::RotatedRect& ellipse,cv::Point2f center){return AutoDialCalibration::metricHomography(ellipse,center);});
        require(structural.valid()&&structural.support>=50,"multi-feature fit must recover known biased projective geometry");
        std::vector<cv::Point2f> check;for(int i=0;i<36;++i)check.push_back(point(i*10,275));
        cv::perspectiveTransform(check,check,structural.correction*initial*knownProjection);
        double structuralError=0;for(int i=0;i<36;++i)structuralError+=std::pow(distance(wrap(std::atan2(check[i].y,check[i].x)*180/CV_PI),i*10),2);
        structuralError=std::sqrt(structuralError/36);
        require(structuralError<.1,"independent physical angles must be recovered, rather than merely making an ellipse circular");
        require(!GraduationRectification::fit(std::vector<cv::Vec4f>(projectedLines.begin(),projectedLines.begin()+12),initial,
            [](const cv::RotatedRect& ellipse,cv::Point2f center){return AutoDialCalibration::metricHomography(ellipse,center);}).valid(),"partial single arc cannot justify structural rectification");
        const double shift=.065, ringRadius=275./300;
        const cv::Mat circularBias=(cv::Mat_<double>(3,3)<<std::sqrt(1-shift*shift),0,0,
            0,1,shift*ringRadius,0,shift/ringRadius,1);
        const auto spanOnly=GraduationRectification::fit(graduationLines,circularBias*normalize,
            [](const cv::RotatedRect& ellipse,cv::Point2f center){return AutoDialCalibration::metricHomography(ellipse,center);});
        require(spanOnly.valid()&&spanOnly.before<.001&&std::abs(spanOnly.spanBefore-270)>3&&
            std::abs(spanOnly.spanAfter-270)<.1,"endpoint feedback must recover a biased span even on a circular ring");
        require(!GraduationRectification::fit(graduationLines,normalize,
            [](const cv::RotatedRect& ellipse,cv::Point2f center){return AutoDialCalibration::metricHomography(ellipse,center);}).valid(),
            "a circular ring with correct endpoint span must keep its original handles");
        auto non270=graduationLines;
        for(int i=0;i<int(non270.size());++i) {
            std::vector<cv::Point2f> p{point(150+i*4.,245),point(150+i*4.,275)};cv::perspectiveTransform(p,p,knownProjection);
            non270[i]={p[0].x,p[0].y,p[1].x,p[1].y};
        }
        require(!GraduationRectification::fit(non270,initial,
            [](const cv::RotatedRect& ellipse,cv::Point2f center){return AutoDialCalibration::metricHomography(ellipse,center);}).valid(),"a measured 240-degree arc must not be forcibly stretched to 270 degrees");
        auto planeEndpoints=matchedEndpoints.points;
        for(int i=0;i<planeEndpoints.size();++i){auto p=planeEndpoints[i].toObject();p["origin_x"]=400.;p["origin_y"]=400.;planeEndpoints[i]=p;}
        ScaleCalibration::Mapping planeMapping;
        ScaleCalibration::Mapping pointerMapping,legacyMapping;
        auto hubEndpoints=planeEndpoints;
        for(int i=0;i<hubEndpoints.size();++i){auto p=hubEndpoints[i].toObject();p.remove("origin_x");p.remove("origin_y");hubEndpoints[i]=p;}
        require(ScaleCalibration::buildForPointer(planeEndpoints,{400,425},{},true,pointerMapping)&&
            ScaleCalibration::build(hubEndpoints,{400,425},{},true,legacyMapping)&&
            pointerMapping.angles==legacyMapping.angles,
            "pointer mapping must measure scale angles from the needle hub regardless of printed origins");
        const auto projectedPointerEndpoints=ScaleCalibration::transformed(planeEndpoints,knownProjection);
        require(ScaleCalibration::buildForPointer(projectedPointerEndpoints,{400,425},knownProjection.inv(),true,pointerMapping),
            "pointer mapping must accept persisted projected scale points");
        for(size_t i=0;i<pointerMapping.angles.size();++i)
            require(AutoScaleCalibration::distance(pointerMapping.angles[i],legacyMapping.angles[i])<.001,
                "homography must transform scale positions but not transform the destination hub twice");
        require(ScaleCalibration::build(planeEndpoints,{400,425},{},true,planeMapping)&&std::abs(planeMapping.offsets.back()-270)<.01,
            "raised cap offset must not change the printed scale angles");
        const auto projectedEndpoints=ScaleCalibration::transformed(planeEndpoints,knownProjection);
        require(ScaleCalibration::build(projectedEndpoints,{400,425},knownProjection.inv(),true,planeMapping)&&std::abs(planeMapping.offsets.back()-270)<.01,
            "scale origin must survive persisted homography transformations");
        const auto lensRoundTrip=CameraUndistortion::scales(CameraUndistortion::scales(planeEndpoints,{800,800},true),{800,800});
        require(ScaleCalibration::build(lensRoundTrip,{400,425},{},true,planeMapping)&&std::abs(planeMapping.offsets.back()-270)<.02,
            "scale origin must survive raw camera storage and lens correction");
        const auto resizedEndpoints=ScaleCalibration::scaled(planeEndpoints,2,2);
        require(ScaleCalibration::build(resizedEndpoints,{800,850},{},true,planeMapping)&&std::abs(planeMapping.offsets.back()-270)<.01,"scale origin must resize with frame resolution");
        std::cout<<"Multi-feature independent angle RMSE="<<structuralError<<" degrees\n";
        // Printed loops must not borrow disconnected strokes as needle arms.
        const cv::RotatedRect cap({100,100},{20,20},0);
        cv::Mat detached(200,200,CV_8UC1,cv::Scalar(210));
        cv::circle(detached,{100,100},10,cv::Scalar(20),-1);
        cv::line(detached,{120,100},{146,100},cv::Scalar(20),3);
        cv::line(detached,{54,100},{80,100},cv::Scalar(20),3);
        require(AutoDialCalibration::needleSpokes(detached,cap,100)==0,
            "separated text/tick strokes cannot establish an axis cap");
        cv::line(detached,{109,100},{120,100},cv::Scalar(20),3);
        cv::line(detached,{80,100},{91,100},cv::Scalar(20),3);
        require(AutoDialCalibration::needleSpokes(detached,cap,100)>0,
            "continuous opposing needle arms establish an axis cap");
        cv::Mat oblique;cv::warpPerspective(synthetic,oblique,knownProjection,{800,600});
        const auto autoDial=AutoDialCalibration::detect(oblique);
        require(autoDial.valid(),"automatic four-point detection on known projected dial");
        require(AutoDialCalibration::diametersMeetAt(autoDial.points,autoDial.hub),
                "automatic refinement must preserve both projected diameters through the detected hub");
        const auto corrected=cv::getPerspectiveTransform(autoDial.points,std::vector<cv::Point2f>{{400,40},{760,400},{400,760},{40,400}});
        std::vector<cv::Point2f> reference;for(int i=0;i<36;++i)reference.push_back(point(i*10,275));
        std::vector<cv::Point2f> measured;cv::perspectiveTransform(reference,measured,corrected*knownProjection);
        std::vector<cv::Point2f> pivot;cv::perspectiveTransform(std::vector<cv::Point2f>{{400,400}},pivot,corrected*knownProjection);
        const auto first=measured[0]-pivot[0];const double rotation=std::atan2(first.y,first.x)*180/CV_PI;
        double angularError=0;for(int i=0;i<36;++i) {const auto d=measured[i]-pivot[0];angularError+=std::pow(distance(wrap(std::atan2(d.y,d.x)*180/CV_PI-rotation),i*10),2);}
        angularError=std::sqrt(angularError/36);
        std::cout<<"Projected dial angle RMSE="<<angularError<<" degrees; biased aspect "<<initialFit.aspect<<" -> "<<betterFit.aspect<<'\n';
        require(angularError<2.5,"four-point rectification must preserve known physical angles under perspective");
        require(AutoDialCalibration::refinePrintedGeometry(biasedRim,{400,400},synthGx,synthGy,{}).size==biasedRim.size,
            "without graduation evidence, refinement must retain the measured rim");
        cv::Mat highResolution;cv::resize(oblique,highResolution,{},3,3,cv::INTER_CUBIC);
        const auto highDial=AutoDialCalibration::detect(highResolution);
        require(highDial.valid()&&highDial.graduationSupport>=24,"high-resolution coarse detection and fine rim refinement");
        double cornerError=0;for(int i=0;i<4;++i)cornerError+=cv::norm(highDial.points[i]/3-autoDial.points[i]);
        require(cornerError/4<2,"high-resolution refinement must retain source-coordinate consistency");
        cv::Mat twoDials;cv::hconcat(synthetic,synthetic,twoDials);
        require(!AutoDialCalibration::detect(twoDials).valid(),"graduation quality must not hide ambiguity between two physical dials");
        AutoDialCalibration::GraduationExclusion liquidMask;
        liquidMask.sourceToPreview=cv::Mat::eye(3,3,CV_64F);
        liquidMask.surface.found=true;liquidMask.surface.centerX=400;liquidMask.surface.radius=360;
        liquidMask.surface.y0=190;liquidMask.surface.margin=12;
        auto withInterference=graduationLines;
        withInterference.push_back({300,175,330,180}); // Above the surface.
        withInterference.push_back({300,195,330,200}); // Buffer below the surface.
        withInterference.push_back({300,200,300,225}); // Crosses the buffer.
        const auto safeEdges=AutoDialCalibration::reliableGraduationEdges(withInterference,liquidMask);
        const auto withoutInterference=AutoDialCalibration::reliableGraduationEdges(graduationLines,liquidMask);
        require(safeEdges.size()==withoutInterference.size(),"adding upper liquid interference must not change accepted graduation evidence");
        for(size_t i=0;i<safeEdges.size();++i)require(cv::norm(safeEdges[i]-withoutInterference[i])==0,"excluded interference must never enter the geometry fit");
        require(safeEdges.size()<graduationLines.size()&&safeEdges.size()>=24,"liquid exclusion keeps enough reliable lower graduations");
        for(const auto& line:safeEdges)require(line[1]>204&&line[3]>204,"all edges above, near or crossing the liquid boundary excluded");
        require(AutoDialCalibration::graduationFit(safeEdges,AutoDialCalibration::metricHomography(cv::RotatedRect({400,400},{600,600},0),{400,400})).support>=24,
            "below-liquid tick support remains sufficient for circle fitting");
        const auto partialFit=AutoDialCalibration::refinePrintedGeometry(biasedRim,{400,400},synthGx,synthGy,safeEdges,.2);
        require(std::abs(partialFit.size.width/biasedRim.size.width-1)<=.010001&&std::abs(partialFit.size.height/biasedRim.size.height-1)<=.010001,
            "partial-circle liquid evidence must not drive a large perspective change");
        auto scaledEdges=withInterference;for(auto& line:scaledEdges)line*=2;
        require(AutoDialCalibration::reliableGraduationEdges(scaledEdges,liquidMask,2).size()==safeEdges.size(),"fine-resolution filtering uses the same frozen liquid boundary");
        liquidMask.surface.found=false;liquidMask.surface.ambiguous=true;
        require(AutoDialCalibration::reliableGraduationEdges(withInterference,liquidMask).empty(),"ambiguous liquid detection disables graduation-assisted refinement");
        liquidMask.surface.found=true;liquidMask.surface.ambiguous=false;liquidMask.surface.curve=72;
        require(AutoDialCalibration::reliableGraduationEdges({{700,230,730,235},{700,280,730,285}},liquidMask).size()==1,
            "curved liquid boundary excludes local upper interference instead of a flat cutoff");
        liquidMask.surface.curve=0;liquidMask.sourceToPreview=(cv::Mat_<double>(3,3)<<1,0,0,0,1,100,0,0,1);
        require(AutoDialCalibration::reliableGraduationEdges({{100,100,120,110},{100,130,120,150}},liquidMask).size()==1,
            "liquid exclusion is evaluated after mapping source edges into the preliminary rectified view");
        std::cout<<"PASS: tick pixels, rotation, CW/CCW, missing endpoints, outliers, ambiguity, parsing\n";
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
