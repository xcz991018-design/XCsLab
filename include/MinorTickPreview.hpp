#pragma once
#include "AutoScaleCalibration.hpp"
#include "LiquidSurface.hpp"
#include <limits>

// Detection proposals are staged separately; callers explicitly adopt validated points.
namespace MinorTickPreview {
struct Mark {
    cv::Point2f a,b; double confidence{0}; bool gap{false};
    double ratio{-1}; bool assigned{false};
};
struct Result {
    std::vector<Mark> marks;
    double division{0},confidence{0}; int intervals{0}; QString message;
    bool divisionConfirmed{false};
    QJsonArray previewPoints; // Private proposal; never replaces saved anchors.
};
// A count hypothesis must produce a representable decimal step, not a
// repeating quotient from a missed tick (e.g. 0.2 / 9). Never round that
// hypothesis into another division; discard it and keep uncertainty explicit.
inline bool decimalDivision(double value) {
    if(!std::isfinite(value)||value<=0)return false;
    double factor=1;
    for(int digits=0;digits<=6;++digits,factor*=10)
        if(std::abs(std::round(value*factor)/factor-value)<=value*1e-8)return true;
    return false;
}
inline QString displayValue(double value) {
    QString text=QString::number(value,'f',6);
    while(text.endsWith('0'))text.chop(1);
    if(text.endsWith('.'))text.chop(1);
    return text;
}
struct Comparison {bool valid{false};double baseline{0},enhanced{0},delta{0};QString message;};
inline Comparison compare(const Result& result,const QJsonArray& anchors,cv::Point2f hub,
                          bool clockwise,double minimum,double maximum,double pointerAngle) {
    Comparison out;ScaleCalibration::Mapping base,enhanced;
    if(!std::isfinite(pointerAngle)||maximum<=minimum||!ScaleCalibration::build(anchors,hub,{},clockwise,base)) {
        out.message=QStringLiteral(u"读数对比不可用：指针方向或大刻度标定无效。");return out;
    }
    const double offset=ScaleCalibration::span(base.angles.front(),pointerAngle,clockwise);
    if(offset>base.offsets.back()) {out.message=QStringLiteral(u"指针位于有效量程之外，未输出对比读数。");return out;}
    out.baseline=minimum+(maximum-minimum)*ScaleCalibration::ratio(base,offset);
    if(!result.divisionConfirmed||!ScaleCalibration::build(result.previewPoints,hub,{},clockwise,enhanced)) {
        out.message=QStringLiteral(u"仅大刻度读数 %1；小刻度赋值尚未确认，增强读数待确认。").arg(out.baseline,0,'f',6);return out;
    }
    // Comparison requires two measured minor ticks around the needle; do not
    // present an unchanged/unconstrained section as locally improved.
    double lower=-1,upper=1e9;
    for(const auto& m:result.marks)if(m.assigned) {
        const auto p=(m.a+m.b)*.5f;const auto o=ScaleCalibration::origin(anchors.first().toObject(),hub);
        const double a=AutoScaleCalibration::wrap(std::atan2(p.y-o.y,p.x-o.x)*180/CV_PI);
        const double x=ScaleCalibration::span(base.angles.front(),a,clockwise);
        if(x<=offset)lower=std::max(lower,x);if(x>=offset)upper=std::min(upper,x);
    }
    if(lower<0||upper>base.offsets.back()||upper-lower>result.division/(maximum-minimum)*base.offsets.back()*3) {
        out.message=QStringLiteral(u"仅大刻度读数 %1；指针附近缺少可靠小刻度约束，暂不输出增强读数。").arg(out.baseline,0,'f',6);return out;
    }
    out.enhanced=minimum+(maximum-minimum)*ScaleCalibration::ratio(enhanced,offset);
    out.delta=out.enhanced-out.baseline;out.valid=true;
    out.message=QStringLiteral(u"同一指针方向：仅大刻度 %1；加入小刻度 %2；差值 %3。差值不代表精度已提高，需独立参考值验证。")
        .arg(out.baseline,0,'f',6).arg(out.enhanced,0,'f',6).arg(out.delta,0,'f',6);
    return out;
}
inline Result detect(const cv::Mat& image,const QJsonArray& anchors,cv::Point2f hub,double radius,
                     double minimum,double maximum,bool clockwise,const LiquidSurface& surface,double pointerAngle=std::numeric_limits<double>::quiet_NaN(),double confirmedDivision=0) {
    Result out; ScaleCalibration::Mapping mapping;
    const bool manualDivision=std::isfinite(confirmedDivision)&&confirmedDivision>0;
    if(image.empty()||image.depth()!=CV_8U||radius<30||!std::isfinite(maximum-minimum)||maximum<=minimum||
       !ScaleCalibration::valid(anchors,image.cols,image.rows,hub)||
       !ScaleCalibration::build(anchors,hub,{},clockwise,mapping)) {
        out.message=QStringLiteral(u"请先确认轴心、量程及至少两个有效大刻度锚点。");return out;
    }
    if(surface.ambiguous){out.message=QStringLiteral(u"液面有歧义，请先手动确认液面。");return out;}
    const auto center=ScaleCalibration::origin(anchors.first().toObject(),hub);
    for(const auto& v:anchors)if(cv::norm(ScaleCalibration::origin(v.toObject(),hub)-center)>1) {
        out.message=QStringLiteral(u"锚点使用不同刻度中心，请先统一几何标定。");return out;
    }
    cv::Mat gray;
    if(image.channels()==1)gray=image;
    else cv::cvtColor(image,gray,image.channels()==4?cv::COLOR_BGRA2GRAY:cv::COLOR_BGR2GRAY);
    // Local contrast survives the shaded liquid region; absolute brightness does not.
    cv::Mat ink;
    int window=std::max(15,cvRound(radius*.085)|1);
    cv::adaptiveThreshold(gray,ink,255,cv::ADAPTIVE_THRESH_GAUSSIAN_C,cv::THRESH_BINARY_INV,window,7);
    const auto lines=AutoScaleCalibration::ticks(gray,center,radius,.09,.018);
    auto point=[&](double angle,double r){const double a=angle*CV_PI/180;return center+cv::Point2f(float(r*std::cos(a)),float(r*std::sin(a)));};
    struct Model {int interval,n;double division,score,phase,step;};std::vector<Model> models;
    struct Interval {double start,sweep,leftRatio,rightRatio,ring;std::vector<int> marks;double middleOffset{-1},middleRatio{-1};bool minimumSide{true};};
    std::vector<Interval> intervals;
    for(int interval=1;interval<anchors.size();++interval) {
        const auto left=anchors[interval-1].toObject(),right=anchors[interval].toObject();
        const double start=mapping.angles[interval-1],sweep=mapping.offsets[interval]-mapping.offsets[interval-1];
        const double r0=cv::norm(cv::Point2f(float(left["x"].toDouble()),float(left["y"].toDouble()))-center);
        const double r1=cv::norm(cv::Point2f(float(right["x"].toDouble()),float(right["y"].toDouble()))-center);
        const double ring=(r0+r1)*.5;
        if(sweep<4||sweep>300||std::abs(r0-r1)>radius*.12)continue;
        auto angleAt=[&](double offset){return start+(clockwise?offset:-offset);};
        // Angular/radial strip: local dark runs expose short strokes without counting their two edges.
        const int bins=std::max(20,int(std::ceil(sweep/.2)));
        std::vector<double> signal(bins+1,0);std::vector<bool> visible(bins+1,true);
        for(int i=0;i<=bins;++i) {
            const double angle=angleAt(sweep*i/bins);int run=0,best=0;
            for(double r=ring-radius*.065;r<=ring+radius*.14;r+=1) {
                const auto p=point(angle,r);const int x=cvRound(p.x),y=cvRound(p.y);
                if(x<0||y<0||x>=gray.cols||y>=gray.rows||surface.excludes(p.x,p.y)){visible[i]=false;break;}
                if(ink.at<uchar>(y,x)){best=std::max(best,++run);}else run=0;
            }
            if(visible[i])signal[i]=best;
        }
        struct Candidate {AutoScaleCalibration::Tick tick;double offset,confidence;};std::vector<Candidate> candidates;
        for(const auto& t:lines) {
            const double offset=ScaleCalibration::span(start,t.angle,clockwise);
            const auto a=point(t.angle,t.radius-t.length*.5),b=point(t.angle,t.radius+t.length*.5);
            if(std::isfinite(pointerAngle)&&AutoScaleCalibration::distance(t.angle,pointerAngle)<2.5)continue;
            if(offset<1.4||offset>sweep-1.4||std::abs(t.radius-ring)>radius*.13||
               // Small ticks share the outer ring with major ticks. A glyph
               // fragment may be radial but stops inside the major midpoint.
               t.radius+t.length*.5<ring-radius*.025||
               t.length>radius*.19||surface.excludes(a.x,a.y)||surface.excludes(b.x,b.y))continue;
            const int bin=std::clamp(cvRound(offset/sweep*bins),0,bins);
            double strength=0;for(int k=std::max(0,bin-2);k<=std::min(bins,bin+2);++k)strength=std::max(strength,signal[k]);
            if(strength<radius*.018)continue;
            const double confidence=std::clamp(std::min(strength,t.length)/(radius*.065),0.,1.);
            // Oblique short strokes can yield separated LSD sides. Merge nearby
            // measured centers, retaining one physical stroke rather than two edges.
            auto duplicate=std::find_if(candidates.begin(),candidates.end(),[&](const Candidate& c){
                return AutoScaleCalibration::distance(c.tick.angle,t.angle)<2.0&&
                       cv::norm(c.tick.point-t.point)<radius*.025;
            });
            if(duplicate==candidates.end())candidates.push_back({t,offset,confidence});
            else if(t.length>duplicate->tick.length)*duplicate={t,offset,confidence};
        }
        const int id=int(intervals.size());
        Interval section{start,sweep,left["ratio"].toDouble(),right["ratio"].toDouble(),ring,{}};
        for(const auto& c:candidates) {
            section.marks.push_back(int(out.marks.size()));
            out.marks.push_back({point(c.tick.angle,c.tick.radius-c.tick.length*.5),point(c.tick.angle,c.tick.radius+c.tick.length*.5),c.confidence,false});
        }
        section.minimumSide=(interval==1);
        // A unique central long stroke proposes a half-major anchor, expressed
        // in ratios so the same geometry works for any range or zero offset.
        // Require distributed strokes on both sides; length alone cannot assign a value.
        const double middleRatio=(section.leftRatio+section.rightRatio)*.5;
        if(interval==1||interval==anchors.size()-1) {
            std::vector<const Candidate*> central;std::vector<double> lengths;
            for(const auto& c:candidates){lengths.push_back(c.tick.length);if(c.offset>sweep*.35&&c.offset<sweep*.65&&c.confidence>=.6)central.push_back(&c);}
            std::sort(lengths.begin(),lengths.end());
            std::sort(central.begin(),central.end(),[](const Candidate* a,const Candidate* b){return a->tick.length>b->tick.length;});
            if(!central.empty()&&!lengths.empty()&&central[0]->tick.length>lengths[lengths.size()/2]*1.3&&
               (central.size()==1||central[0]->tick.length>central[1]->tick.length*1.2)) {
                int before=0,after=0;
                for(const auto& c:candidates)if(c.offset<central[0]->offset-1)++before;else if(c.offset>central[0]->offset+1)++after;
                if(before>=3&&after>=3&&std::abs(before-after)<=2) {
                    section.middleOffset=central[0]->offset;section.middleRatio=middleRatio;
                }
            }
        }
        intervals.push_back(section);
        // Keep competing subdivision counts for cross-interval consensus.
        for(int n=3;n<=80;++n) {
            double step=sweep/n,phase=0;
            if(section.middleOffset>=0) {
                const double index=n*(section.middleRatio-section.leftRatio)/(section.rightRatio-section.leftRatio);
                if(std::abs(index-std::round(index))>.001)continue;
                if(section.minimumSide){step=(sweep-section.middleOffset)/(n-index);phase=sweep-n*step;}
                else {step=section.middleOffset/index;phase=0;}
            }
            if(step<1.8)continue;
            std::set<int> occupied;int aligned=0;double error=0;
            for(const auto& c:candidates){const int k=cvRound((c.offset-phase)/step);const double e=std::abs(c.offset-phase-k*step)/step;
                if(k>0&&k<n&&e<.22){occupied.insert(k);++aligned;error+=e;}}
            int visibleCount=0;for(int k=1;k<n;++k){const double x=phase+k*step;
                if(x>1.4&&x<sweep-1.4)visibleCount+=visible[std::clamp(cvRound(x/sweep*bins),0,bins)];}
            if(occupied.size()<3||visibleCount<3)continue;
            const double score=std::min(1.,double(occupied.size())/visibleCount)*double(aligned)/std::max(size_t(1),candidates.size())*(1-error/std::max(1,aligned));
            const double division=(maximum-minimum)*(section.rightRatio-section.leftRatio)/n;
            if(!manualDivision&&!decimalDivision(division))continue;
            if(manualDivision&&std::abs(division-confirmedDivision)>confirmedDivision*.001)continue;
            if(score>=(manualDivision?.35:.55))models.push_back({id,n,(maximum-minimum)*(section.rightRatio-section.leftRatio)/n,score,phase,step});
        }
    }
    struct Consensus {double division,score;std::vector<Model> support;};std::vector<Consensus> groups;
    for(const auto& seed:models) {
        if(std::any_of(groups.begin(),groups.end(),[&](const Consensus& c){return std::abs(c.division-seed.division)<seed.division*.03;}))continue;
        Consensus c{seed.division,0,{}};
        for(int id=0;id<int(intervals.size());++id) {
            const Model* selected=nullptr;double best=0;
            for(const auto& m:models)if(m.interval==id) {
                best=std::max(best,m.score);
                if(std::abs(m.division-seed.division)<seed.division*.03&&(!selected||m.score>selected->score))selected=&m;
            }
            if(selected&&selected->score>=(manualDivision?.35:.60)&&selected->score>=best-.12){c.support.push_back(*selected);c.score+=selected->score;}
        }
        if(!c.support.empty())groups.push_back(c);
    }
    std::sort(groups.begin(),groups.end(),[](const Consensus& a,const Consensus& b){return a.score>b.score;});
    if(!groups.empty()) {
        const auto& best=groups.front();out.division=best.division;out.intervals=int(best.support.size());
        out.confidence=best.score/best.support.size();
        const double margin=groups.size()>1?best.score-groups[1].score:best.score;
        int unambiguous=0;
        for(const auto& selected:best.support) {
            double alternative=0;
            for(const auto& m:models)if(m.interval==selected.interval&&m.n!=selected.n)alternative=std::max(alternative,m.score);
            if(selected.score-alternative>=.10)++unambiguous;
        }
        out.divisionConfirmed=manualDivision||(unambiguous>=2&&margin>=.20);
        if(!out.divisionConfirmed)out.confidence*=.5;
        out.previewPoints=anchors;
        for(const auto& model:best.support) {
            const auto& section=intervals[model.interval];const double step=model.step;
            std::vector<std::vector<int>> slotsByIndex(model.n+1);
            for(int index:section.marks) {
                const auto& m=out.marks[index];const auto mid=(m.a+m.b)*.5f;
                const double a=AutoScaleCalibration::wrap(std::atan2(mid.y-center.y,mid.x-center.x)*180/CV_PI);
                const double offset=ScaleCalibration::span(section.start,a,clockwise);const int k=cvRound((offset-model.phase)/step);
                if(k>0&&k<model.n&&std::abs(offset-model.phase-k*step)<step*(manualDivision?.35:.22)&&m.confidence>=.60)slotsByIndex[k].push_back(index);
            }
            for(int k=1;k<model.n;++k) {
                const double ratio=section.leftRatio+(section.rightRatio-section.leftRatio)*k/model.n;
                if(slotsByIndex[k].size()==1) {
                    auto& m=out.marks[slotsByIndex[k].front()];m.ratio=ratio;m.assigned=out.divisionConfirmed;
                    m.confidence=std::min(m.confidence,model.score);
                    if(m.assigned) {const auto p=(m.a+m.b)*.5f;out.previewPoints.append(QJsonObject{{"x",p.x},{"y",p.y},{"ratio",ratio},{"origin_x",center.x},{"origin_y",center.y},{"source","minor_tick"}});}
                } else if(slotsByIndex[k].empty()) {
                    const double expectedOffset=model.phase+k*step;
                    if(expectedOffset<1.4||expectedOffset>section.sweep-1.4)continue;
                    const double a=section.start+(clockwise?1:-1)*expectedOffset;
                    const auto p=point(a,section.ring),a0=point(a,section.ring-radius*.065),b0=point(a,section.ring+radius*.14);
                    if(!surface.excludes(a0.x,a0.y)&&!surface.excludes(b0.x,b0.y)&&p.x>=0&&p.y>=0&&p.x<image.cols&&p.y<image.rows)
                        out.marks.push_back({point(a,section.ring-radius*.025),point(a,section.ring+radius*.025),model.score,true});
                }
            }
        }
        std::vector<QJsonObject> ordered;for(const auto& p:out.previewPoints)ordered.push_back(p.toObject());
        std::sort(ordered.begin(),ordered.end(),[](const QJsonObject& a,const QJsonObject& b){return a["ratio"].toDouble()<b["ratio"].toDouble();});
        out.previewPoints={};for(const auto& p:ordered)out.previewPoints.append(p);
        ScaleCalibration::Mapping checked;
        if(!ScaleCalibration::valid(out.previewPoints,image.cols,image.rows,hub)||!ScaleCalibration::build(out.previewPoints,hub,{},clockwise,checked)) {
            out.divisionConfirmed=false;out.previewPoints={};for(auto& m:out.marks)m.assigned=false;
        }
    }
    for(auto& m:out.marks)if(!m.gap&&m.ratio<0) {
        const auto p=(m.a+m.b)*.5f-center;
        const double angle=AutoScaleCalibration::wrap(std::atan2(p.y,p.x)*180/CV_PI);
        m.ratio=ScaleCalibration::ratio(mapping,ScaleCalibration::span(mapping.angles.front(),angle,clockwise));
        for(const auto& section:intervals)if(section.middleOffset>=0) {
            const double offset=ScaleCalibration::span(section.start,angle,clockwise);
            if(offset<section.sweep) {
                if(section.minimumSide)m.ratio=std::clamp(section.middleRatio+(offset-section.middleOffset)*(section.rightRatio-section.middleRatio)/(section.sweep-section.middleOffset),0.,1.);
                else m.ratio=std::clamp(section.leftRatio+offset*(section.middleRatio-section.leftRatio)/section.middleOffset,0.,1.);
            }
        }
        if(out.division>0) {
            const double step=out.division/(maximum-minimum);
            m.ratio=std::clamp(std::round(m.ratio/step)*step,0.,1.);
        }
    }
    // Numerical labels must not make an unconfirmed subdivision look usable.
    if(!out.divisionConfirmed)for(auto& m:out.marks){m.ratio=-1;m.assigned=false;}
    const int gaps=int(std::count_if(out.marks.begin(),out.marks.end(),[](const Mark& m){return m.gap;}));
    out.message=QStringLiteral(u"小刻度预览：%1 条候选（紫色），%2 处推断缺口（橙色虚线）；候选分度值 %3；规则置信度 %4%。%5 可选择在保存时采用已确认的小刻度。")
        .arg(int(out.marks.size())-gaps).arg(gaps).arg(out.divisionConfirmed?displayValue(out.division):QStringLiteral(u"待确认"))
        .arg(qRound(out.confidence*100)).arg(!out.divisionConfirmed?QStringLiteral(u"分度尚未通过多个区间验证，暂不显示或保存数值，避免漏检导致错位赋值。 "):(manualDivision?QStringLiteral(u"使用人工指定分度。 "):QStringLiteral(u"分度已通过多个区间验证。 ")));
    if(!intervals.empty()&&intervals.front().middleOffset>=0)
        out.message+=QStringLiteral(u" 自动推断起始中刻度 %1（由相邻大刻度数值确定），按内侧间距向最小端反推。").arg(minimum+intervals.front().middleRatio*(maximum-minimum));
    const int assigned=int(std::count_if(out.marks.begin(),out.marks.end(),[](const Mark& m){return m.assigned;}));
    out.message+=QStringLiteral(u" 可靠赋值 %1 条（青蓝色）；缺口不生成标定点。").arg(assigned);
    return out;
}
}

