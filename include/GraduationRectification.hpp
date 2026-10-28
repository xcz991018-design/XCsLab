#pragma once
#include <opencv2/opencv.hpp>
#include <vector>
#include <algorithm>
#include <cmath>

// Printed-ring projective rectification, independently checked against radial
// strokes, bilateral symmetry and the measured 270-degree active arc.
namespace GraduationRectification {
struct Observation {cv::Point2f outer,inner;double angle,length;};
struct Result {
    cv::Mat correction;
    int support{0},symmetricPairs{0};
    double before{0},after{0},endpointError{0};
    bool valid() const {return !correction.empty();}
};
inline double angle(cv::Point2f p) {double a=std::atan2(p.y,p.x)*180/CV_PI;return a<90?a+360:a;}
inline double median(std::vector<double> v) {std::sort(v.begin(),v.end());return v[v.size()/2];}
inline std::vector<Observation> observations(const std::vector<cv::Vec4f>& lines,const cv::Mat& H) {
    std::vector<Observation> all;
    for(const auto& line:lines) {
        std::vector<cv::Point2f> p{{line[0],line[1]},{line[2],line[3]}};cv::perspectiveTransform(p,p,H);
        const auto d=p[1]-p[0],m=(p[0]+p[1])*.5f;const double r=cv::norm(m),length=cv::norm(d);
        if(!std::isfinite(r)||r<.58||r>.94||length<.018||length>.24||
           std::abs(d.x*m.y-d.y*m.x)>length*r*.34)continue;
        if(cv::norm(p[0])<cv::norm(p[1]))std::swap(p[0],p[1]);
        const double a=angle(m);if(a<113||a>427)continue;
        all.push_back({p[0],p[1],a,length});
    }
    std::sort(all.begin(),all.end(),[](const Observation& a,const Observation& b){return a.angle<b.angle;});
    std::vector<Observation> groups;
    for(const auto& o:all) {
        if(!groups.empty()&&o.angle-groups.back().angle<1.7) {
            if(o.length>groups.back().length)groups.back()=o;
        } else groups.push_back(o);
    }
    if(groups.empty())return {};
    std::vector<double> radii;for(const auto& o:groups)radii.push_back(cv::norm(o.outer));
    const double ring=median(radii);
    groups.erase(std::remove_if(groups.begin(),groups.end(),[&](const Observation& o){return std::abs(cv::norm(o.outer)-ring)>.09;}),groups.end());
    return groups;
}
inline double normalizedRadius(const cv::RotatedRect& e,cv::Point2f p) {
    const double a=e.angle*CV_PI/180,dx=p.x-e.center.x,dy=p.y-e.center.y;
    return std::hypot((dx*std::cos(a)+dy*std::sin(a))/(e.size.width*.5),
                      (-dx*std::sin(a)+dy*std::cos(a))/(e.size.height*.5));
}
inline double symmetryError(const std::vector<cv::Point2f>& points,int& pairs) {
    std::vector<double> errors;pairs=0;
    for(auto p:points)if(p.x<-.25) {
        double nearest=1e9;
        for(auto q:points)if(q.x>.25)nearest=std::min(nearest,cv::norm(cv::Point2f(-p.x,p.y)-q));
        if(nearest<.065){errors.push_back(nearest);++pairs;}
    }
    return errors.size()>=6?median(errors):1e9;
}
inline bool radialCenter(const std::vector<Observation>& strokes,cv::Point2f& center) {
    // The printed radial lines belong to the dial plane; a raised hub cap does
    // not. Fit their common intersection without moving the measured needle hub.
    cv::Point2d current(0,0);
    for(int iteration=0;iteration<8;++iteration) {
        cv::Matx22d A(0,0,0,0);cv::Vec2d b(0,0);
        for(const auto& o:strokes) {
            const auto d=o.outer-o.inner;const double length=cv::norm(d);
            const cv::Vec2d n(-d.y/length,d.x/length);
            const double target=n.dot(cv::Vec2d(o.outer.x,o.outer.y));
            const double error=std::abs(n.dot(cv::Vec2d(current.x,current.y))-target);
            const double weight=std::min(1.,.012/std::max(error,1e-9));
            A+=cv::Matx22d(n[0]*n[0],n[0]*n[1],n[0]*n[1],n[1]*n[1])*weight;
            b+=n*(target*weight);
        }
        if(cv::determinant(A)<1e-5)return false;
        const auto solved=A.inv()*b;current={solved[0],solved[1]};
    }
    if(cv::norm(current)>.12)return false;
    std::vector<double> errors;
    for(const auto& o:strokes) {
        const auto d=o.outer-o.inner,p=o.outer-cv::Point2f(float(current.x),float(current.y));
        errors.push_back(std::abs(d.x*p.y-d.y*p.x)/cv::norm(d));
    }
    if(median(errors)>.018)return false;
    center={float(current.x),float(current.y)};return true;
}
template<class Metric>
inline Result fit(const std::vector<cv::Vec4f>& reliableLines,const cv::Mat& initialH,Metric metric) {
    Result best;const auto strokes=observations(reliableLines,initialH);if(strokes.size()<18)return best;
    auto endpoint=[&](double expected) {
        int found=-1;double cost=1e9;
        for(int i=0;i<int(strokes.size());++i) {
            const auto& o=strokes[i];if(std::abs(o.angle-expected)>20)continue;
            bool continued=false;
            for(const auto& other:strokes) {
                const double outside=expected<270?o.angle-other.angle:other.angle-o.angle;
                if(outside>2.5&&outside<7)continued=true;
            }
            if(continued)continue; // An interior tick is not an arc endpoint.
            double longest=o.length;for(const auto& other:strokes)if(std::abs(o.angle-other.angle)<18)longest=std::max(longest,other.length);
            if(o.length<longest*.70)continue;
            const double c=std::abs(o.angle-expected)-o.length*25;
            if(c<cost){cost=c;found=i;}
        }return found;
    };
    const int first=endpoint(135),last=endpoint(405);if(first<0||last<0||first==last)return best;
    const double originalSpan=strokes[last].angle-strokes[first].angle;
    if(originalSpan<245||originalSpan>295)return best;
    std::vector<cv::Point2f> points;std::vector<double> radii;
    for(const auto& o:strokes){points.push_back(o.outer);radii.push_back(cv::norm(o.outer));}
    const double radius=median(radii);
    std::vector<cv::RotatedRect> hypotheses;
    // Alternate strokes are reserved for validation, and never used by these
    // robust hypotheses. Endpoints are checked separately rather than fabricated.
    std::vector<cv::Point2f> training;for(size_t i=0;i<points.size();i+=2)training.push_back(points[i]);
    if(training.size()<9)return best;
    std::vector<Observation> trainingStrokes;for(size_t i=0;i<strokes.size();i+=2)trainingStrokes.push_back(strokes[i]);
    cv::Point2f planeCenter;if(!radialCenter(trainingStrokes,planeCenter))return best;
    hypotheses.push_back(cv::fitEllipse(training));cv::RNG rng(0x47524144);
    for(int trial=0;trial<72;++trial) {
        std::vector<cv::Point2f> sample;
        for(int bin=0;bin<8;++bin) {
            const int begin=int(training.size()*bin/8),end=int(training.size()*(bin+1)/8);
            sample.push_back(training[rng.uniform(begin,end)]);
        }
        hypotheses.push_back(cv::fitEllipse(sample));
    }
    double bestCost=1e9;
    for(size_t h=0;h<hypotheses.size();++h) {
        const auto& ellipse=hypotheses[h];
        if(!std::isfinite(ellipse.angle)||!std::isfinite(ellipse.center.x)||!std::isfinite(ellipse.center.y)||
           ellipse.size.width<1.1||ellipse.size.height<1.1||ellipse.size.width>2.1||ellipse.size.height>2.1||cv::norm(ellipse.center)>.18)continue;
        if(std::abs(normalizedRadius(ellipse,strokes[first].outer)-1)>.04||
           std::abs(normalizedRadius(ellipse,strokes[last].outer)-1)>.04)continue;
        auto C=metric(ellipse,planeCenter);if(C.empty())continue;
        const cv::Mat resize=(cv::Mat_<double>(3,3)<<radius,0,0,0,radius,0,0,0,1);C=resize*C;
        std::vector<cv::Point2f> ends;cv::perspectiveTransform(std::vector<cv::Point2f>{strokes[first].outer,strokes[last].outer},ends,C);
        const double a=angle(ends[0]),b=angle(ends[1]),span=b-a;
        if(std::abs(span-270)>4)continue;
        const double rotation=270-(a+b)*.5;if(std::abs(rotation)>10)continue;
        const double r=rotation*CV_PI/180;
        C=(cv::Mat_<double>(3,3)<<std::cos(r),-std::sin(r),0,std::sin(r),std::cos(r),0,0,0,1)*C;
        if(!cv::checkRange(C)||std::hypot(C.at<double>(2,0),C.at<double>(2,1))>.25)continue;
        cv::Mat singular;cv::SVD::compute(C(cv::Rect(0,0,2,2)),singular);
        if(singular.at<double>(0)>1.3||singular.at<double>(1)<.75)continue;
        std::vector<cv::Point2f> mapped;cv::perspectiveTransform(points,mapped,C);
        std::vector<cv::Point2f> inner;for(const auto& o:strokes)inner.push_back(o.inner);cv::perspectiveTransform(inner,inner,C);
        std::vector<double> beforeErrors,afterErrors,radialErrors;int inliers=0;
        for(size_t i=0;i<points.size();++i) {
            const double e=std::abs(cv::norm(mapped[i])-radius);
            if(e<.025)++inliers;
            if(i%2){
                beforeErrors.push_back(std::abs(cv::norm(points[i])-radius));afterErrors.push_back(e);
                const auto d=mapped[i]-inner[i];radialErrors.push_back(std::abs(d.x*mapped[i].y-d.y*mapped[i].x)/cv::norm(d));
            }
        }
        const double before=median(beforeErrors),after=median(afterErrors);
        if(before<.006||inliers<int(points.size()*.8)||after>.014||after>before*.75||median(radialErrors)>.025)continue;
        int pairs=0;const double symmetric=symmetryError(mapped,pairs);
        if(pairs<6||symmetric>.035)continue;
        const double cost=after+.0008*std::abs(span-270)+symmetric*.15;
        if(cost<bestCost){bestCost=cost;best={C,inliers,pairs,before,after,std::abs(span-270)*.5};}
    }
    return best;
}
}

