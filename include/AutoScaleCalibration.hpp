#pragma once
#include "ScaleCalibration.hpp"
#include <QRegularExpression>
#include <set>

// Coordinates are always rectified image pixels. OCR locates labels, never ticks.
namespace AutoScaleCalibration {
struct Number { double value, confidence; cv::Point2f center; };
struct Tick { cv::Point2f point; double angle, radius, length; };
struct Result { QJsonArray points; QString error,diagnostics; int matchedNumbers{0}; bool valid() const { return error.isEmpty() && points.size()>=3; } };
inline double wrap(double a) { return std::fmod(a+720.,360.); }
inline double distance(double a,double b) { return std::abs(std::remainder(a-b,360.)); }
inline std::vector<Number> numbers(const QJsonArray& tokens,double minimum,double maximum) {
    std::vector<Number> out;
    const QRegularExpression numeric(QStringLiteral("^[+-]?(?:[0-9]+(?:\\.[0-9]*)?|\\.[0-9]+)$"));
    for(const auto& entry:tokens) {
        const auto token=entry.toObject();
        QString text=token["text"].toString().trimmed();
        text.replace(QChar(0xff0e),'.'); text.replace(QChar(0x2212),'-');
        if(!numeric.match(text).hasMatch())continue;
        bool ok=false; const double value=text.toDouble(&ok),score=token["score"].toDouble();
        const auto polygon=token["polygon"].toArray();
        if(!ok || !std::isfinite(value) || value<minimum || value>maximum || !std::isfinite(score) || score<.65 || polygon.size()!=4)continue;
        cv::Point2f center(0,0); bool valid=true;
        for(const auto& vertex:polygon) {
            const auto p=vertex.toArray();
            if(p.size()!=2 || !p[0].isDouble() || !p[1].isDouble() || !std::isfinite(p[0].toDouble()) || !std::isfinite(p[1].toDouble())) {valid=false;break;}
            center+=cv::Point2f(float(p[0].toDouble()),float(p[1].toDouble()));
        }
        if(valid)out.push_back({value,score,center*.25f});
    }
    return out;
}
inline std::vector<Tick> ticks(const cv::Mat& image,cv::Point2f center,double radius,double radialTolerance=.022,double minimumLength=.045,double mergeAngle=1.2) {
    cv::Mat gray;
    if(image.channels()==1)gray=image;else cv::cvtColor(image,gray,cv::COLOR_BGR2GRAY);
    gray=gray.clone();
    // Ignore the hub and rim. Text edges may survive; joint matching rejects them.
    for(int y=0;y<gray.rows;++y)for(int x=0;x<gray.cols;++x) {
        const double r=std::hypot(x-center.x,y-center.y);
        if(r<radius*.48 || r>radius*.98)gray.at<uchar>(y,x)=255;
    }
    std::vector<cv::Vec4f> lines;
    cv::createLineSegmentDetector(cv::LSD_REFINE_STD)->detect(gray,lines);
    std::vector<Tick> out;
    for(const auto& l:lines) {
        const cv::Point2f a{float(l[0]),float(l[1])},b{float(l[2]),float(l[3])};
        const auto mid=(a+b)*.5f,delta=b-a,radial=mid-center;
        const double length=cv::norm(delta),r=cv::norm(radial);
        if(r<radius*.5 || length<radius*minimumLength || length>radius*.24 || std::abs(delta.x*radial.y-delta.y*radial.x)/length>radius*radialTolerance)continue;
        const double angle=wrap(std::atan2(radial.y,radial.x)*180/CV_PI);
        auto it=std::find_if(out.begin(),out.end(),[&](const Tick& t){
            return distance(t.angle,angle)<mergeAngle && std::abs(t.radius-r)<(t.length+length)*.5+radius*.015;
        });
        if(it==out.end())out.push_back({mid,angle,r,length});
        else {
            // Major ticks often have a thin stem and thick inner end. LSD splits
            // that transition; join touching radial intervals before length ranking.
            const double inner=std::min(it->radius-it->length*.5,r-length*.5);
            const double outer=std::max(it->radius+it->length*.5,r+length*.5);
            const double mergedAngle=wrap(it->angle+std::remainder(angle-it->angle,360.)*length/(length+it->length));
            const double mergedRadius=(inner+outer)*.5;
            const auto mergedPoint=center+cv::Point2f(float(mergedRadius*std::cos(mergedAngle*CV_PI/180)),float(mergedRadius*std::sin(mergedAngle*CV_PI/180)));
            *it={mergedPoint,mergedAngle,mergedRadius,outer-inner};
        }
    }
    return out;
}
inline Result match(const std::vector<Number>& labels,const std::vector<Tick>& lines,
                    cv::Point2f center,double radius,double minimum,double maximum,bool clockwise) {
    Result result;
    if(!std::isfinite(minimum)||!std::isfinite(maximum)||maximum<=minimum||radius<=0) {result.error=QStringLiteral(u"模板量程或表盘半径无效。");return result;}
    const double sign=clockwise?1:-1;
    struct Pair { int number,tick; double ratio; };
    std::vector<Pair> pairs;
    for(int n=0;n<int(labels.size());++n) {
        const auto& label=labels[n]; const auto d=label.center-center; const double r=cv::norm(d);
        if(r<radius*.35 || r>radius*.96)continue;
        const double a=wrap(std::atan2(d.y,d.x)*180/CV_PI);
        for(int t=0;t<int(lines.size());++t)
            if(distance(a,lines[t].angle)<=12 && std::abs(r-lines[t].radius)<radius*.32)
                pairs.push_back({n,t,(label.value-minimum)/(maximum-minimum)});
    }
    struct Hypothesis { double start,ring,score; std::vector<Pair> inliers; };
    std::vector<Hypothesis> hypotheses;
    for(const auto& seed:pairs) {
        const double start=wrap(lines[seed.tick].angle-sign*270*seed.ratio),ring=lines[seed.tick].radius;
        std::vector<Pair> inliers; double score=0;
        // At most one observation per value and one label per physical tick.
        auto candidates=pairs;
        std::sort(candidates.begin(),candidates.end(),[&](const Pair& a,const Pair& b) {
            auto cost=[&](const Pair& p){return distance(lines[p.tick].angle,wrap(start+sign*270*p.ratio))+.15*distance(lines[p.tick].angle,wrap(std::atan2(labels[p.number].center.y-center.y,labels[p.number].center.x-center.x)*180/CV_PI));};
            return cost(a)<cost(b);
        });
        std::set<int> usedTicks; std::set<double> usedValues;
        for(const auto& p:candidates) {
            const auto& tick=lines[p.tick]; const double error=distance(tick.angle,wrap(start+sign*270*p.ratio));
            if(error>4 || std::abs(tick.radius-ring)>radius*.065 || usedTicks.count(p.tick)||usedValues.count(labels[p.number].value))continue;
            // A major tick must be comparable in length to nearby ticks in this ring.
            double longest=tick.length;
            for(const auto& other:lines)if(distance(other.angle,tick.angle)<15 && std::abs(other.radius-tick.radius)<radius*.12)longest=std::max(longest,other.length);
            if(tick.length<longest*.65)continue;
            usedTicks.insert(p.tick);usedValues.insert(labels[p.number].value);inliers.push_back(p);
            score+=2+labels[p.number].confidence-error*.15;
        }
        if(inliers.size()<3)continue;
        std::sort(inliers.begin(),inliers.end(),[](const Pair& a,const Pair& b){return a.ratio<b.ratio;});
        if(inliers.back().ratio-inliers.front().ratio<.3)continue;
        hypotheses.push_back({start,ring,score,inliers});
    }
    if(hypotheses.empty()) {result.error=QStringLiteral(u"未找到至少三个一致的数字与大刻度匹配，请检查量程、方向、270°表型及图像清晰度。");return result;}
    std::sort(hypotheses.begin(),hypotheses.end(),[](const Hypothesis& a,const Hypothesis& b){return a.score>b.score;});
    auto best=hypotheses.front();
    for(const auto& other:hypotheses)
        if((distance(other.start,best.start)>8 || std::abs(other.ring-best.ring)>radius*.1) && other.score>best.score*.9) {
            result.diagnostics=QString("best start=%1 ring=%2 score=%3 count=%4; alternative start=%5 ring=%6 score=%7 count=%8")
                .arg(best.start).arg(best.ring).arg(best.score).arg(best.inliers.size()).arg(other.start).arg(other.ring).arg(other.score).arg(other.inliers.size());
            for(const auto& h:{best,other})for(const auto& p:h.inliers)result.diagnostics+=QString(" | ratio=%1 angle=%2 r=%3 len=%4").arg(p.ratio).arg(lines[p.tick].angle).arg(lines[p.tick].radius).arg(lines[p.tick].length);
            result.error=QStringLiteral(u"存在多组相近的刻度匹配（可能为双圈刻度），请手动标定或检查模板量程。");return result;
        }
    // Robust start estimate; keep actual measured intermediate angles afterwards.
    std::vector<double> starts,lengths;
    for(const auto& p:best.inliers) {
        starts.push_back(std::remainder(lines[p.tick].angle-sign*270*p.ratio-best.start,360.));
        lengths.push_back(lines[p.tick].length);
    }
    std::sort(lengths.begin(),lengths.end());const double majorLength=lengths[lengths.size()/2];
    std::sort(starts.begin(),starts.end());best.start=wrap(best.start+starts[starts.size()/2]);
    auto append=[&](const Tick& tick,double ratio) {result.points.append(QJsonObject{{"x",tick.point.x},{"y",tick.point.y},{"ratio",ratio}});};
    auto endpoint=[&](double ratio)->int {
        for(const auto& p:best.inliers)if(std::abs(p.ratio-ratio)<1e-8)return p.tick;
        int found=-1;double cost=4;
        for(int t=0;t<int(lines.size());++t) {
            if(std::abs(lines[t].radius-best.ring)>radius*.065 || lines[t].length<majorLength*.65)continue;
            const double e=distance(lines[t].angle,wrap(best.start+sign*270*ratio));
            double longest=lines[t].length;
            for(const auto& other:lines)if(distance(other.angle,lines[t].angle)<15 && std::abs(other.radius-lines[t].radius)<radius*.12)longest=std::max(longest,other.length);
            if(e<cost && lines[t].length>=longest*.65){cost=e;found=t;}
        }
        return found;
    };
    const int first=endpoint(0),last=endpoint(1);
    if(first<0 || last<0 || std::abs(ScaleCalibration::span(lines[first].angle,lines[last].angle,clockwise)-270)>6) {
        result.error=QStringLiteral(u"270°预测位置缺少可靠起止大刻度线；原标定保留，请手动补充。");return result;
    }
    append(lines[first],0);
    for(const auto& p:best.inliers)if(p.ratio>1e-8&&p.ratio<1-1e-8)append(lines[p.tick],p.ratio);
    append(lines[last],1);result.matchedNumbers=int(best.inliers.size());
    ScaleCalibration::Mapping mapping;
    if(!ScaleCalibration::build(result.points,center,{},clockwise,mapping)) {result.points={};result.error=QStringLiteral(u"自动刻度排列无效，请手动标定。");}
    return result;
}
}
