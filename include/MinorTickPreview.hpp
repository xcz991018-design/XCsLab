#pragma once
#include "AutoScaleCalibration.hpp"
#include "OcrDialRange.hpp"
#include "LiquidSurface.hpp"
#include <limits>
#include <functional>

// Detection proposals are staged separately; callers explicitly adopt validated points.
namespace MinorTickPreview {
struct Mark {
    cv::Point2f a,b; double confidence{0}; bool gap{false};
    double ratio{-1}; bool assigned{false};
    bool inferred{false};
};
struct Result {
    std::vector<Mark> marks;
    double division{0},confidence{0}; int intervals{0}; QString message;
    bool divisionConfirmed{false};
    QJsonArray previewPoints; // Private proposal; never replaces saved anchors.
    QStringList coverage;
};
inline QJsonArray readingAnchors(const Result& result,const QJsonArray& major,cv::Size size,cv::Point2f hub,
                                 double range,bool clockwise) {
    ScaleCalibration::Mapping base;
    if(!result.divisionConfirmed||!std::isfinite(range)||!std::isfinite(result.division)||
       range<=0||result.division<=0||result.division>=range||
       !ScaleCalibration::valid(major,size.width,size.height,hub)||
       !ScaleCalibration::buildForPointer(major,hub,{},clockwise,base))return major;
    const double step=result.division/range;
    QJsonArray reading{major.first()};
    for(size_t i=1;i<base.ratios.size();++i) {
        struct Candidate {QJsonObject point;double ratio,offset;};
        std::vector<Candidate> measured;
        const double low=base.ratios[i-1],high=base.ratios[i];
        const double pitch=step*(base.offsets[i]-base.offsets[i-1])/(high-low);
        for(const auto& item:result.previewPoints) {
            auto p=item.toObject();
            const double ratio=p["ratio"].toDouble();
            if(p["source"]!="minor_tick"||ratio<=low||ratio>=high)continue;
            const cv::Point2f position(p["x"].toDouble(),p["y"].toDouble());
            if(std::none_of(result.marks.begin(),result.marks.end(),[&](const Mark& mark){
                return mark.assigned&&!mark.gap&&!mark.inferred&&mark.confidence>=.6&&
                       std::abs(mark.ratio-ratio)<1e-6&&cv::norm((mark.a+mark.b)*.5f-position)<1;
            }))continue;
            const double angle=std::atan2(position.y-hub.y,position.x-hub.x)*180/CV_PI;
            const double offset=ScaleCalibration::span(base.angles.front(),angle,clockwise);
            const double expected=base.offsets[i-1]+(ratio-low)*pitch/step;
            if(offset<=base.offsets[i-1]||offset>=base.offsets[i]||std::abs(offset-expected)>pitch*.45)continue;
            p["source"]=QStringLiteral("minor_tick_verified");
            measured.push_back({p,ratio,offset});
        }
        // Use adjacent measured divisions whose angle and value increments agree.
        // Isolated observations cannot change the interpolation.
        for(size_t j=0;j<measured.size();++j) {
            const auto consistent=[&](const Candidate& a,const Candidate& b) {
                return std::abs(b.ratio-a.ratio-step)<=step*.01&&std::abs(b.offset-a.offset-pitch)<=pitch*.30;
            };
            if((j>0&&consistent(measured[j-1],measured[j]))||
               (j+1<measured.size()&&consistent(measured[j],measured[j+1])))reading.append(measured[j].point);
        }
        reading.append(major[int(i)]);
    }
    ScaleCalibration::Mapping checked;
    if(!ScaleCalibration::valid(reading,size.width,size.height,hub)||
       !ScaleCalibration::buildForPointer(reading,hub,{},clockwise,checked))return major;
    return reading;
}
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
// Fill only a single missing division bracketed by measured points, supported
// by a neighboring measured pitch and actual pointer-mask overlap. Inferred
// points remain explicitly distinct from measured assignment evidence.
inline int fillPointerGaps(Result& out,cv::Point2f hub,double radius,double minimum,double maximum,
                           bool clockwise,const LiquidSurface& surface,const cv::Mat& mask) {
    if(!out.divisionConfirmed||out.division<=0||maximum<=minimum||mask.empty()||mask.type()!=CV_8UC1)return 0;
    QJsonArray measured;
    for(const auto& p:out.previewPoints)if(p.toObject()["source"].toString()!="minor_tick_inferred")measured.append(p);
    ScaleCalibration::Mapping mapping;
    if(measured.size()<4||!ScaleCalibration::build(measured,hub,{},clockwise,mapping))return 0;
    const double step=out.division/(maximum-minimum),tolerance=step*.001;
    // Allow a small positional tolerance between the local graduation lattice
    // and the measured needle silhouette (including its antialiased boundary).
    cv::Mat occlusion;
    const int margin=std::max(1,cvRound(radius*.012));
    cv::dilate(mask,occlusion,cv::getStructuringElement(cv::MORPH_ELLIPSE,{2*margin+1,2*margin+1}));
    auto points=out.previewPoints;std::vector<Mark> additions;
    for(int i=0;i+1<measured.size();++i) {
        if(std::abs(mapping.ratios[i+1]-mapping.ratios[i]-2*step)>tolerance)continue;
        const double pitch=(mapping.offsets[i+1]-mapping.offsets[i])*.5;
        bool supported=false,consistent=pitch>1.4;
        for(int j:{i-1,i+1})if(j>=0&&j+1<measured.size()&&
            std::abs(mapping.ratios[j+1]-mapping.ratios[j]-step)<tolerance) {
            supported=true;
            consistent&=std::abs((mapping.offsets[j+1]-mapping.offsets[j])/pitch-1)<.30;
        }
        if(!supported||!consistent)continue;
        const auto left=measured[i].toObject(),right=measured[i+1].toObject();
        const auto origin=ScaleCalibration::origin(left,hub);
        if(cv::norm(origin-ScaleCalibration::origin(right,hub))>1)continue;
        const double angle=(mapping.angles[i]+(clockwise?pitch:-pitch))*CV_PI/180;
        const cv::Point2f direction(float(std::cos(angle)),float(std::sin(angle)));
        const double ring=(cv::norm(cv::Point2f(left["x"].toDouble(),left["y"].toDouble())-origin)+
                           cv::norm(cv::Point2f(right["x"].toDouble(),right["y"].toDouble())-origin))*.5;
        const auto p=origin+direction*float(ring),a=origin+direction*float(ring-radius*.025),b=origin+direction*float(ring+radius*.025);
        if(surface.excludes(a.x,a.y)||surface.excludes(b.x,b.y))continue;
        bool occluded=false;
        // Edge tracking may end before the physical tip. Require alignment
        // with the outer needle body rather than a hit at the missing tick only.
        for(double r=ring*.65;r<=ring+radius*.04;r+=1) {
            const auto q=origin+direction*float(r);const int x=cvRound(q.x),y=cvRound(q.y);
            if(x>=0&&y>=0&&x<mask.cols&&y<mask.rows&&occlusion.at<uchar>(y,x))occluded=true;
        }
        if(!occluded)continue;
        const double ratio=(mapping.ratios[i]+mapping.ratios[i+1])*.5;
        points.append(QJsonObject{{"x",p.x},{"y",p.y},{"ratio",ratio},{"origin_x",origin.x},{"origin_y",origin.y},
            {"source","minor_tick_inferred"},{"inference","pointer_occlusion_interpolation"},
            {"left_ratio",mapping.ratios[i]},{"right_ratio",mapping.ratios[i+1]}});
        additions.push_back({a,b,out.confidence*.75,true,ratio,false,true});
    }
    std::vector<QJsonObject> ordered;for(const auto& p:points)ordered.push_back(p.toObject());
    std::sort(ordered.begin(),ordered.end(),[](const QJsonObject& a,const QJsonObject& b){return a["ratio"].toDouble()<b["ratio"].toDouble();});
    points={};for(const auto& p:ordered)points.append(p);
    ScaleCalibration::Mapping checked;
    if(additions.empty()||!ScaleCalibration::valid(points,mask.cols,mask.rows,hub)||
       !ScaleCalibration::build(points,hub,{},clockwise,checked))return 0;
    out.previewPoints=points;
    for(const auto& inferred:additions) {
        out.marks.erase(std::remove_if(out.marks.begin(),out.marks.end(),[&](const Mark& m){
            return !m.assigned&&m.ratio>=0&&std::abs(m.ratio-inferred.ratio)<tolerance;
        }),out.marks.end());
        out.marks.push_back(inferred);
    }
    return int(additions.size());
}
struct Comparison {bool valid{false};double baseline{0},enhanced{0},delta{0};QString message;};
inline Comparison compare(const Result& result,const QJsonArray& anchors,cv::Size size,cv::Point2f hub,
                          bool clockwise,double minimum,double maximum,double pointerAngle) {
    Comparison out;ScaleCalibration::Mapping base,enhanced;
    if(!std::isfinite(pointerAngle)||maximum<=minimum||!ScaleCalibration::buildForPointer(anchors,hub,{},clockwise,base)) {
        out.message=QStringLiteral(u"读数对比不可用：指针方向或大刻度标定无效。");return out;
    }
    const double offset=ScaleCalibration::span(base.angles.front(),pointerAngle,clockwise);
    if(offset>base.offsets.back()) {out.message=QStringLiteral(u"指针位于有效量程之外，未输出对比读数。");return out;}
    out.baseline=minimum+(maximum-minimum)*ScaleCalibration::ratio(base,offset);
    const auto reading=readingAnchors(result,anchors,size,hub,maximum-minimum,clockwise);
    if(reading.size()==anchors.size()||!ScaleCalibration::buildForPointer(reading,hub,{},clockwise,enhanced)) {
        out.message=QStringLiteral(u"仅大刻度读数 %1；小刻度赋值尚未确认，增强读数待确认。").arg(out.baseline,0,'f',6);return out;
    }
    // Comparison requires two measured minor ticks around the needle; do not
    // present an unchanged/unconstrained section as locally improved.
    double lower=-1,upper=1e9;
    for(int i=0;i<reading.size();++i)if(ScaleCalibration::isVerifiedMinor(reading[i].toObject())) {
        const double x=enhanced.offsets[i];
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
                     double minimum,double maximum,bool clockwise,const LiquidSurface& requestedSurface,double pointerAngle=std::numeric_limits<double>::quiet_NaN(),double confirmedDivision=0,QJsonArray* diagnostics=nullptr, std::function<void(Result)> showLong={},const cv::Mat& pointerMask={}) {
    Result out; ScaleCalibration::Mapping mapping;
    const bool manualDivision=std::isfinite(confirmedDivision)&&confirmedDivision>0;
    if(image.empty()||image.depth()!=CV_8U||radius<30||!std::isfinite(maximum-minimum)||maximum<=minimum||
       !ScaleCalibration::valid(anchors,image.cols,image.rows,hub)||
       !ScaleCalibration::build(anchors,hub,{},clockwise,mapping)) {
        out.message=QStringLiteral(u"请先确认轴心、量程及至少两个有效大刻度锚点。");return out;
    }
    const auto surface=requestedSurface;
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
    auto detectionGray=gray.clone();
    const bool hasPointerMask=pointerMask.type()==CV_8UC1&&pointerMask.size()==gray.size();
    auto pointerCovers=[&](int x,int y){return hasPointerMask&&x>=0&&y>=0&&x<gray.cols&&y<gray.rows&&pointerMask.at<uchar>(y,x)!=0;};
    if(hasPointerMask) {
        detectionGray.setTo(255,pointerMask);
        ink.setTo(0,pointerMask);
    }
    if(surface.found)for(int y=0;y<gray.rows;++y)for(int x=0;x<gray.cols;++x)
        if(surface.excludes(x,y))detectionGray.at<uchar>(y,x)=255;
    auto lines=AutoScaleCalibration::ticks(detectionGray,center,radius,.09,.018);
    // Preserve complete thick-head/thin-stem measurements. LSD alone may
    // split a middle major into a short, ordinary-looking minor stroke.
    // The same component/ink checks used by major calibration recover its
    // physical extent, before the existing local merge and lattice checks.
    for(const auto& major:OcrDialRange::majorTicks(detectionGray,{},center,radius,surface))
        if(major.tick.length>=radius*.09)lines.push_back(major.tick);
    auto point=[&](double angle,double r){const double a=angle*CV_PI/180;return center+cv::Point2f(float(r*std::cos(a)),float(r*std::sin(a)));};
    struct Model {
        int interval,n;double division,score,phase,step;
        double split{-1},outerStep{0};
        double offset(double index) const {return phase+std::min(index,split<0?index:split)*step+(split>=0?std::max(0.,index-split)*outerStep:0.);}
        double index(double x) const {return split>=0&&x>phase+split*step?split+(x-phase-split*step)/outerStep:(x-phase)/step;}
    };std::vector<Model> models;
    struct Interval {double start,sweep,leftRatio,rightRatio,ring;std::vector<int> marks;double middleOffset{-1},middleRatio{-1};bool minimumSide{true};bool liquidMasked{false};std::vector<int> longMarks;};
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
            const auto ringPoint=point(angle,ring);
            visible[i]=!surface.excludes(ringPoint.x,ringPoint.y);
            for(double r=ring-radius*.065;r<=ring+radius*.14;r+=1) {
                const auto p=point(angle,r);const int x=cvRound(p.x),y=cvRound(p.y);
                // The search strip can extend beyond a visible tick into the
                // liquid. Exclude those samples, not the whole angular bin;
                // the measured stroke itself is checked against the mask below.
                if(x<0||y<0||x>=gray.cols||y>=gray.rows||surface.excludes(p.x,p.y)){run=0;continue;}
                if(ink.at<uchar>(y,x)){best=std::max(best,++run);}else run=0;
            }
            if(visible[i])signal[i]=best;
        }
        struct Candidate {AutoScaleCalibration::Tick tick;double offset,confidence;};std::vector<Candidate> candidates;
        for(const auto& t:lines) {
            const double offset=ScaleCalibration::span(start,t.angle,clockwise);
            const auto a=point(t.angle,t.radius-t.length*.5),b=point(t.angle,t.radius+t.length*.5);
            bool excluded=false;
            for(int sample=0;sample<=8;++sample){const auto p=a+(b-a)*(sample/8.f);excluded|=surface.excludes(p.x,p.y)||pointerCovers(cvRound(p.x),cvRound(p.y));}
            if(diagnostics)diagnostics->append(QJsonObject{{"type","raw_tick"},{"id",int(intervals.size())},
                {"offset",offset},{"radius",t.radius},{"length",t.length},{"ring",ring},{"excluded",excluded}});
            if(excluded||offset<1.4||offset>sweep-1.4||std::abs(t.radius-ring)>radius*.13||
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
                       std::abs(c.tick.radius-t.radius)<(c.tick.length+t.length)*.5+radius*.015;
            });
            if(duplicate==candidates.end())candidates.push_back({t,offset,confidence});
            else if(t.length>duplicate->tick.length)*duplicate={t,offset,confidence};
        }
        // LSD can lose a thin stem or split it at a thick head. Recover only
        // narrow, measured ink runs in the radial strip, never predicted slots.
        const double minimumRun=radius*.035;
        for(int begin=1;begin<bins;) {
            if(!visible[begin]||signal[begin]<minimumRun){++begin;continue;}
            const int runBegin=begin;int end=begin,peak=begin;
            while(end+1<bins&&visible[end+1]&&signal[end+1]>=minimumRun){++end;if(signal[end]>signal[peak])peak=end;}
            const double offset=sweep*peak/bins;
            begin=end+1;
            if(sweep*(end-runBegin+1)/bins>3.5)continue;
            if(offset<1.4||offset>sweep-1.4)continue;
            const double angle=angleAt(offset);
            const auto existing=std::find_if(candidates.begin(),candidates.end(),[&](const Candidate& c){return AutoScaleCalibration::distance(c.tick.angle,angle)<2.;});
            if(existing!=candidates.end()&&existing->confidence>=.6)continue;
            double bestLo=0,bestHi=0,runLo=0;bool inRun=false;
            for(double r=ring-radius*.065;r<=ring+radius*.14;r+=1) {
                const auto p=point(angle,r);const int x=cvRound(p.x),y=cvRound(p.y);
                const bool dark=x>=0&&y>=0&&x<gray.cols&&y<gray.rows&&!surface.excludes(p.x,p.y)&&ink.at<uchar>(y,x);
                if(dark){if(!inRun){runLo=r;inRun=true;}if(r-runLo>bestHi-bestLo){bestLo=runLo;bestHi=r;}}
                else inRun=false;
            }
            const double length=bestHi-bestLo;
            if(diagnostics)diagnostics->append(QJsonObject{{"type","ink_run"},{"id",int(intervals.size())},
                {"offset",offset},{"inner",bestLo},{"outer",bestHi},{"length",length},{"ring",ring}});
            if(length<minimumRun||length>radius*.19||bestHi<ring-radius*.025)continue;
            const auto before=point(angle,bestLo-3),after=point(angle,bestHi+3);
            if(surface.excludes(before.x,before.y-2)||surface.excludes(after.x,after.y-2))continue;
            const double r=(bestLo+bestHi)*.5;
            Candidate recovered{{point(angle,r),AutoScaleCalibration::wrap(angle),r,length},offset,std::clamp(length/(radius*.065),0.,1.)};
            if(existing==candidates.end())candidates.push_back(recovered);
            else if(recovered.confidence>existing->confidence)*existing=recovered;
        }
        const int id=int(intervals.size());
        Interval section{start,sweep,left["ratio"].toDouble(),right["ratio"].toDouble(),ring,{}};
        if(surface.found)for(int i=0;i<=bins;++i) {
            const auto p=point(angleAt(sweep*i/bins),ring);
            if(surface.excludes(p.x,p.y)){section.liquidMasked=true;break;}
        }
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
            for(const auto& c:candidates) {
                lengths.push_back(c.tick.length);
                if(c.offset>sweep*.35&&c.offset<sweep*.65&&c.confidence>=.6)
                    central.push_back(&c);
            }
            // Visible strokes beside the measured needle silhouette remain
            // eligible. Prominence, distribution and cross-interval agreement
            // below verify the half-major's value.
            std::sort(lengths.begin(),lengths.end());
            std::sort(central.begin(),central.end(),[](const Candidate* a,const Candidate* b){return a->tick.length>b->tick.length;});
            // Occlusion can shorten a major stroke. Its unique prominence
            // among central candidates and subsequent cross-interval lattice
            // agreement are stronger evidence than a 30% length advantage.
            if(!central.empty()&&!lengths.empty()&&central[0]->tick.length>lengths[lengths.size()/2]*1.1&&
               (central.size()==1||central[0]->tick.length>central[1]->tick.length*1.2)) {
                int before=0,after=0;
                for(const auto& c:candidates)if(c.offset<central[0]->offset-1)++before;else if(c.offset>central[0]->offset+1)++after;
                if(before>=3&&after>=3&&std::abs(before-after)<=2) {
                    section.middleOffset=central[0]->offset;section.middleRatio=middleRatio;
                }
            }
        }
        // Resolve the hierarchy of printed long strokes before numbering short ones.
        if(!section.liquidMasked && candidates.size()>=5) {
            std::vector<double> lengths;
            for(const auto& c:candidates)lengths.push_back(c.tick.length);
            std::sort(lengths.begin(),lengths.end());
            std::vector<int> longer;
            for(int i=0;i<int(candidates.size());++i)
                if(candidates[i].confidence>=.6 && candidates[i].tick.length>lengths[lengths.size()/2]*1.35)
                    longer.push_back(i);
            std::sort(longer.begin(),longer.end(),[&](int a,int b){return candidates[a].offset<candidates[b].offset;});
            const int parts=int(longer.size())+1;
            bool regular=parts>=2;
            for(int i=0;i<int(longer.size());++i)
                if(std::abs(candidates[longer[i]].offset/sweep*parts-(i+1))>.30)regular=false;
            // On the 1.6 dial a short stroke touching the needle can look
            // long. Resolve one extra candidate using the observed 4/5/5/5
            // subdivision sequence between 0 and the printed 0.4 anchor.
            if(!regular && longer.size()==4 && minimum==0 && std::abs(maximum-1.6)<1e-6 &&
               section.leftRatio==0 && std::abs(section.rightRatio*maximum-.4)<1e-6) {
                std::vector<int> unique;
                for(int extra:longer) {
                    std::vector<int> trial;
                    std::vector<double> edges{0};
                    for(int i:longer)if(i!=extra){trial.push_back(i);edges.push_back(candidates[i].offset);}
                    edges.push_back(sweep);
                    bool fits=true;
                    for(int k=0;k<4&&fits;++k) {
                        const int cells=k==0?4:5;
                        std::set<int> seen;
                        for(const auto& c:candidates) {
                            if(c.confidence<.6||c.offset<=edges[k]||c.offset>=edges[k+1])continue;
                            const double position=(c.offset-edges[k])*cells/(edges[k+1]-edges[k]);
                            const int n=cvRound(position);
                            if(n<=0||n>=cells||std::abs(position-n)>.35||!seen.insert(n).second){fits=false;break;}
                        }
                        if(int(seen.size())<cells-2)fits=false;
                    }
                    if(fits){if(!unique.empty()){unique.clear();break;}unique=trial;}
                }
                if(!unique.empty()){longer=unique;regular=true;}
            }
            if(regular)for(int i:longer)section.longMarks.push_back(section.marks[i]);
        }
        intervals.push_back(section);
        // Adjacent observed strokes also constrain the pitch. Use the median
        // so an isolated missed stroke or a split edge cannot set the spacing.
        std::vector<double> offsets;
        for(const auto& c:candidates)if(c.confidence>=.6)offsets.push_back(c.offset);
        std::sort(offsets.begin(),offsets.end());
        if(diagnostics) {
            QJsonArray observations;for(const auto& c:candidates)observations.append(QJsonObject{{"offset",c.offset},{"length",c.tick.length},{"confidence",c.confidence}});
            diagnostics->append(QJsonObject{{"type","interval"},{"id",id},{"sweep",sweep},{"middle",section.middleOffset},{"observations",observations}});
        }
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
            Model model{id,n,0,0,phase,step};
            // The measured half-major and both endpoints constrain two local
            // spacings. Perspective need not preserve one angular step over
            // the entire maximum-side arc; never synthesize missing strokes.
            if(section.middleOffset>=0&&!section.minimumSide) {
                model.split=n*(section.middleRatio-section.leftRatio)/(section.rightRatio-section.leftRatio);
                model.outerStep=(sweep-section.middleOffset)/(n-model.split);
                if(model.outerStep<1.8)continue;
            }
            std::set<int> occupied;int aligned=0;double error=0;
            for(const auto& c:candidates){const double index=model.index(c.offset);const int k=cvRound(index);const double e=std::abs(index-k);
                // Residual perspective/refraction can shift a measured stroke
                // by 0.3 of a cell; keep it well below the half-cell ambiguity.
                if(k>0&&k<n&&e<.30){occupied.insert(k);++aligned;error+=e;}}
            int visibleCount=0;for(int k=1;k<n;++k){const double x=model.offset(k);
                if(x>1.4&&x<sweep-1.4)visibleCount+=visible[std::clamp(cvRound(x/sweep*bins),0,bins)];}
            if(occupied.size()<3||visibleCount<3)continue;
            double score=std::min(1.,double(occupied.size())/visibleCount)*double(aligned)/std::max(size_t(1),candidates.size())*(1-error/std::max(1,aligned));
            if(offsets.size()>=5) {
                std::vector<double> pitchErrors;
                for(size_t i=1;i<offsets.size();++i) {
                    const double cells=model.index(offsets[i])-model.index(offsets[i-1]);
                    // Gaps spanning multiple cells do not measure adjacent pitch.
                    if(cells<1.5)pitchErrors.push_back(std::abs(cells-1));
                }
                if(pitchErrors.size()>=4) {
                    std::sort(pitchErrors.begin(),pitchErrors.end());
                    score*=1-pitchErrors[pitchErrors.size()/2];
                }
            }
            const double division=(maximum-minimum)*(section.rightRatio-section.leftRatio)/n;
            if(!manualDivision&&!decimalDivision(division))continue;
            if(manualDivision&&std::abs(division-confirmedDivision)>confirmedDivision*.001)continue;
            if(score>=(manualDivision?.35:.55)){model.division=division;model.score=score;models.push_back(model);}
        }
    }
    Result longPreview;
    std::vector<double> measuredSteps;
    for(const auto& section:intervals) {
        if(section.longMarks.empty())continue;
        const double width=(section.rightRatio-section.leftRatio)/(section.longMarks.size()+1);
        std::vector<double> bounds{0},shortOffsets;
        for(int i:section.marks) {
            auto m=out.marks[i];
            const auto d=(m.a+m.b)*.5f-center;
            const double angle=ScaleCalibration::span(section.start,std::atan2(d.y,d.x)*180/CV_PI,clockwise);
            const auto it=std::find(section.longMarks.begin(),section.longMarks.end(),i);
            if(it!=section.longMarks.end()) {
                m.assigned=true;m.ratio=section.leftRatio+width*(1+std::distance(section.longMarks.begin(),it));
                longPreview.marks.push_back(m);bounds.push_back(angle);
            } else if(m.confidence>=.6)shortOffsets.push_back(angle);
        }
        bounds.push_back(section.sweep);std::sort(bounds.begin(),bounds.end());std::sort(shortOffsets.begin(),shortOffsets.end());
        for(size_t k=1;k<bounds.size();++k) {
            std::vector<double> chain{bounds[k-1]};
            for(double x:shortOffsets)if(x>bounds[k-1]&&x<bounds[k])chain.push_back(x);
            chain.push_back(bounds[k]);
            const int count=int(chain.size())-1;if(count<3)continue;
            const double pitch=(chain.back()-chain.front())/count;
            bool regular=true;
            for(size_t j=1;j<chain.size();++j)if(std::abs((chain[j]-chain[j-1])/pitch-1)>.30)regular=false;
            if(regular)measuredSteps.push_back(width*(maximum-minimum)/count);
        }
    }
    if(showLong)showLong(longPreview);
    double verifiedStep=0;
    for(double step:measuredSteps) {
        if(!decimalDivision(step))continue;
        const auto votes=std::count_if(measuredSteps.begin(),measuredSteps.end(),[&](double x){return std::abs(x-step)<step*.001;});
        if(votes<2 || anchors.size()<3)continue;
        if(verifiedStep && std::abs(verifiedStep-step)>step*.001){verifiedStep=0;break;}
        verifiedStep=step;
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
    if(diagnostics) {
        for(const auto& m:models)diagnostics->append(QJsonObject{{"type","model"},{"id",m.interval},{"n",m.n},{"division",m.division},{"score",m.score},{"step",m.step},{"phase",m.phase}});
        for(const auto& g:groups)diagnostics->append(QJsonObject{{"type","consensus"},{"division",g.division},{"score",g.score},{"support",int(g.support.size())}});
    }
    if(groups.empty()&&verifiedStep&&!manualDivision)groups.push_back({verifiedStep,0,{}});
    if(!groups.empty()) {
        const auto& best=groups.front();out.division=best.division;out.intervals=int(best.support.size());
        out.confidence=best.support.empty()?0.:best.score/best.support.size();
        const double margin=groups.size()>1?best.score-groups[1].score:best.score;
        int unambiguous=0;
        for(const auto& selected:best.support) {
            double alternative=0;
            for(const auto& m:models)if(m.interval==selected.interval&&m.n!=selected.n)alternative=std::max(alternative,m.score);
            if(selected.score-alternative>=.10)++unambiguous;
        }
        out.divisionConfirmed=manualDivision||(unambiguous>=2&&margin>=.20);
        if(!out.divisionConfirmed && verifiedStep && !manualDivision) {
            if(std::abs(verifiedStep-out.division)>verifiedStep*.001){groups.front().support.clear();out.intervals=0;}
            out.division=verifiedStep;out.divisionConfirmed=true;out.confidence=.8;
        }
        if(!out.divisionConfirmed)out.confidence*=.5;
        out.previewPoints=anchors;
        if(out.divisionConfirmed)for(const auto& section:intervals) {
            if(section.longMarks.empty())continue;
            const int parts=int(section.longMarks.size())+1;
            const double valueSpan=(section.rightRatio-section.leftRatio)*(maximum-minimum)/parts;
            const double steps=valueSpan/out.division;
            if(steps<2 || steps>80 || std::abs(steps-std::round(steps))>.01)continue;
            auto offsetOf=[&](int i){
                const auto d=(out.marks[i].a+out.marks[i].b)*.5f-center;
                return ScaleCalibration::span(section.start,std::atan2(d.y,d.x)*180/CV_PI,clockwise);
            };
            auto adopt=[&](int i,double ratio){
                auto& m=out.marks[i];m.ratio=ratio;m.assigned=true;
                const auto p=(m.a+m.b)*.5f;
                out.previewPoints.append(QJsonObject{{"x",p.x},{"y",p.y},{"ratio",ratio},{"origin_x",center.x},{"origin_y",center.y},{"source","minor_tick"}});
            };
            const double width=(section.rightRatio-section.leftRatio)/parts;
            for(int k=1;k<parts;++k)adopt(section.longMarks[k-1],section.leftRatio+k*width);
            for(int k=0;k<parts;++k) {
                const double lo=k?offsetOf(section.longMarks[k-1]):0;
                const double hi=k+1==parts?section.sweep:offsetOf(section.longMarks[k]);
                int count=cvRound(steps);
                // The four-part 0..0.4 layout has four *short* divisions before its first long mark.
                if(k==0 && parts==4 && section.leftRatio==0 && minimum==0 && std::abs(maximum-1.6)<1e-6 &&
                   std::abs(section.rightRatio*maximum-.4)<1e-6 && count==5)count=4;
                std::vector<std::vector<int>> matches(count);
                for(int i:section.marks) {
                    if(out.marks[i].assigned || out.marks[i].confidence<.6)continue;
                    const double slot=(offsetOf(i)-lo)*count/(hi-lo);
                    const int n=cvRound(slot);
                    if(n>0 && n<count && std::abs(slot-n)<.35)matches[n].push_back(i);
                }
                for(int n=1;n<count;++n)if(matches[n].size()==1)
                    adopt(matches[n][0],section.leftRatio+width*(k+double(n)/count));
                // The zero stop can be displaced relative to the printed lattice.
                // Verify the visible run against the measured long stroke instead
                // of stretching its pitch to that stop. Never invent a missing line.
                if(k==0 && section.leftRatio==0 && count>=4) {
                    std::vector<std::pair<double,int>> run;
                    for(int i:section.marks)if(out.marks[i].confidence>=.6) {
                        const double x=offsetOf(i);
                        if(x>lo && x<hi-.01)run.push_back({hi-x,i});
                    }
                    std::sort(run.begin(),run.end());
                    if(run.size()>=3 && int(run.size())<count) {
                        std::vector<double> gaps{run.front().first};
                        for(size_t j=1;j<run.size();++j)gaps.push_back(run[j].first-run[j-1].first);
                        auto sorted=gaps;std::sort(sorted.begin(),sorted.end());
                        const double pitch=sorted[sorted.size()/2];
                        bool fits=pitch>1.4;
                        for(double gap:gaps)fits &= std::abs(gap/pitch-1)<.25;
                        for(size_t j=0;j<run.size();++j) {
                            const auto& m=out.marks[run[j].second];
                            const double ratio=section.leftRatio+width*(1-double(j+1)/count);
                            if(m.assigned && std::abs(m.ratio-ratio)>1e-6)fits=false;
                        }
                        if(fits)for(size_t j=0;j<run.size();++j)
                            if(!out.marks[run[j].second].assigned)
                                adopt(run[j].second,section.leftRatio+width*(1-double(j+1)/count));
                    }
                }
            }
        }
        for(const auto& model:best.support) {
            const auto& section=intervals[model.interval];const double step=model.step;
            if(!section.longMarks.empty()&&out.marks[section.longMarks.front()].assigned)continue;
            std::vector<std::vector<int>> slotsByIndex(model.n+1);
            for(int index:section.marks) {
                const auto& m=out.marks[index];const auto mid=(m.a+m.b)*.5f;
                const double a=AutoScaleCalibration::wrap(std::atan2(mid.y-center.y,mid.x-center.x)*180/CV_PI);
                const double offset=ScaleCalibration::span(section.start,a,clockwise);const double slot=model.index(offset);const int k=cvRound(slot);
                if(k>0&&k<model.n&&std::abs(slot-k)<(manualDivision?.35:.30)&&m.confidence>=.60)slotsByIndex[k].push_back(index);
            }
            for(int k=1;k<model.n;++k) {
                const double ratio=section.leftRatio+(section.rightRatio-section.leftRatio)*k/model.n;
                if(slotsByIndex[k].size()==1) {
                    auto& m=out.marks[slotsByIndex[k].front()];m.ratio=ratio;m.assigned=out.divisionConfirmed;
                    m.confidence=std::min(m.confidence,model.score);
                    if(m.assigned) {const auto p=(m.a+m.b)*.5f;out.previewPoints.append(QJsonObject{{"x",p.x},{"y",p.y},{"ratio",ratio},{"origin_x",center.x},{"origin_y",center.y},{"source","minor_tick"}});}
                } else if(slotsByIndex[k].empty()) {
                    const double expectedOffset=model.offset(k);
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
    // Try to close the observed chain against both anchors before falling
    // back to short, continuous runs beside either visible endpoint.
    const auto measuredPoints=out.previewPoints;
    std::vector<bool> measuredAssignments;for(const auto& m:out.marks)measuredAssignments.push_back(m.assigned);
    // Local numbering also improves provisional labels when global consensus
    // fails. Provisional assignments are discarded before returning below.
    if(out.division>0)for(const auto& section:intervals) {
        if(!section.longMarks.empty()&&measuredAssignments[section.longMarks.front()])continue;
        const double valueStep=out.division/(maximum-minimum);
        // A displaced end stop can invalidate the whole first interval even
        // when every graduation from its long middle stroke to the interior
        // major is visible. Verify that half independently by counting ink.
        if(section.middleOffset>=0) {
            const double lo=section.minimumSide?section.middleOffset:0;
            const double hi=section.minimumSide?section.sweep:section.middleOffset;
            const double base=section.minimumSide?section.middleRatio:section.leftRatio;
            const double count=(section.rightRatio-section.leftRatio)*.5/valueStep;
            const int steps=cvRound(count);
            if(steps>=3&&steps<=80&&std::abs(steps-count)<.01) {
                const double pitch=(hi-lo)/steps;
                std::vector<int> measured(steps+1,-1);
                bool agrees=true;
                for(int i:section.marks) {
                    const auto& m=out.marks[i];if(m.confidence<.6&&!m.assigned)continue;
                    const auto d=(m.a+m.b)*.5f-center;
                    const double offset=ScaleCalibration::span(section.start,std::atan2(d.y,d.x)*180/CV_PI,clockwise);
                    if(offset<lo-.01||offset>hi+.01)continue;
                    const double position=(offset-lo)/pitch;
                    const int k=cvRound(position);
                    if(k<0||k>steps||std::abs(position-k)>.35||measured[k]>=0||
                       (m.assigned&&std::abs(m.ratio-base-k*valueStep)>valueStep*.01)){agrees=false;break;}
                    measured[k]=i;
                }
                const int first=section.minimumSide?0:1,last=section.minimumSide?steps-1:steps;
                for(int k=first;k<=last;++k)if(measured[k]<0)agrees=false;
                if(agrees)for(int k=first;k<=last;++k) {
                    auto& m=out.marks[measured[k]];if(m.assigned)continue;
                    m.assigned=true;m.ratio=base+k*valueStep;m.confidence=std::min(m.confidence,out.confidence);
                    const auto p=(m.a+m.b)*.5f;
                    out.previewPoints.append(QJsonObject{{"x",p.x},{"y",p.y},{"ratio",m.ratio},{"origin_x",center.x},{"origin_y",center.y},{"source","minor_tick"}});
                }
            }
        }
        for(bool fromLeft:{true,false}) {
            struct Nearby {int index;double offset;};std::vector<Nearby> nearby;
            for(int index:section.marks) {
                const auto& mark=out.marks[index];
                // Estimate pitch and count only strokes eligible for assignment.
                // A weak edge beside the anchor must not become the first step.
                if(mark.confidence<.6)continue;
                const auto p=(mark.a+mark.b)*.5f-center;
                const double angle=AutoScaleCalibration::wrap(std::atan2(p.y,p.x)*180/CV_PI);
                const double offset=ScaleCalibration::span(section.start,angle,clockwise);
                nearby.push_back({index,fromLeft?offset:section.sweep-offset});
            }
            std::sort(nearby.begin(),nearby.end(),[](const Nearby& a,const Nearby& b){return a.offset<b.offset;});
            if(nearby.size()<3)continue;
            if(fromLeft) {
                const double divisions=(section.rightRatio-section.leftRatio)/valueStep;
                std::vector<double> gaps;
                double last=0;
                for(const auto& candidate:nearby) {
                    gaps.push_back(candidate.offset-last);
                    last=candidate.offset;
                }
                gaps.push_back(section.sweep-last);
                int slot=0;
                std::vector<int> indices;
                for(int i=0;i<int(gaps.size());++i) {
                    std::vector<double> neighbors;
                    for(int j=std::max(0,i-3);j<=std::min(int(gaps.size())-1,i+3);++j)
                        if(j!=i)neighbors.push_back(gaps[j]);
                    std::sort(neighbors.begin(),neighbors.end());
                    const double pitch=neighbors[(neighbors.size()-1)/2];
                    if(pitch<1.4)break;
                    const double steps=gaps[i]/pitch;
                    const int count=cvRound(steps);
                    if(count<1||count>3||std::abs(steps-count)>.25)break;
                    slot+=count;
                    if(i<int(nearby.size())) {
                        const auto& mark=out.marks[nearby[i].index];
                        if(mark.assigned&&std::abs(mark.ratio-section.leftRatio-slot*valueStep)>valueStep*.01)break;
                    }
                    indices.push_back(slot);
                }
                // Counting backward from the right anchor yields the same
                // indices only if every gap is resolved and the total closes.
                if(indices.size()==gaps.size()&&std::abs(slot-divisions)<.01) {
                    for(size_t i=0;i<nearby.size();++i) {
                        auto& mark=out.marks[nearby[i].index];
                        if(mark.assigned)continue;
                        mark.ratio=section.leftRatio+indices[i]*valueStep;
                        mark.assigned=true;
                        mark.confidence=std::min(mark.confidence,out.confidence);
                        const auto p=(mark.a+mark.b)*.5f;
                        out.previewPoints.append(QJsonObject{{"x",p.x},{"y",p.y},{"ratio",mark.ratio},{"origin_x",center.x},{"origin_y",center.y},{"source","minor_tick"}});
                    }
                    break;
                }
            }
            std::vector<double> spacings;
            for(size_t i=1;i<std::min(size_t(5),nearby.size());++i) {
                const double d=nearby[i].offset-nearby[i-1].offset;
                if(d>1.4)spacings.push_back(d);
            }
            if(spacings.size()<2)continue;
            std::sort(spacings.begin(),spacings.end());const double spacing=spacings[spacings.size()/2];
            double previous=0;int count=0;
            std::vector<int> chain;
            for(const auto& candidate:nearby) {
                const double delta=candidate.offset-previous;
                if(candidate.offset>section.sweep*.5||delta<spacing*.5||delta>spacing*1.5)break;
                ++count;previous=candidate.offset;
                auto& mark=out.marks[candidate.index];
                if(!mark.assigned)mark.ratio=std::clamp((fromLeft?section.leftRatio:section.rightRatio)+(fromLeft?1:-1)*count*valueStep,0.,1.);
                chain.push_back(candidate.index);
            }
            // A confirmed global division plus a continuous, locally regular
            // chain supplies evidence even when the opposite anchor is masked.
            bool regular=chain.size()>=3;previous=0;
            for(size_t i=0;i<chain.size();++i) {
                const double delta=nearby[i].offset-previous;previous=nearby[i].offset;
                regular&=std::abs(delta/spacing-1)<.25;
            }
            if(!regular) {
                // An irregular stroke near the needle must not invalidate the
                // measured regular prefix beside an anchor. Establish pitch
                // from three consecutive observed gaps (including the anchor),
                // then accept only measured strokes close to that local lattice.
                // One unresolved slot may be skipped; it never creates a point.
                std::vector<double> initial{nearby[0].offset,nearby[1].offset-nearby[0].offset,
                                            nearby[2].offset-nearby[1].offset};
                auto sorted=initial;std::sort(sorted.begin(),sorted.end());
                const double pitch=sorted[1];
                const bool prefix=pitch>1.4&&std::all_of(initial.begin(),initial.end(),[&](double gap){return std::abs(gap/pitch-1)<.25;});
                chain.clear();int lastSlot=0;
                if(prefix)for(int index:section.marks) {
                    auto& mark=out.marks[index];
                    if(mark.assigned||mark.confidence>=.6)continue;
                    const auto p=(mark.a+mark.b)*.5f-center;
                    const double offset=ScaleCalibration::span(section.start,std::atan2(p.y,p.x)*180/CV_PI,clockwise);
                    const double distance=fromLeft?offset:section.sweep-offset;
                    const double position=distance/pitch;
                    if(distance<=section.sweep*.5&&position>0&&position<=5.25)
                        mark.ratio=std::clamp((fromLeft?section.leftRatio:section.rightRatio)+(fromLeft?1:-1)*position*valueStep,0.,1.);
                }
                if(prefix)for(const auto& candidate:nearby) {
                    if(candidate.offset>section.sweep*.5)break;
                    const double position=candidate.offset/pitch;const int slot=cvRound(position);
                    if(slot>lastSlot+2)break;
                    if(slot<=lastSlot||std::abs(position-slot)>=.25)continue;
                    auto& mark=out.marks[candidate.index];
                    const double ratio=(fromLeft?section.leftRatio:section.rightRatio)+(fromLeft?1:-1)*slot*valueStep;
                    if(ratio<=section.leftRatio||ratio>=section.rightRatio)break;
                    if(mark.assigned&&std::abs(mark.ratio-ratio)>valueStep*.01)break;
                    if(!mark.assigned)mark.ratio=ratio;
                    chain.push_back(candidate.index);lastSlot=slot;
                }
                regular=chain.size()>=3;
            }
            if(regular)for(int index:chain) {
                auto& mark=out.marks[index];if(mark.assigned)continue;
                const auto p=(mark.a+mark.b)*.5f;
                mark.assigned=true;mark.confidence=std::min(mark.confidence,out.confidence);
                out.previewPoints.append(QJsonObject{{"x",p.x},{"y",p.y},{"ratio",mark.ratio},{"origin_x",center.x},{"origin_y",center.y},{"source","minor_tick"}});
            }
        }
    }
    if(out.divisionConfirmed) {
        const auto before=out.previewPoints;
        std::vector<QJsonObject> ordered;for(const auto& p:before)ordered.push_back(p.toObject());
        std::sort(ordered.begin(),ordered.end(),[](const QJsonObject& a,const QJsonObject& b){return a["ratio"].toDouble()<b["ratio"].toDouble();});
        out.previewPoints={};for(const auto& p:ordered)out.previewPoints.append(p);
        ScaleCalibration::Mapping checked;
        if(!ScaleCalibration::valid(out.previewPoints,image.cols,image.rows,hub)||!ScaleCalibration::build(out.previewPoints,hub,{},clockwise,checked)) {
            // Preserve the measured anchors; never persist a conflicting chain.
            out.previewPoints=measuredPoints;
            for(size_t i=0;i<out.marks.size();++i)out.marks[i].assigned=measuredAssignments[i];
        }
    }
    // Keep candidate values for the preview; only confirmed assignments may be saved.
    if(!out.divisionConfirmed) {
        out.previewPoints=measuredPoints;
        for(auto& m:out.marks)m.assigned=false;
    }
    for(const auto& section:intervals) {
        const double step=(section.rightRatio-section.leftRatio)/(section.longMarks.size()+1);
        for(size_t k=0;k<section.longMarks.size();++k) {
            auto& mark=out.marks[section.longMarks[k]];
            const double counted=section.leftRatio+(k+1)*step;
            if(mark.assigned&&std::abs(mark.ratio-counted)>1e-6) {
                // A competing short-tick numbering cannot replace a counted long value.
                const auto middle=(mark.a+mark.b)*.5f;
                for(int j=out.previewPoints.size()-1;j>=0;--j) {
                    const auto p=out.previewPoints[j].toObject();
                    if(p["source"]=="minor_tick"&&cv::norm(middle-cv::Point2f(p["x"].toDouble(),p["y"].toDouble()))<1)
                        out.previewPoints.removeAt(j);
                }
                mark.assigned=false;
            }
            mark.ratio=counted;
        }
    }
    const int gaps=int(std::count_if(out.marks.begin(),out.marks.end(),[](const Mark& m){return m.gap;}));
    out.message=QStringLiteral(u"小刻度预览：%1 条候选（紫色），%2 处推断缺口（橙色虚线）；候选分度值 %3；规则置信度 %4%。%5")
        .arg(int(out.marks.size())-gaps).arg(gaps).arg(out.division>0?displayValue(out.division):QStringLiteral(u"待确认"))
        .arg(qRound(out.confidence*100)).arg(!out.divisionConfirmed?QStringLiteral(u"分度尚未通过多个区间验证，显示的估计数值仅供预览，不保存为标定点。 "):(manualDivision?QStringLiteral(u"使用人工指定分度。 "):QStringLiteral(u"分度已通过多个区间验证。 ")));
    if(!intervals.empty()&&intervals.front().middleOffset>=0)
        out.message+=QStringLiteral(u" 自动推断起始中刻度 %1（由相邻大刻度数值确定），按内侧间距向最小端反推。").arg(minimum+intervals.front().middleRatio*(maximum-minimum));
    const int assigned=int(std::count_if(out.marks.begin(),out.marks.end(),[](const Mark& m){return m.assigned;}));
    out.message+=QStringLiteral(u" 可靠赋值 %1 条（青蓝色）；未补齐的缺口不生成标定点。").arg(assigned);
    for(const auto& section:intervals) {
        std::vector<double> ratios{section.leftRatio,section.rightRatio};
        for(int index:section.marks)if(out.marks[index].assigned)ratios.push_back(out.marks[index].ratio);
        std::sort(ratios.begin(),ratios.end());
        double largest=0;
        const bool special=minimum==0 && std::abs(maximum-1.6)<1e-6 && section.leftRatio==0 && section.longMarks.size()==3 && out.marks[section.longMarks.front()].assigned && std::abs(section.rightRatio*maximum-.4)<1e-6 && std::abs(out.division-.02)<1e-6;
        for(size_t i=1;i<ratios.size();++i){
            double delta=(ratios[i]-ratios[i-1])*(maximum-minimum);
            if(special) {
                const double firstLong=out.marks[section.longMarks.front()].ratio*maximum;
                delta-=.2*std::max(0.,std::min(firstLong,ratios[i]*maximum)-ratios[i-1]*maximum);
            }
            largest=std::max(largest,delta);
        }
        QString line=QStringLiteral(u"%1～%2：候选 %3，确认 %4")
            .arg(displayValue(minimum+section.leftRatio*(maximum-minimum)),displayValue(minimum+section.rightRatio*(maximum-minimum)))
            .arg(int(section.marks.size())).arg(int(ratios.size())-2);
        if(out.divisionConfirmed)line+=QStringLiteral(u"，最大插值跨度 %1 格").arg(displayValue(largest/out.division));
        if(section.liquidMasked)line+=QStringLiteral(u"（含液面屏蔽区）");
        out.coverage.append(line);
    }
    if(!out.coverage.isEmpty())out.message+="\n"+out.coverage.join("\n");
    const int inferred=fillPointerGaps(out,center,radius,minimum,maximum,clockwise,surface,pointerMask);
    if(inferred)out.message+=QStringLiteral(u"\n指针遮挡处推算 %1 条（橙色虚线，仅用于预览）。").arg(inferred);
    return out;
}
}
