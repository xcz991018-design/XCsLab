#include "MinorTickPreview.hpp"
#include "AutoDialCalibration.hpp"
#include "CameraUndistortion.hpp"
#include "OcrDialRange.hpp"
#include <iostream>
#include <QJsonDocument>
int main(int argc,char** argv){
    if((argc==5||argc==6)&&std::string(argv[1])=="--calibrated") {
        auto image=cv::imread(argv[2]);const float s=image.cols*.5f;const cv::Point2f center(s,s);const double radius=s*.9;
        QFile input(argv[3]);if(!input.open(QIODevice::ReadOnly))return 80;
        auto tokens=QJsonDocument::fromJson(input.readAll()).object()["tokens"].toArray();auto surface=detectLiquidSurface(image,center,radius);
        auto c=OcrDialRange::calibrate(image,tokens,center,radius,true,surface);if(!c.valid())return 81;
        QJsonArray diagnostics;
        auto r=MinorTickPreview::detect(image,c.points,center,radius,0,1,true,surface,argc==6?std::stod(argv[5]):182.74,0,&diagnostics);
        std::cout<<r.message.toStdString()<<'\n';
        QJsonArray marks;int assigned=0;
        for(const auto& m:r.marks) {
            const auto p=(m.a+m.b)*.5f;
            marks.append(QJsonObject{{"x",p.x},{"y",p.y},{"ratio",m.ratio},{"assigned",m.assigned},{"gap",m.gap}});
            if(m.assigned){++assigned;if(m.gap)return 83;}
        }
        QFile out(argv[4]);out.open(QIODevice::WriteOnly);out.write(QJsonDocument(QJsonObject{{"anchors",c.points},{"points",r.previewPoints},{"marks",marks},{"division",r.division},{"confirmed",r.divisionConfirmed},{"diagnostics",diagnostics}}).toJson());
        if(r.divisionConfirmed&&r.previewPoints.size()!=c.points.size()+assigned)return 84;
        return r.divisionConfirmed?0:82;
    }
    if(argc==4&&std::string(argv[1])=="--seven") {
        auto raw=CameraUndistortion::image(cv::imread(argv[2]));auto dial=AutoDialCalibration::detect(raw);if(!dial.valid())return 70;
        auto H=cv::getPerspectiveTransform(dial.points,std::vector<cv::Point2f>{{405,40.5f},{769.5f,405},{405,769.5f},{40.5f,405}});
        cv::Mat image;cv::warpPerspective(raw,image,H,{810,810},cv::INTER_CUBIC);
        const cv::Point2f center(405,405);QJsonArray anchors;
        for(const auto& p:std::vector<cv::Point3d>{{207,568,0},{132,348,.2},{672,346,.8},{578,578,1}})
            anchors.append(QJsonObject{{"x",p.x},{"y",p.y},{"ratio",p.z}});
        auto surface=detectLiquidSurface(image,center,364.5);
        QJsonArray tokens;
        for(const auto& p:std::vector<cv::Point3d>{{250,525,0},{201,354,.2},{604,354,.8},{552,525,1}})
            tokens.append(QJsonObject{{"text",QString::number(p.z)},{"score",.99},{"polygon",QJsonArray{QJsonArray{p.x-18,p.y-25},QJsonArray{p.x+18,p.y-25},QJsonArray{p.x+18,p.y+25},QJsonArray{p.x-18,p.y+25}}}});
        const auto calibration=OcrDialRange::calibrate(image,tokens,center,364.5,true,surface);
        if(!calibration.valid())return 73;
        anchors=calibration.points;
        std::cout<<"surface "<<surface.found<<" ambiguous "<<surface.ambiguous<<" y "<<surface.y0<<"\n";
        auto r=MinorTickPreview::detect(image,anchors,center,364.5,0,1,true,surface);
        std::cout<<r.message.toStdString()<<"\n";
        int upper=0;std::set<int> upperValues;
        for(const auto& m:r.marks)if(!m.gap) {
            for(int i=0;i<=8;++i){auto p=m.a+(m.b-m.a)*(i/8.f);if(surface.excludes(p.x,p.y))return 71;}
            if(m.assigned&&m.ratio>.2&&m.ratio<.8){++upper;upperValues.insert(cvRound(m.ratio*100));}
            std::cout<<(m.a+m.b)*.5f<<" value "<<m.ratio<<" assigned "<<m.assigned<<"\n";
            cv::line(image,m.a,m.b,m.assigned?cv::Scalar(255,255,0):cv::Scalar(255,0,255),2);
            cv::putText(image,MinorTickPreview::displayValue(m.ratio).toStdString(),(m.a+m.b)*.5f,cv::FONT_HERSHEY_SIMPLEX,.4,{0,0,255},1);
        }
        cv::imwrite(argv[3],image);
        const std::set<int> required{22,24,26,28,30,70,72,74,76,78};
        return surface.found&&r.divisionConfirmed&&upper>=10&&std::includes(upperValues.begin(),upperValues.end(),required.begin(),required.end())?0:72;
    }
    if(argc==4&&(std::string(argv[1])=="--five"||std::string(argv[1])=="--five-raw")) {
        auto full=cv::imread(argv[2]);cv::Mat image;
        if(std::string(argv[1])=="--five-raw") {
            auto raw=CameraUndistortion::image(full);auto dial=AutoDialCalibration::detect(raw);if(!dial.valid())return 60;
            auto H=cv::getPerspectiveTransform(dial.points,std::vector<cv::Point2f>{{446.5f,44.65f},{848.35f,446.5f},{446.5f,848.35f},{44.65f,446.5f}});
            cv::warpPerspective(raw,image,H,{893,893},cv::INTER_CUBIC);
        } else {if(full.cols<1818||full.rows<1016)return 60;image=full(cv::Rect(925,123,893,893)).clone();}
        const cv::Point2f center(447,447);QJsonArray anchors;
        for(const auto& p:std::vector<cv::Point3d>{{233,631,0},{143,384,.2},{739,381,.8},{638,636,1}})
            anchors.append(QJsonObject{{"x",p.x},{"y",p.y},{"ratio",p.z}});
        LiquidSurface surface;surface.found=true;surface.centerX=447;surface.radius=405;surface.y0=188;surface.slope=-13;surface.margin=8;
        auto r=MinorTickPreview::detect(image,anchors,center,405,0,1,true,surface);
        std::cout<<r.message.toStdString()<<" confirmed="<<r.divisionConfirmed<<"\n";
        for(const auto& m:r.marks)std::cout<<(m.a+m.b)*.5f<<" "<<m.ratio<<" "<<m.assigned<<"\n";
        cv::imwrite(argv[3],image);
        const int assigned=int(std::count_if(r.marks.begin(),r.marks.end(),[](const MinorTickPreview::Mark& m){return m.assigned;}));
        return r.divisionConfirmed&&std::abs(r.division-.02)<1e-6&&assigned>=10?0:61;
    }
    if(argc==4&&std::string(argv[1])=="--masked-screenshot") {
        auto full=cv::imread(argv[2]);if(full.cols<1617||full.rows<917)return 50;
        auto image=full(cv::Rect(824,124,793,793)).clone();
        const cv::Point2f center(396,396);QJsonArray anchors;
        for(const auto& p:std::vector<cv::Point3d>{{206,562,0},{129,340,.2},{658,339,.8},{565,566,1}})
            anchors.append(QJsonObject{{"x",p.x},{"y",p.y},{"ratio",p.z}});
        LiquidSurface surface;surface.found=true;surface.centerX=396;surface.radius=360;surface.y0=150;surface.slope=-13;surface.margin=0;
        auto result=MinorTickPreview::detect(image,anchors,center,360,0,1,true,surface,std::numeric_limits<double>::quiet_NaN(),.02);
        bool left=false,right=false;
        for(const auto& m:result.marks)if(!m.gap) {
            const auto p=(m.a+m.b)*.5f;
            std::cout<<p<<" value="<<m.ratio<<" assigned="<<m.assigned<<'\n';
            if(cv::norm(p-cv::Point2f(166,195))<18){left=true;if(std::abs(m.ratio-.3)>1e-6)return 51;}
            if(cv::norm(p-cv::Point2f(627,197))<18){right=true;if(std::abs(m.ratio-.7)>1e-6)return 52;}
            cv::putText(image,MinorTickPreview::displayValue(m.ratio).toStdString(),p,cv::FONT_HERSHEY_SIMPLEX,.4,{0,0,255},1);
        }
        cv::imwrite(argv[3],image);return left&&right?0:53;
    }
    if(argc==4&&(std::string(argv[1])=="--raw"||std::string(argv[1])=="--234")) {
        auto raw=CameraUndistortion::image(cv::imread(argv[2]));if(raw.empty())return 10;
        auto dial=AutoDialCalibration::detect(raw);if(!dial.valid()){std::cout<<dial.error.toStdString();return 11;}
        auto H=cv::getPerspectiveTransform(dial.points,std::vector<cv::Point2f>{{400,40},{760,400},{400,760},{40,400}});
        cv::Mat image;cv::warpPerspective(raw,image,H,{800,800},cv::INTER_CUBIC);
        cv::Point2f center(400,400);if(dial.structuralRectification){std::vector<cv::Point2f> p{dial.graduationCenter};cv::perspectiveTransform(p,p,H);center=p[0];}
        QJsonArray anchors;for(const auto& p:std::vector<cv::Point3d>{{210,561,0},{130,342,.2},{664,342,.8},{570,569,1}})
            anchors.append(QJsonObject{{"x",p.x},{"y",p.y},{"ratio",p.z},{"origin_x",center.x},{"origin_y",center.y}});
        if(std::string(argv[1])=="--234") {
            anchors={};for(const auto& p:std::vector<cv::Point3d>{{190,560,0},{130.859,329.252,.2},{683.6,351.078,.8},{600,610,1}})
                anchors.append(QJsonObject{{"x",p.x},{"y",p.y},{"ratio",p.z},{"origin_x",center.x},{"origin_y",center.y}});
        }
        auto surface=detectLiquidSurface(image,{400,400},360);
        auto result=MinorTickPreview::detect(image,anchors,{400,400},360,0,1,true,surface,std::numeric_limits<double>::quiet_NaN(),0);
        auto manual=MinorTickPreview::detect(image,anchors,{400,400},360,0,1,true,surface,std::numeric_limits<double>::quiet_NaN(),.02);
        if(std::string(argv[1])=="--234") {
            cv::imwrite(argv[3],image);std::cout<<result.message.toStdString()<<std::endl;
            if(!result.divisionConfirmed||std::abs(result.division-.02)>1e-8||result.previewPoints.size()<anchors.size()+10)return 42;
            bool left=false,right=false;
            for(const auto& m:result.marks)if(m.assigned){const auto p=(m.a+m.b)*.5f;
                if(cv::norm(p-cv::Point2f(140.12f,469.17f))<3){left=true;if(std::abs(m.ratio-.1)>1e-8)return 43;}
                if(cv::norm(p-cv::Point2f(677.13f,498.28f))<3){right=true;if(std::abs(m.ratio-.9)>1e-8)return 44;}}
            if(!left||!right)return 45;
            return 0;
        }
        bool middleFound=false,previousFound=false,nextFound=false;
        for(const auto& m:manual.marks)if(!m.gap){const auto p=(m.a+m.b)*.5f-center;
            const double angle=AutoScaleCalibration::wrap(std::atan2(p.y,p.x)*180/CV_PI);
            if(std::abs(angle-163.34)<1){middleFound=true;if(std::abs(m.ratio-.1)>.00001||!m.assigned)return 24;}
            if(std::abs(angle-157.60)<1){previousFound=true;if(std::abs(m.ratio-.08)>.00001)return 25;}
            if(std::abs(angle-168.34)<1){nextFound=true;if(std::abs(m.ratio-.12)>.00001)return 26;}
        }
        if(!middleFound||!previousFound||!nextFound)return 27;
        for(const auto& range:std::vector<cv::Point2d>{{0,1.6},{0,10},{-1,1},{10,110}}) {
            const auto scaled=MinorTickPreview::detect(image,anchors,{400,400},360,range.x,range.y,true,surface);
            if(!scaled.divisionConfirmed||std::abs(scaled.division-.02*(range.y-range.x))>1e-6)return 28;
            bool found=false;for(const auto& m:scaled.marks)if(!m.gap){const auto p=(m.a+m.b)*.5f-center;
                if(std::abs(AutoScaleCalibration::wrap(std::atan2(p.y,p.x)*180/CV_PI)-163.34)<1){found=true;if(std::abs(m.ratio-.1)>1e-6)return 29;}}
            if(!found)return 30;
        }
        std::cout<<"Manual division: "<<manual.message.toStdString()<<std::endl;
        if(!manual.divisionConfirmed||manual.previewPoints.size()<anchors.size()+6)return 19;
        for(auto m:result.marks)cv::line(image,m.a,m.b,m.gap?cv::Scalar(0,150,255):cv::Scalar(255,0,255),2);
        cv::imwrite(argv[3],image);std::cout<<"center="<<center<<" surface="<<surface.found<<" "<<result.message.toStdString()<<std::endl;
        return result.marks.size()<24?12:0;
    }
    if(argc==3) {
        auto full=cv::imread(argv[1]);if(full.empty())return 10;
        const cv::Rect crop(941,100,855,855);if(full.cols<crop.br().x||full.rows<crop.br().y)return 11;
        auto image=full(crop).clone();const cv::Point2f hub(427,428);QJsonArray anchors;
        for(const auto& p:std::vector<cv::Point3d>{{222,604,0},{140,365,.2},{708,364,.8},{610,610,1}})
            anchors.append(QJsonObject{{"x",p.x},{"y",p.y},{"ratio",p.z}});
        LiquidSurface surface;surface.found=true;surface.centerX=427;surface.radius=400;surface.y0=177;surface.slope=-20;surface.curve=4;surface.margin=8;
        auto result=MinorTickPreview::detect(image,anchors,hub,387,0,1,true,surface);
        for(auto m:result.marks)cv::line(image,m.a,m.b,m.gap?cv::Scalar(0,150,255):cv::Scalar(255,0,255),2);
        cv::imwrite(argv[2],image);std::cout<<result.message.toStdString()<<std::endl;return result.marks.size()<24||result.marks.size()>40||std::abs(result.division-.02)>.001?12:0;
    }

    if(MinorTickPreview::decimalDivision(.2/9)||MinorTickPreview::decimalDivision(.2/6)||
       !MinorTickPreview::decimalDivision(.02)||!MinorTickPreview::decimalDivision(.025)||
       !MinorTickPreview::decimalDivision(.03)||!MinorTickPreview::decimalDivision(.125)||
       MinorTickPreview::displayValue(.12000000001)!="0.12")return 39;
    const cv::Point2f center(300,300);const double radius=270;
    auto point=[&](double a,double r){return center+cv::Point2f(float(r*cos(a*CV_PI/180)),float(r*sin(a*CV_PI/180)));};
    for(bool clockwise:{true,false})for(bool missing:{false,true})for(bool shadedOblique:{false,true}) {
        cv::Mat image(600,600,CV_8UC1,cv::Scalar(shadedOblique?115:255));QJsonArray anchors;
        for(int i=0;i<=20;++i){double a=clockwise?20+i*3:100-i*3;
            if(!(missing&&i==7)){
                const cv::Point2f tangent(float(-sin(a*CV_PI/180)),float(cos(a*CV_PI/180)));
                const auto skew=shadedOblique?tangent*.9f:cv::Point2f();
                cv::line(image,point(a,i%10?220:205)-skew,point(a,244)+skew,cv::Scalar(shadedOblique?65:0),2,cv::LINE_AA);
            }
            if(i%10==0){auto p=point(a,230);anchors.append(QJsonObject{{"x",p.x},{"y",p.y},{"ratio",i/20.}});}}
        if(!missing&&!shadedOblique) {
            const auto clean=MinorTickPreview::detect(image,anchors,center,radius,0,1,clockwise,{});
            auto withText=image.clone();const double textAngle=clockwise?66.5:53.5;
            cv::line(withText,point(textAngle,207),point(textAngle,218),cv::Scalar(0),2,cv::LINE_AA);
            const auto filtered=MinorTickPreview::detect(withText,anchors,center,radius,0,1,clockwise,{});
            if(filtered.marks.size()!=clean.marks.size())return 6;
            for(const auto& mark:filtered.marks)if(!mark.gap&&cv::norm(mark.b-center)<220)return 7;
        }
        const auto original=anchors;auto result=MinorTickPreview::detect(image,anchors,center,radius,0,1,clockwise,{});
        std::cout<<clockwise<<" "<<missing<<" "<<result.message.toStdString()<<std::endl;
        for(const auto& m:result.marks)if(!m.gap&&m.ratio<0)return 20;
        const auto persisted=QJsonDocument::fromJson(QJsonDocument(result.previewPoints).toJson()).array();
        if(persisted!=result.previewPoints)return 22;
        const int minorCount=int(std::count_if(persisted.begin(),persisted.end(),[](const QJsonValue& p){return p.toObject()["source"].toString()=="minor_tick";}));
        if(minorCount!=int(std::count_if(result.marks.begin(),result.marks.end(),[](const MinorTickPreview::Mark& m){return m.assigned;})))return 23;
        const auto reloaded=ScaleCalibration::transformed(ScaleCalibration::transformed(result.previewPoints,(cv::Mat_<double>(3,3)<<1,0,10,0,1,20,0,0,1)),(cv::Mat_<double>(3,3)<<1,0,-10,0,1,-20,0,0,1));
        for(int i=0;i<result.previewPoints.size();++i)if(reloaded[i].toObject()["ratio"]!=result.previewPoints[i].toObject()["ratio"]||reloaded[i].toObject()["source"]!=result.previewPoints[i].toObject()["source"])return 21;
        if(!result.divisionConfirmed||result.previewPoints.size()!=anchors.size()+int(std::count_if(result.marks.begin(),result.marks.end(),[](const MinorTickPreview::Mark& m){return m.assigned;})))return 15;
        for(const auto& m:result.marks)if(m.assigned) {
            const auto p=(m.a+m.b)*.5f-center;const double a=AutoScaleCalibration::wrap(std::atan2(p.y,p.x)*180/CV_PI);
            const int index=cvRound(ScaleCalibration::span(clockwise?20:100,a,clockwise)/3);
            if(std::abs(m.ratio-index/20.)>.00001||(missing&&index==7))return 16;
        }
        if(anchors!=original||result.marks.empty()||std::abs(result.division-.05)>.001)return 1;
        if(!missing&&!shadedOblique) {
            QJsonArray endpoints;endpoints.append(anchors.first());endpoints.append(anchors.last());
            const auto single=MinorTickPreview::detect(image,endpoints,center,radius,0,1,clockwise,{});
            if(single.divisionConfirmed||std::any_of(single.marks.begin(),single.marks.end(),[](const MinorTickPreview::Mark& m){return m.assigned;}))return 17;
            const double needleAngle=clockwise?44:76;
            const auto occluded=MinorTickPreview::detect(image,anchors,center,radius,0,1,clockwise,{},needleAngle);
            for(const auto& m:occluded.marks)if(!m.gap){const auto p=(m.a+m.b)*.5f-center;
                if(AutoScaleCalibration::distance(std::atan2(p.y,p.x)*180/CV_PI,needleAngle)<2.5)return 18;}
        }
        const int gaps=int(std::count_if(result.marks.begin(),result.marks.end(),[](const MinorTickPreview::Mark& m){return m.gap;}));
        if(!shadedOblique&&(gaps!=(missing?1:0)||int(result.marks.size())-gaps!=(missing?17:18)))return 2;
        if(shadedOblique&&(result.marks.size()!=18||int(result.marks.size())-gaps<15))return 5;
        LiquidSurface surface;surface.found=true;surface.centerX=300;surface.radius=270;surface.y0=600;
        auto excluded=MinorTickPreview::detect(image,anchors,center,radius,0,1,clockwise,surface);
        if(!excluded.marks.empty()||excluded.division!=0)return 3;
        auto blank=MinorTickPreview::detect(cv::Mat(600,600,CV_8UC1,cv::Scalar(255)),anchors,center,radius,0,1,clockwise,{});
        if(!blank.marks.empty()||blank.division!=0)return 4;
    }
    // A convincing nine-way count must not publish repeating-decimal values.
    {
        cv::Mat image(600,600,CV_8UC1,cv::Scalar(255));QJsonArray anchors;
        for(int i=0;i<=18;++i){const double angle=20+i*60./18;
            cv::line(image,point(angle,i%9?220:205),point(angle,244),cv::Scalar(0),2,cv::LINE_AA);
            if(i%9==0){const auto p=point(angle,230);anchors.append(QJsonObject{{"x",p.x},{"y",p.y},{"ratio",i/18.}});}}
        const auto uncertain=MinorTickPreview::detect(image,anchors,center,radius,0,.4,true,{});
        if(uncertain.divisionConfirmed)return 40;
        for(const auto& m:uncertain.marks)if(m.assigned||m.ratio>=0)return 41;
    }
    // A known nonlinear local mapping gives a calculable change at the same angle.
    QJsonArray base;
    for(const auto& v:std::vector<cv::Point2d>{{20,0},{50,.5},{80,1}}){auto p=point(v.x,230);base.append(QJsonObject{{"x",p.x},{"y",p.y},{"ratio",v.y}});}
    const auto unchanged=base;
    MinorTickPreview::Result proposal;proposal.division=.2;proposal.divisionConfirmed=true;proposal.previewPoints=base;
    for(const auto& v:std::vector<cv::Point2d>{{32,.2},{43,.4}}){auto p=point(v.x,230);proposal.previewPoints.append(QJsonObject{{"x",p.x},{"y",p.y},{"ratio",v.y}});proposal.marks.push_back({point(v.x,220),point(v.x,240),.9,false,v.y,true});}
    std::vector<QJsonObject> sorted;for(auto p:proposal.previewPoints)sorted.push_back(p.toObject());
    std::sort(sorted.begin(),sorted.end(),[](const QJsonObject& a,const QJsonObject& b){return a["ratio"].toDouble()<b["ratio"].toDouble();});
    proposal.previewPoints={};for(auto p:sorted)proposal.previewPoints.append(p);
    const auto comparison=MinorTickPreview::compare(proposal,base,center,true,0,1,40);
    if(!comparison.valid||std::abs(comparison.baseline-1./3)>.00001||std::abs(comparison.enhanced-(.2+8./11*.2))>.00001||base!=unchanged)return 8;
    if(MinorTickPreview::compare(proposal,base,center,true,0,1,100).valid)return 9;
    proposal.divisionConfirmed=false;
    if(MinorTickPreview::compare(proposal,base,center,true,0,1,40).valid)return 14;
    std::cout<<"PASS: assignment, missing indices, text exclusion, unchanged anchors, comparison and abstention"<<std::endl;
}
