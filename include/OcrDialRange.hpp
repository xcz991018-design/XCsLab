#pragma once
#include "PaddleScaleOcr.hpp"
#include "LiquidSurface.hpp"
#include <array>
#include <limits>
#include "MajorAnchorRectification.hpp"

namespace OcrDialRange {
struct Result {bool valid{false};double minimum{0},maximum{0};cv::Point2f minimumPosition{},maximumPosition{};};
inline Result detect(const QJsonArray& tokens,cv::Point2f center,double size,bool clockwise=true) {
    struct Candidate {double value,cost;cv::Point2f position;};
    std::vector<Candidate> sides[2];
    for(const auto& entry:PaddleScaleOcr::numericTokens(tokens)) {
        const auto t=entry.toObject();cv::Point2f p(0,0);
        for(const auto& v:t["polygon"].toArray())p+=cv::Point2f(v.toArray()[0].toDouble(),v.toArray()[1].toDouble())*.25f;
        const auto d=p-center;const double r=cv::norm(d);
        if(d.y<size*.08||std::abs(d.x)<size*.08||r<size*.16||r>size*.48)continue;
        const double angle=std::atan2(d.y,std::abs(d.x))*180/CV_PI;
        if(std::abs(angle-45)>28)continue;
        sides[d.x>0?1:0].push_back({t["text"].toString().toDouble(),
            std::abs(angle-45)+std::abs(r/size-.28)*20+(1-t["score"].toDouble())*5,p});
    }
    for(auto& side:sides) {
        if(side.empty())return {};
        std::sort(side.begin(),side.end(),[](const Candidate& a,const Candidate& b){return a.cost<b.cost;});
        for(size_t i=1;i<side.size();++i)
            if(side[i].cost-side[0].cost<8&&std::abs(side[i].value-side[0].value)>1e-6)return {};
    }
    const auto& low=sides[clockwise?0:1].front();
    const auto& high=sides[clockwise?1:0].front();
    if(low.value>=high.value)return {};
    return {true,low.value,high.value,low.position,high.position};
}
struct Endpoints {QJsonArray points;QString error;bool valid() const {return points.size()==2&&error.isEmpty();}};
inline QJsonArray reliableNumbers(const QJsonArray& tokens,const LiquidSurface& surface) {
    QJsonArray result;if(surface.ambiguous)return result;
    for(const auto& token:PaddleScaleOcr::numericTokens(tokens)) {
        bool safe=true;const auto polygon=token.toObject()["polygon"].toArray();
        for(int i=0;i<polygon.size();++i) {
            const auto a=polygon[i].toArray(),b=polygon[(i+1)%polygon.size()].toArray();
            for(int k=0;k<=4;++k) {
                const double t=k/4.,x=a[0].toDouble()*(1-t)+b[0].toDouble()*t,y=a[1].toDouble()*(1-t)+b[1].toDouble()*t;
                if(surface.excludes(x,y-2))safe=false;
            }
        }
        if(safe)result.append(token);
    }
    return result;
}
struct MajorTick {AutoScaleCalibration::Tick tick;double innerWidth{0},outerWidth{0};cv::Point2f outer{},inner{};bool complete{false};};
inline bool joinMajorStem(const MajorTick& head,const MajorTick& stem,cv::Point2f center,double radius,MajorTick& joined) {
    if(!head.complete || !stem.complete || stem.tick.radius<=head.tick.radius)return false;
    const cv::Point2f axis=(head.outer-head.inner)/head.tick.length;
    const cv::Point2f stemAxis=(stem.outer-stem.inner)/stem.tick.length;
    const auto gap=stem.inner-head.outer;
    const double width=std::max(head.outerWidth,stem.innerWidth);
    // Compare the measured ink axes; a nearby parallel minor tick has a
    // lateral offset, even when its angle from the hub is a close match.
    if(axis.dot(stemAxis)<.99 || std::abs(axis.cross(gap))>width*.5 ||
       gap.dot(axis)<-width*.5 || cv::norm(gap)>radius*.015)return false;
    const double length=cv::norm(stem.outer-head.inner);
    if(length<radius*.13||length>radius*.24)return false;
    const auto mid=(head.inner+stem.outer)*.5f,radial=mid-center;
    const double angle=AutoScaleCalibration::wrap(std::atan2(radial.y,radial.x)*180/CV_PI);
    joined={{mid,angle,cv::norm(radial),length},head.innerWidth,stem.outerWidth,stem.outer,head.inner,true};
    return true;
}
inline double exteriorStrokeBoundary(const AutoScaleCalibration::Tick& tick,const QJsonArray& numbers,cv::Point2f center,double radius) {
    const auto direction=(tick.point-center)*float(1/tick.radius);
    const auto inner=tick.point-direction*float(tick.length*.5),outer=tick.point+direction*float(tick.length*.5);
    for(const auto& token:numbers) {
        std::vector<cv::Point2f> polygon;cv::Point2f label(0,0);
        for(const auto& value:token.toObject()["polygon"].toArray()) {
            const auto p=value.toArray();polygon.emplace_back(p[0].toDouble(),p[1].toDouble());label+=polygon.back()*.25f;
        }
        if(polygon.size()!=4)continue;
        const auto radial=label-center;const double labelRadius=cv::norm(radial);
        const double labelAngle=AutoScaleCalibration::wrap(std::atan2(radial.y,radial.x)*180/CV_PI);
        if(tick.radius<labelRadius+radius*.035||AutoScaleCalibration::distance(tick.angle,labelAngle)>14)continue;
        // Recover a stem only if it crosses the outer side of the OCR box.
        // A glyph contained by its text box cannot satisfy this geometry.
        if(cv::pointPolygonTest(polygon,inner,true)>=-2&&cv::pointPolygonTest(polygon,outer,true)<-radius*.02)
            return labelRadius+radius*.035;
    }
    return -1;
}
inline std::vector<MajorTick> majorTicks(const cv::Mat& image,const QJsonArray& numbers,cv::Point2f center,double radius,const LiquidSurface& surface,QJsonArray* probes=nullptr,bool obliqueMajors=false,std::vector<AutoScaleCalibration::Tick>* shortStrokes=nullptr,std::vector<MajorTick>* recoveredMajors=nullptr,bool faintComponentsOnly=false) {
    std::vector<MajorTick> result;if(image.empty()||radius<40||surface.ambiguous)return result;
    struct Component {
        MajorTick stroke;
        bool standalone;
        int partners{0};
    };
    std::vector<Component> components;
    cv::Mat gray,ink;
    if(image.channels()==1)gray=image;else cv::cvtColor(image,gray,cv::COLOR_BGR2GRAY);
    cv::adaptiveThreshold(gray,ink,255,cv::ADAPTIVE_THRESH_GAUSSIAN_C,cv::THRESH_BINARY_INV,31,faintComponentsOnly?4:9);
    for(int y=0;y<ink.rows;++y)for(int x=0;x<ink.cols;++x) {
        const double r=std::hypot(x-center.x,y-center.y);
        if(r<radius*.60||r>radius*.96||surface.excludes(x,y-2))ink.at<uchar>(y,x)=0;
    }
    const auto exteriorInk=faintComponentsOnly?cv::Mat():ink.clone();
    // OCR regions are text, not stroke evidence. Keep a small separation from
    // their edges so glyphs cannot join a nearby graduation component.
    cv::Mat text=cv::Mat::zeros(ink.size(),CV_8U);
    for(const auto& token:numbers) {
        std::vector<cv::Point> p;for(const auto& v:token.toObject()["polygon"].toArray())p.emplace_back(cvRound(v.toArray()[0].toDouble()),cvRound(v.toArray()[1].toDouble()));
        if(p.size()==4)cv::fillConvexPoly(text,p,255);
    }
    cv::dilate(text,text,cv::getStructuringElement(cv::MORPH_ELLIPSE,{5,5}));ink.setTo(0,text);
    cv::Mat pixelLabels,stats,centroids;const int count=cv::connectedComponentsWithStats(ink,pixelLabels,stats,centroids,8);
    for(int label=1;label<count;++label) {
        if(stats.at<int>(label,cv::CC_STAT_AREA)<radius*.05)continue;
        const cv::Rect bounds(stats.at<int>(label,cv::CC_STAT_LEFT),stats.at<int>(label,cv::CC_STAT_TOP),stats.at<int>(label,cv::CC_STAT_WIDTH),stats.at<int>(label,cv::CC_STAT_HEIGHT));
        std::vector<cv::Point2f> pixels;
        for(int y=bounds.y;y<bounds.br().y;++y)for(int x=bounds.x;x<bounds.br().x;++x)if(pixelLabels.at<int>(y,x)==label)pixels.emplace_back(float(x),float(y));
        cv::Vec4f line;cv::fitLine(pixels,line,cv::DIST_HUBER,0,.01,.01);
        cv::Point2f direction(line[0],line[1]),anchor(line[2],line[3]);
        if(direction.dot(anchor-center)<0)direction=-direction;
        std::vector<double> projections;for(auto p:pixels)projections.push_back((p-anchor).dot(direction));
        std::sort(projections.begin(),projections.end());
        const double lo=projections[projections.size()/50],hi=projections[projections.size()-1-projections.size()/50],length=hi-lo;
        const auto middle=anchor+direction*float((lo+hi)*.5),radial=middle-center;const double r=cv::norm(radial);
        const double axisOffset=std::abs(direction.cross(radial));
        if(length<radius*(shortStrokes?.035:.07)||length>radius*.24||r<radius*.64||r+length*.5<radius*.72||r>radius*.95||
           axisOffset>radius*.13)continue;
        // Compare transverse ink width along the inner and outer thirds. The
        // inner thick head is supporting evidence; straight long ticks also work.
        double innerArea=0,outerArea=0;
        for(auto p:pixels){const double t=(p-anchor).dot(direction);if(t>lo+length*.08&&t<lo+length*.38)++innerArea;if(t>lo+length*.62&&t<lo+length*.92)++outerArea;}
        const double innerWidth=innerArea/(length*.30),outerWidth=outerArea/(length*.30);
        if(innerWidth<1||outerWidth<.7||std::max(innerWidth,outerWidth)>radius*.045||length/std::max(innerWidth,outerWidth)<3)continue;
        if(surface.found) {
            bool safe=true;
            for(auto p:pixels)if(surface.excludes(p.x,p.y-2)){safe=false;break;}
            // A component truncated at the liquid mask is not a whole tick.
            for(const auto p:{anchor+direction*float(lo-3),anchor+direction*float(hi+3)})if(surface.excludes(p.x,p.y-2))safe=false;
            if(!safe)continue;
        }
        const double a=AutoScaleCalibration::wrap(std::atan2(radial.y,radial.x)*180/CV_PI);
        const bool standalone=axisOffset<=radius*.09 || (obliqueMajors&&length>=radius*.13);
        if(length<radius*.07) {
            if(standalone)shortStrokes->push_back({middle,a,r,length});
            continue;
        }
        const auto outer=anchor+direction*float(hi),inner=anchor+direction*float(lo);
        const bool complete=cv::norm(outer-center)<radius*.96-3 && cv::norm(inner-center)>radius*.60+3;
        MajorTick stroke{{middle,a,r,length},innerWidth,outerWidth,outer,inner,complete};
        if(standalone)result.push_back(stroke);
        if(recoveredMajors&&complete)components.push_back({stroke,standalone});
    }
    if(faintComponentsOnly)return result;
    // Resolve pairs before applying the single-component radial test. A
    // re-warp can put either piece just outside that tolerance.
    struct Joined {size_t head,stem; MajorTick stroke;};
    std::vector<Joined> joined;
    for(size_t i=0;i<components.size();++i)for(size_t j=i+1;j<components.size();++j) {
        if(components[i].standalone && components[j].standalone)continue;
        const bool iFirst=components[i].stroke.tick.radius<components[j].stroke.tick.radius;
        const size_t head=iFirst?i:j,stem=iFirst?j:i;
        MajorTick stroke;
        if(joinMajorStem(components[head].stroke,components[stem].stroke,center,radius,stroke)) {
            ++components[head].partners;
            ++components[stem].partners;
            joined.push_back({head,stem,stroke});
        }
    }
    for(const auto& pair:joined)if(components[pair.head].partners==1&&components[pair.stem].partners==1) {
        recoveredMajors->push_back(pair.stroke);
    }
    // A complete tilted line may also qualify when its thin and thick parts
    // are connected. Only the label-aware matching stage can promote it.
    for(const auto& component:components)
        if(!component.standalone && component.partners==0 && component.stroke.tick.length>=radius*.13)
            recoveredMajors->push_back(component.stroke);
    // A tick can touch the needle or a zero-stop symbol. Recover its straight
    // stem with LSD, then verify real ink and width along that stem instead of
    // discarding the entire joined connected component.
    for(const auto& t:AutoScaleCalibration::ticks(image,center,radius,.09)) {
        if(t.length<radius*.07||t.radius<radius*.64||t.radius+t.length*.5<radius*.72||t.radius>radius*.95)continue;
        const auto direction=(t.point-center)*float(1/t.radius);const cv::Point2f normal(-direction.y,direction.x);
        const double exteriorBoundary=exteriorStrokeBoundary(t,numbers,center,radius);
        double innerWidth=0,outerWidth=0;int present=0;bool safe=true;
        for(int step=0;step<20;++step) {
            const double f=(step+.5)/20.;const auto p=t.point+direction*float((f-.5)*t.length);
            int width=0;
            for(int offset=-cvRound(radius*.025);offset<=cvRound(radius*.025);++offset) {
                const auto q=p+normal*float(offset);const int x=cvRound(q.x),y=cvRound(q.y);
                if(surface.excludes(q.x,q.y-2))safe=false;
                if(x>=0&&y>=0&&x<ink.cols&&y<ink.rows) {
                    const bool recover=exteriorBoundary>=0&&cv::norm(q-center)>exteriorBoundary;
                    if((recover?exteriorInk:ink).at<uchar>(y,x))++width;
                }
            }
            if(width)++present;
            if(step>=2&&step<8)innerWidth+=width/6.;if(step>=12&&step<18)outerWidth+=width/6.;
        }
        if(probes)probes->append(QJsonObject{{"angle",t.angle},{"length",t.length},{"present",present},{"safe",safe},{"inner",innerWidth},{"outer",outerWidth},{"ocr_box_recovery",exteriorBoundary>=0}});
        if(!safe||present<16||innerWidth<1||outerWidth<.7||std::max(innerWidth,outerWidth)>radius*.045)continue;
        auto existing=std::find_if(result.begin(),result.end(),[&](const MajorTick& m){return AutoScaleCalibration::distance(m.tick.angle,t.angle)<1.7&&std::abs(m.tick.radius-t.radius)<radius*.08;});
        if(existing==result.end())result.push_back({t,innerWidth,outerWidth});
        else if(t.length>existing->tick.length*1.15)*existing={t,innerWidth,outerWidth};
    }
    // The thick head and thin stem of the zero tick can be separate components.
    // Merge overlapping radial pieces before ring validation and length ranking;
    // otherwise its head falls inside the ring and a nearby minor tick wins.
    for(size_t i=0;i<result.size();++i)for(size_t j=i+1;j<result.size();) {
        auto& a=result[i];const auto b=result[j];
        const double ai=a.tick.radius-a.tick.length*.5,ao=a.tick.radius+a.tick.length*.5;
        const double bi=b.tick.radius-b.tick.length*.5,bo=b.tick.radius+b.tick.length*.5;
        const double inner=std::min(ai,bi),outer=std::max(ao,bo);
        if(AutoScaleCalibration::distance(a.tick.angle,b.tick.angle)<1.7&&
           std::max(ai,bi)<=std::min(ao,bo)+radius*.015&&outer-inner<=radius*.24) {
            const double angle=AutoScaleCalibration::wrap(a.tick.angle+std::remainder(b.tick.angle-a.tick.angle,360.)*b.tick.length/(a.tick.length+b.tick.length));
            const double r=(inner+outer)*.5,rad=angle*CV_PI/180;
            const double innerWidth=ai<=bi?a.innerWidth:b.innerWidth,outerWidth=ao>=bo?a.outerWidth:b.outerWidth;
            a={{center+cv::Point2f(float(r*std::cos(rad)),float(r*std::sin(rad))),angle,r,outer-inner},innerWidth,outerWidth};
            result.erase(result.begin()+j);
        } else ++j;
    }
    return result;
}
inline double printedRadius(const cv::RotatedRect& ellipse,cv::Point2f p) {
    const double a=ellipse.angle*CV_PI/180;const auto d=p-ellipse.center;
    return std::hypot((d.x*std::cos(a)+d.y*std::sin(a))/(ellipse.size.width*.5),(-d.x*std::sin(a)+d.y*std::cos(a))/(ellipse.size.height*.5));
}
inline Endpoints matchEndpoints(const Result& range,const std::vector<AutoScaleCalibration::Tick>& ticks,
                               cv::Point2f center,double radius,bool clockwise,const std::vector<double>& shapeScores={},const cv::RotatedRect& printed={}) {
    using namespace AutoScaleCalibration;
    if(!range.valid||radius<=0)return {{},QStringLiteral(u"缺少可靠端点数字或表盘半径。")};
    struct Candidate {int index;double cost;};std::vector<Candidate> sides[2];
    for(int side=0;side<2;++side) {
        const auto label=(side==0?range.minimumPosition:range.maximumPosition)-center;
        const double labelRadius=cv::norm(label),angle=wrap(std::atan2(label.y,label.x)*180/CV_PI);
        for(int i=0;i<int(ticks.size());++i) {
            const auto& t=ticks[i];
            const double error=distance(angle,t.angle);
            if(error>14||t.radius<radius*.64||t.radius+t.length*.5<radius*.72||t.radius>radius*.95||t.radius<labelRadius+radius*.06||t.length<radius*.07)continue;
            double longest=t.length;
            for(const auto& other:ticks)if(distance(other.angle,t.angle)<16&&std::abs(other.radius-t.radius)<radius*.10)longest=std::max(longest,other.length);
            if(t.length<longest*.70)continue;
            sides[side].push_back({i,error-t.length/radius*15-(i<int(shapeScores.size())?shapeScores[i]:0)});
        }
    }
    struct Pair {int first,last;double cost;};std::vector<Pair> pairs;
    double closestSweepError=360;
    for(const auto& a:sides[0])for(const auto& b:sides[1]) {
        const auto& lo=ticks[a.index];const auto& hi=ticks[b.index];
        const double sweep=ScaleCalibration::span(lo.angle,hi.angle,clockwise);
        if(a.index==b.index)continue;
        if(printed.size.width>0) {
            const auto onRing=[&](const Tick& t){const double a=t.angle*CV_PI/180;const double r=t.radius+t.length*.5;
                return std::abs(printedRadius(printed,center+cv::Point2f(float(r*std::cos(a)),float(r*std::sin(a))))-1)<.055;};
            if(!onRing(lo)||!onRing(hi))continue;
        } else if(std::abs(lo.radius-hi.radius)>radius*.08)continue;
        closestSweepError=std::min(closestSweepError,std::abs(sweep-270));
        // A wide tolerance admits the zero-stop symbol or adjacent tick as
        // the minimum endpoint. Keep the measured span within the original
        // narrow tolerance; geometry errors must not be hidden by acceptance.
        constexpr double endpointSweepTolerance=6;
        if(std::abs(sweep-270)>endpointSweepTolerance)continue;
        pairs.push_back({a.index,b.index,a.cost+b.cost+std::abs(sweep-270)*.7+std::abs(lo.radius-hi.radius)/radius*20});
    }
    if(pairs.empty())return {{},closestSweepError<360?
        QStringLiteral(u"起止刻度跨度偏离270°约%1°，超出允许的±6°范围；未采用本次起止刻度，原标定保留。").arg(closestSweepError,0,'f',1):
        QStringLiteral(u"数字外侧未找到可靠起止大刻度线，请检查四切点、轴心及图像清晰度；原刻度标定保留。")};
    std::sort(pairs.begin(),pairs.end(),[](const Pair& a,const Pair& b){return a.cost<b.cost;});
    const auto best=pairs.front();
    for(const auto& alternative:pairs)if(alternative.cost<best.cost+2&&
        (distance(ticks[alternative.first].angle,ticks[best.first].angle)>2||distance(ticks[alternative.last].angle,ticks[best.last].angle)>2||
         std::abs(ticks[alternative.first].radius-ticks[best.first].radius)>radius*.05))
        return {{},QStringLiteral(u"端点附近存在多组相近的大刻度线，请手动确认，原标定保留。")};
    Endpoints result;
    for(const auto& endpoint:std::vector<std::pair<int,int>>{{best.first,0},{best.last,1}}) {
        const auto p=ticks[endpoint.first].point;
        result.points.append(QJsonObject{{"x",p.x},{"y",p.y},{"ratio",endpoint.second}});
    }
    return result;
}
inline Endpoints findEndpoints(const cv::Mat& image,const Result& range,cv::Point2f center,double radius,bool clockwise) {
    // Local perspective/refraction can tilt the stroke slightly relative to the
    // hub. The label window and paired major-tick checks constrain this search.
    return matchEndpoints(range,AutoScaleCalibration::ticks(image,center,radius,.09),center,radius,clockwise);
}
struct Calibration {
    Result range;
    QJsonArray points;
    QString error;
    int skipped{0};
    std::vector<MajorAnchorRectification::Observation> geometry;
    // Derived values are reading anchors, never independent evidence for rectification.
    QJsonArray unlabeledPoints;
    void transformPoints(const cv::Mat& H) {
        points=ScaleCalibration::transformed(points,H);
        unlabeledPoints=ScaleCalibration::transformed(unlabeledPoints,H);
    }
    bool valid() const {return error.isEmpty()&&points.size()>=2;}
};
inline cv::RotatedRect printedRing(const std::vector<MajorTick>& ticks,cv::Point2f center,double radius) {
    std::vector<cv::Point2f> tips;std::array<int,4> quadrants{};
    for(const auto& m:ticks) {
        const auto& t=m.tick;const double r=t.radius+t.length*.5,a=t.angle*CV_PI/180;
        if(r<radius*.68||r>radius*.99)continue;
        tips.emplace_back(center.x+float(r*std::cos(a)),center.y+float(r*std::sin(a)));
        ++quadrants[int(AutoScaleCalibration::wrap(t.angle)/90)%4];
    }
    if(tips.size()<18||std::count_if(quadrants.begin(),quadrants.end(),[](int n){return n>=4;})<3)return {};
    auto ellipse=cv::fitEllipse(tips);
    const auto valid=[&](const cv::RotatedRect& e){return std::isfinite(e.angle)&&std::isfinite(e.center.x)&&std::isfinite(e.center.y)&&
        e.size.width>radius*1.2&&e.size.height>radius*1.2&&e.size.width<radius*2.1&&e.size.height<radius*2.1&&cv::norm(e.center-center)<radius*.2;};
    if(!valid(ellipse))return {};
    const auto residual=[](const cv::RotatedRect& e,cv::Point2f p){
        const double a=e.angle*CV_PI/180;const auto d=p-e.center;
        return std::abs(std::hypot((d.x*std::cos(a)+d.y*std::sin(a))/(e.size.width*.5),(-d.x*std::sin(a)+d.y*std::cos(a))/(e.size.height*.5))-1);
    };
    std::vector<cv::Point2f> inliers;for(auto p:tips)if(residual(ellipse,p)<.04)inliers.push_back(p);
    if(inliers.size()<18||inliers.size()<tips.size()*.8)return {};
    ellipse=cv::fitEllipse(inliers);return valid(ellipse)?ellipse:cv::RotatedRect{};
}
inline QJsonArray unlabeledMajorAnchors(const cv::Mat& image,const QJsonArray& tokens,const Calibration& calibration,const ScaleCalibration::Mapping& mapping,
        const std::vector<MajorTick>& strokes,const std::vector<AutoScaleCalibration::Tick>& shortStrokes,const std::vector<AutoScaleCalibration::Number>& labels,
        cv::Point2f center,double radius,bool clockwise,const cv::RotatedRect& printed,const LiquidSurface& surface) {
    QJsonArray points;
    const double range=calibration.range.maximum-calibration.range.minimum;
    // Use the stroke lengths and outer tips measured at the two bounding labels.
    std::vector<const MajorTick*> references;
    for(const auto& entry:calibration.points) {
        const auto p=entry.toObject();
        const cv::Point2f position(p["x"].toDouble(),p["y"].toDouble());
        const auto found=std::find_if(strokes.begin(),strokes.end(),[&](const MajorTick& m){return cv::norm(m.tick.point-position)<.5;});
        if(found==strokes.end())return {};
        references.push_back(&*found);
    }
    struct Candidate {const AutoScaleCalibration::Tick* tick; double offset,relativeLength; bool major;};
    struct MaskedInterval {
        size_t index;
        double arc,firstHidden,lastHidden,minimumLength;
        double firstUnmatchedRatio,lastUnmatchedRatio;
        std::vector<Candidate> ticks;
    };
    std::vector<MaskedInterval> maskedIntervals;
    std::vector<double> countedSteps(references.size(),0);
    const bool fittedRing=printed.size.width>0;
    const auto ringAt=[&](const AutoScaleCalibration::Tick& tick,cv::Point2f outer) {
        return fittedRing?printedRadius(printed,outer):tick.radius+tick.length*.5;
    };
    const auto radialTip=[&](const AutoScaleCalibration::Tick& tick) {
        return center+(tick.point-center)*float(1+tick.length/(2*tick.radius));
    };
    const auto majorRing=[&](const MajorTick& mark) {
        return ringAt(mark.tick,mark.complete?mark.outer:radialTip(mark.tick));
    };
    const auto anchor=[&](const Candidate& c,double ratio) {
        return QJsonObject{{"x",c.tick->point.x},{"y",c.tick->point.y},{"ratio",ratio},
            {"origin_x",center.x},{"origin_y",center.y},{"source",QStringLiteral("unlabeled_major_tick")}};
    };
    for(size_t i=1;i<references.size();++i) {
        const auto& left=references[i-1]->tick;
        const auto& right=references[i]->tick;
        const double arc=mapping.offsets[i]-mapping.offsets[i-1];
        const double low=calibration.range.minimum+range*mapping.ratios[i-1];
        const double high=calibration.range.minimum+range*mapping.ratios[i];
        double firstHidden=arc,lastHidden=-1;
        double firstUnmatchedRatio=mapping.ratios[i],lastUnmatchedRatio=mapping.ratios[i-1];
        for(const auto& label:labels) {
            if(label.value<=low+1e-6||label.value>=high-1e-6)continue;
            const auto radial=label.center-center;
            const double angle=std::atan2(radial.y,radial.x)*180/CV_PI;
            const double offset=ScaleCalibration::span(left.angle,angle,clockwise);
            const double ratio=(label.value-calibration.range.minimum)/range;
            firstUnmatchedRatio=std::min(firstUnmatchedRatio,ratio);
            lastUnmatchedRatio=std::max(lastUnmatchedRatio,ratio);
            // Stop at the unmatched label's observed ray as well as its value.
            // A label outside this arc cannot localize the break.
            firstHidden=std::min(firstHidden,offset<=arc?offset:0.);
            lastHidden=std::max(lastHidden,offset<=arc?offset:arc);
        }
        if(surface.found) {
            const int samples=int(std::ceil(arc));
            for(int j=0;j<=samples;++j) {
                const double f=double(j)/samples;
                const double angle=(left.angle+(clockwise?1:-1)*arc*f)*CV_PI/180;
                const double r=left.radius+(right.radius-left.radius)*f;
                const double length=left.length+(right.length-left.length)*f;
                for(double tip:{r-length*.5,r+length*.5}) {
                    if(surface.excludes(center.x+tip*std::cos(angle),center.y+tip*std::sin(angle)-2)) {
                        firstHidden=std::min(firstHidden,arc*f);
                        lastHidden=std::max(lastHidden,arc*f);
                    }
                }
            }
        }
        std::vector<Candidate> candidates;
        const double leftRing=majorRing(*references[i-1]),rightRing=majorRing(*references[i]);
        const auto collect=[&](const AutoScaleCalibration::Tick& t,double ring,bool major) {
            const double offset=ScaleCalibration::span(left.angle,t.angle,clockwise);
            if(offset<1.7||offset>arc-1.7)return;
            const double f=offset/arc;
            const double length=left.length+(right.length-left.length)*f;
            if(std::abs(ring-(leftRing+(rightRing-leftRing)*f))>(fittedRing?.02:radius*.045))return;
            candidates.push_back({&t,offset,t.length/length,major});
        };
        for(const auto& m:strokes)collect(m.tick,majorRing(m),m.complete);
        for(const auto& t:shortStrokes)collect(t,ringAt(t,radialTip(t)),false);
        if(candidates.empty())continue;
        // Use the shorter strokes on the same ring as a length reference. On a
        // long-only dial the matched numeric ticks supply that reference instead.
        std::vector<double> lengths;
        for(const auto& c:candidates)lengths.push_back(c.relativeLength);
        std::sort(lengths.begin(),lengths.end());
        const double shorter=lengths[lengths.size()/4];
        const double minimumLength=shorter<.8?std::max(.65,shorter*1.30):.85;
        std::vector<Candidate> longTicks;
        for(const auto& c:candidates)
            if(c.major&&c.relativeLength>=minimumLength&&c.relativeLength<=1.30)longTicks.push_back(c);
        std::sort(longTicks.begin(),longTicks.end(),[](const Candidate& a,const Candidate& b){return a.offset<b.offset;});
        if(lastHidden>=0) {
            maskedIntervals.push_back({i,arc,firstHidden,lastHidden,minimumLength,firstUnmatchedRatio,lastUnmatchedRatio,std::move(longTicks)});
            continue;
        }
        if(longTicks.empty())continue;

        const double parts=double(longTicks.size()+1);
        const double pitch=arc/parts;
        QJsonArray interval;
        double previous=0;
        for(size_t k=0;k<longTicks.size();++k) {
            const auto& c=longTicks[k];
            // A skipped or competing line must not renumber the rest of the arc.
            if(std::abs(c.offset-(k+1)*pitch)>std::min(5.,pitch*.28)||
               std::abs((c.offset-previous)-pitch)>pitch*.30)break;
            previous=c.offset;
            const double ratio=mapping.ratios[i-1]+(mapping.ratios[i]-mapping.ratios[i-1])*(k+1)/parts;
            interval.append(anchor(c,ratio));
        }
        if(size_t(interval.size())==longTicks.size()) {
            countedSteps[i]=(mapping.ratios[i]-mapping.ratios[i-1])/parts;
            for(const auto& p:interval)points.append(p);
        }
    }
    // The hidden middle supplies no count. Work inward from each numbered
    // edge only while actual visible long strokes support the adjacent measured step.
    std::vector<MajorTick> faintStrokes;
    bool searchedFaintStrokes=false;
    for(auto& section:maskedIntervals) {
        const size_t i=section.index;
        const double leftStep=countedSteps[i-1],rightStep=i+1<countedSteps.size()?countedSteps[i+1]:0;
        if(leftStep>0&&rightStep>0&&std::abs(leftStep-rightStep)>1e-6)continue;
        if(leftStep<=0&&rightStep<=0)continue;
        if(surface.found&&fittedRing) {
            if(!searchedFaintStrokes) {
                faintStrokes=majorTicks(image,tokens,center,radius,surface,nullptr,false,nullptr,nullptr,true);
                searchedFaintStrokes=true;
            }
            const auto& left=references[i-1]->tick;
            const auto& right=references[i]->tick;
            const double leftRing=majorRing(*references[i-1]),rightRing=majorRing(*references[i]);
            const auto strongCount=section.ticks.size();
            for(const auto& mark:faintStrokes) {
                if(!mark.complete)continue;
                const auto& tick=mark.tick;
                const double offset=ScaleCalibration::span(left.angle,tick.angle,clockwise);
                if(offset<1.7||offset>section.arc-1.7||
                   (offset>=section.firstHidden&&offset<=section.lastHidden))continue;
                const double f=offset/section.arc;
                const double relativeLength=tick.length/(left.length+(right.length-left.length)*f);
                // Two contrast passes must agree on the ink before allowing
                // another half percent of local ring deviation after re-warping.
                if(relativeLength<section.minimumLength||relativeLength>1.30||
                   std::abs(majorRing(mark)-(leftRing+(rightRing-leftRing)*f))>.025)continue;
                if(std::any_of(section.ticks.begin(),section.ticks.begin()+strongCount,[&](const Candidate& c){
                    return std::abs(c.offset-offset)<1.7;
                }))continue;
                // The lower-contrast fit must extend or refine ink also found in
                // the original pass. A faint edge by itself cannot add an anchor.
                const auto axis=(mark.outer-mark.inner)/tick.length;
                if(std::none_of(strokes.begin(),strokes.end(),[&](const MajorTick& strong){
                    if(!strong.complete||AutoScaleCalibration::distance(strong.tick.angle,tick.angle)>=1.7)return false;
                    const auto d=strong.tick.point-tick.point;
                    return std::abs(axis.cross(d))<=radius*.012&&std::abs(axis.dot(d))<=tick.length*.5;
                }))continue;
                section.ticks.push_back({&tick,offset,relativeLength,true});
            }
            std::sort(section.ticks.begin(),section.ticks.end(),[](const Candidate& a,const Candidate& b){return a.offset<b.offset;});
        }
        const double span=mapping.ratios[i]-mapping.ratios[i-1];
        for(bool fromLeft:{true,false}) {
            // A break at a known value lets either edge use the verified step
            // from the opposite neighbor. Each intervening stroke must still match.
            const bool hasUnmatched=section.firstUnmatchedRatio<=section.lastUnmatchedRatio;
            const double step=hasUnmatched?std::max(leftStep,rightStep):(fromLeft?leftStep:rightStep);
            if(step<=0)continue;
            const double parts=std::round(span/step);
            if(parts<2||std::abs(span/step-parts)>1e-5)continue;
            const double pitch=section.arc/parts;
            const double tolerance=std::min(5.,pitch*.28);
            const double visible=fromLeft?section.firstHidden:section.arc-section.lastHidden;
            double previous=0;
            for(size_t k=0;k<section.ticks.size();++k) {
                const auto& c=section.ticks[fromLeft?k:section.ticks.size()-1-k];
                const double offset=fromLeft?c.offset:section.arc-c.offset;
                const double expected=(k+1)*pitch;
                if(offset>=visible||k+1>=parts||std::abs(offset-expected)>tolerance||
                   std::abs(offset-previous-pitch)>pitch*.30)break;
                if(k+1<section.ticks.size()) {
                    const double next=fromLeft?section.ticks[k+1].offset:section.arc-section.ticks[section.ticks.size()-2-k].offset;
                    if(std::abs(next-expected)<=tolerance)break;
                }
                const double ratio=fromLeft?mapping.ratios[i-1]+(k+1)*step:mapping.ratios[i]-(k+1)*step;
                if(fromLeft?ratio>=section.firstUnmatchedRatio-1e-6:ratio<=section.lastUnmatchedRatio+1e-6)break;
                previous=offset;
                int index=0;
                while(index<points.size()&&points.at(index).toObject()["ratio"].toDouble()<ratio-1e-6)++index;
                if(index==points.size()||std::abs(points.at(index).toObject()["ratio"].toDouble()-ratio)>1e-6)
                    points.insert(index,anchor(c,ratio));
            }
        }
    }
    return points;
}
inline Calibration calibrate(const cv::Mat& image,const QJsonArray& tokens,cv::Point2f center,double radius,bool clockwise,const LiquidSurface& surface={},QJsonArray* diagnostics=nullptr,const QJsonArray& expectedAnchors={},bool obliqueMajors=false) {
    using namespace AutoScaleCalibration;
    Calibration result;
    if(surface.ambiguous){result.error=QStringLiteral(u"液面位置有歧义，请先确认液面；原刻度标定保留。");return result;}
    const auto refined=PaddleScaleOcr::tightenTickSuffixBoxes(image,tokens);
    const auto reliable=reliableNumbers(refined,surface);
    result.range=detect(reliable,center,image.cols,clockwise);
    if(!result.range.valid){result.error=QStringLiteral(u"液面下方缺少可靠的左下、右下端点数字，原标定保留。");return result;}
    std::vector<Tick> shortStrokes;
    std::vector<MajorTick> recoveredMajors;
    const auto numericTokens=PaddleScaleOcr::numericTokens(refined);
    auto majors=majorTicks(image,numericTokens,center,radius,surface,nullptr,obliqueMajors,&shortStrokes,&recoveredMajors);
    const auto printed=printedRing(majors,center,radius);
    std::vector<Tick> ticks;std::vector<double> shapeScores;
    for(const auto& m:majors){ticks.push_back(m.tick);shapeScores.push_back(std::clamp(m.innerWidth/std::max(1.,m.outerWidth)-1.,0.,2.)*.8);}
    const auto endpoints=matchEndpoints(result.range,ticks,center,radius,clockwise,shapeScores,printed);
    if(!endpoints.valid()){result.error=endpoints.error;return result;}
    const int ordinaryCount=int(majors.size());
    majors.insert(majors.end(),recoveredMajors.begin(),recoveredMajors.end());
    const auto first=endpoints.points[0].toObject(),last=endpoints.points[1].toObject();
    const cv::Point2f firstPoint(first["x"].toDouble(),first["y"].toDouble()),lastPoint(last["x"].toDouble(),last["y"].toDouble());
    const double firstAngle=wrap(std::atan2(firstPoint.y-center.y,firstPoint.x-center.x)*180/CV_PI);
    const double lastAngle=wrap(std::atan2(lastPoint.y-center.y,lastPoint.x-center.x)*180/CV_PI);
    const double sweep=ScaleCalibration::span(firstAngle,lastAngle,clockwise),ring=(cv::norm(firstPoint-center)+cv::norm(lastPoint-center))*.5;
    struct Choice {double ratio,cost;int tick;};std::vector<Choice> choices;
    const auto labels=numbers(reliable,result.range.minimum,result.range.maximum);
    for(const auto& number:labels) {
        const double ratio=(number.value-result.range.minimum)/(result.range.maximum-result.range.minimum);
        if(ratio<=0||ratio>=1)continue;
        const auto d=number.center-center;const double labelAngle=wrap(std::atan2(d.y,d.x)*180/CV_PI),labelRadius=cv::norm(d);
        // Re-detection on a warped copy has a prediction from the same frozen frame.
        double trackedAngle=std::numeric_limits<double>::quiet_NaN();
        for(const auto& entry:expectedAnchors) {
            const auto prior=entry.toObject();
            if(std::abs(prior["ratio"].toDouble()-ratio)>1e-6)continue;
            trackedAngle=std::atan2(prior["y"].toDouble()-center.y,prior["x"].toDouble()-center.x)*180/CV_PI;
            break;
        }
        std::vector<Choice> candidates;
        std::vector<Choice> recoveredChoices;
        for(int i=0;i<int(majors.size());++i) {
            const auto& m=majors[i];const auto& t=m.tick;
            const double expected=wrap(firstAngle+(clockwise?1:-1)*sweep*ratio),angularError=distance(t.angle,expected);
            // LSD can follow either edge of a thick head. Compare the angular
            // interval occupied by that stroke, allowing at most one degree of
            // measured half-width beyond the nominal 5-degree model residual.
            const double uncertainty=std::min(1.,std::atan2(std::max(m.innerWidth,m.outerWidth)*.5,t.radius)*180/CV_PI);
            if(std::isfinite(trackedAngle)&&distance(trackedAngle,t.angle)>1.5)continue;
            auto onRing=[&](const Tick& t) {
                bool sameRing=std::abs(t.radius-ring)<=radius*.08;
                if(printed.size.width>0) {
                    const double a=t.angle*CV_PI/180;const auto tip=center+cv::Point2f(float((t.radius+t.length*.5)*std::cos(a)),float((t.radius+t.length*.5)*std::sin(a)));
                    const auto d=tip-printed.center;const double r=printed.angle*CV_PI/180;
                    sameRing=std::abs(std::hypot((d.x*std::cos(r)+d.y*std::sin(r))/(printed.size.width*.5),(-d.x*std::sin(r)+d.y*std::cos(r))/(printed.size.height*.5))-1)<.055;
                }
                return sameRing;
            };
            const bool sameRing=onRing(t);
            const double labelError=distance(t.angle,labelAngle);
            if(diagnostics)diagnostics->append(QJsonObject{{"stage","geometry"},{"value",number.value},{"tick",i},{"angle",t.angle},{"angular_error",angularError},{"label_error",labelError},{"same_ring",sameRing},{"radius",t.radius},{"label_radius",labelRadius},{"length",t.length}});
            if(labelError>14||!sameRing||t.radius<labelRadius+radius*.035)continue;
            if(ScaleCalibration::span(firstAngle,t.angle,clockwise)>=sweep)continue;
            double longest=t.length,nextLength=0;
            // Off-ring edges cannot compete with printed graduations.
            for(const auto& other:ticks)if(onRing(other)&&distance(other.angle,t.angle)<16&&std::abs(other.radius-t.radius)<radius*.1) {
                longest=std::max(longest,other.length);
                if(distance(other.angle,t.angle)>2.5)nextLength=std::max(nextLength,other.length);
            }
            if(t.length<longest*.70)continue;
            const double taper=std::clamp(m.innerWidth/std::max(1.,m.outerWidth)-1.,0.,2.);
            // An unambiguous major immediately outside its label,
            // on a distributed observed ring, is stronger evidence than uniform
            // angular spacing in a view with residual perspective. Keep the
            // default tight prior for short or competing strokes. A clear
            // length advantage also supports straight or weakly tapered heads.
            const bool directMajor=printed.size.width>0&&labelError<=4&&t.length>=radius*.11&&
                t.length>=nextLength*1.35&&(taper>=.25||t.length>=nextLength*1.7);
            if(diagnostics)diagnostics->append(QJsonObject{{"stage","angle"},{"value",number.value},{"tick",i},{"angular_error",angularError},{"limit",(directMajor?8.:5.)+uncertainty},{"direct_major",directMajor},{"next_length",nextLength},{"taper",taper}});
            if(angularError>(directMajor?8.:5.)+uncertainty)continue;
            // A nearby needle edge can be closer to the uniform-angle prior
            // than the real printed tick. Give the observed numeric label
            // enough weight to distinguish those competing measured strokes.
            const Choice choice{ratio,angularError+labelError*.5-t.length/radius*12-taper*.8,i};
            if(i<ordinaryCount)candidates.push_back(choice);
            else if(directMajor)recoveredChoices.push_back(choice);
        }
        // Correct a competing short tick only when a unique measured long stroke
        // directly outside the number is longer and closer to the label.
        if(recoveredChoices.size()==1&&(!candidates.empty()||std::isfinite(trackedAngle))) {
            const auto& recovered=majors[recoveredChoices.front().tick].tick;
            const double recoveredError=distance(recovered.angle,labelAngle);
            if(std::all_of(candidates.begin(),candidates.end(),[&](const Choice& c){
                const auto& other=ticks[c.tick];
                return other.length<recovered.length*.70 && distance(other.angle,labelAngle)>recoveredError+2.5;
            }))candidates=recoveredChoices;
        }
        if(candidates.empty()){++result.skipped;continue;}
        std::sort(candidates.begin(),candidates.end(),[](const Choice& a,const Choice& b){return a.cost<b.cost;});
        if(candidates.size()>1&&candidates[1].cost<candidates[0].cost+1.5&&distance(ticks[candidates[0].tick].angle,ticks[candidates[1].tick].angle)>2){++result.skipped;continue;}
        choices.push_back(candidates[0]);
    }
    std::sort(choices.begin(),choices.end(),[](const Choice& a,const Choice& b){return a.cost<b.cost;});
    std::set<int> usedTicks;std::set<double> usedRatios;
    std::vector<QJsonObject> points{first,last};
    for(const auto& c:choices)if(!usedTicks.count(c.tick)&&!usedRatios.count(c.ratio)) {
        usedTicks.insert(c.tick);usedRatios.insert(c.ratio);
        const auto p=majors[c.tick].tick.point;points.push_back({{"x",p.x},{"y",p.y},{"ratio",c.ratio}});
    }
    // Only selected repairs may supply evidence for unlabeled graduations.
    majors.resize(ordinaryCount);
    for(int index:usedTicks)if(index>=ordinaryCount)majors.push_back(recoveredMajors[index-ordinaryCount]);
    std::sort(points.begin(),points.end(),[](const QJsonObject& a,const QJsonObject& b){return a["ratio"].toDouble()<b["ratio"].toDouble();});
    for(auto p:points){
        p["origin_x"]=center.x;p["origin_y"]=center.y;result.points.append(p);
        const cv::Point2f position(p["x"].toDouble(),p["y"].toDouble());
        for(const auto& m:majors)if(m.complete&&cv::norm(m.tick.point-position)<.5) {
            result.geometry.push_back({m.outer,m.inner,p["ratio"].toDouble()});break;
        }
    }
    ScaleCalibration::Mapping mapping;
    if(!ScaleCalibration::valid(result.points,image.cols,image.rows,center)||!ScaleCalibration::build(result.points,center,{},clockwise,mapping)) {
        result.points={};result.error=QStringLiteral(u"数字与长刻度的顺序不一致，原标定保留。");
    } else result.unlabeledPoints=unlabeledMajorAnchors(image,numericTokens,result,mapping,majors,shortStrokes,labels,center,radius,clockwise,printed,surface);
    return result;
}
inline bool mergeCalibration(const QJsonArray& existing,const Calibration& calibration,cv::Point2f hub,int size,bool clockwise,QJsonArray& merged) {
    if(!calibration.valid())return false;
    std::vector<QJsonObject> points;for(const auto& p:calibration.points)points.push_back(p.toObject());
    for(const auto& value:ScaleCalibration::majorAnchors(existing)) {
        const auto p=value.toObject();const double ratio=p["ratio"].toDouble();if(ratio<=0||ratio>=1)continue;
        if(ScaleCalibration::isUnlabeledMajor(p))continue; // Reacquire automatic strokes on each run.
        bool replaced=false;for(const auto& automatic:points)if(std::abs(automatic["ratio"].toDouble()-ratio)<1e-6)replaced=true;
        if(!replaced)points.push_back(p);
    }
    std::sort(points.begin(),points.end(),[](const QJsonObject& a,const QJsonObject& b){return a["ratio"].toDouble()<b["ratio"].toDouble();});
    QJsonArray candidate;for(const auto& p:points)candidate.append(p);ScaleCalibration::Mapping mapping;
    if(!ScaleCalibration::valid(candidate,size,size,hub)||!ScaleCalibration::build(candidate,hub,{},clockwise,mapping))return false;
    for(const auto& value:calibration.unlabeledPoints) {
        const double ratio=value.toObject()["ratio"].toDouble();
        int index=0;
        while(index<candidate.size()&&candidate[index].toObject()["ratio"].toDouble()<ratio-1e-6)++index;
        if(index==0||index==candidate.size()||std::abs(candidate[index].toObject()["ratio"].toDouble()-ratio)<1e-6)continue;
        auto trial=candidate;
        trial.insert(index,value);
        if(ScaleCalibration::valid(trial,size,size,hub)&&ScaleCalibration::build(trial,hub,{},clockwise,mapping))candidate=trial;
    }
    merged=candidate;return true;
}
inline bool mergeEndpoints(const QJsonArray& existing,const Endpoints& endpoints,cv::Point2f center,
                           int size,bool clockwise,QJsonArray& merged) {
    if(!endpoints.valid())return false;
    QJsonArray candidate{endpoints.points[0]};
    for(const auto& p:ScaleCalibration::majorAnchors(existing))if(p.toObject()["ratio"].toDouble()>0&&p.toObject()["ratio"].toDouble()<1)candidate.append(p);
    candidate.append(endpoints.points[1]);ScaleCalibration::Mapping mapping;
    if(!ScaleCalibration::valid(candidate,size,size,center)||!ScaleCalibration::build(candidate,center,{},clockwise,mapping))return false;
    merged=candidate;return true;
}
inline bool valid(const QJsonObject& range) {
    return range["min_val"].isDouble()&&range["max_val"].isDouble()&&
        std::isfinite(range["min_val"].toDouble())&&std::isfinite(range["max_val"].toDouble())&&
        range["min_val"].toDouble()<range["max_val"].toDouble();
}
inline QJsonObject withRange(QJsonObject definition,const QJsonObject& range) {
    if(!valid(range))return definition;
    if(definition["profiles"].isArray()) {
        auto profiles=definition["profiles"].toArray();
        for(int i=0;i<profiles.size();++i)profiles[i]=withRange(profiles[i].toObject(),range);
        definition["profiles"]=profiles;
    } else {
        definition["min_val"]=range["min_val"];definition["max_val"]=range["max_val"];
        definition["unit"]=range["unit"].toString("MPa");
    }
    return definition;
}
}
