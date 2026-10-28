#pragma once
#include <opencv2/opencv.hpp>
#include <QString>
#include <algorithm>
#include <cmath>
#include <array>
#include "LiquidSurface.hpp"
#include "GraduationRectification.hpp"

namespace AutoDialCalibration {
struct Result {
    std::vector<cv::Point2f> points;
    cv::Point2f hub{-1,-1};
    cv::RotatedRect rim;
    double contourResidual{0}, rimCoverage{0}, hubSupport{0};
    double graduationResidual{-1}, graduationAspect{0};
    int graduationSupport{0};
    double initialGraduationAspect{0};
    bool graduationLiquidExcluded{false},graduationLiquidAmbiguous{false};
    bool structuralRectification{false};
    cv::Point2f graduationCenter{-1,-1};
    int structuralSupport{0},structuralSymmetricPairs{0};
    double structuralErrorBefore{0},structuralErrorAfter{0};
    QString error;
    bool valid() const {return error.isEmpty() && points.size()==4;}
};
struct Shape {cv::RotatedRect ellipse;double residual{0},coverage{0};};
inline cv::Matx22d ellipseAxes(const cv::RotatedRect& ellipse) {
    const double a=ellipse.angle*CV_PI/180;
    return {std::cos(a),-std::sin(a),std::sin(a),std::cos(a)};
}
inline double percentile(std::vector<double> values,double quantile) {
    std::sort(values.begin(),values.end());return values[std::min(values.size()-1,size_t(quantile*(values.size()-1)))];
}
inline double normalizedRadius(const cv::RotatedRect& e,cv::Point2f p) {
    const auto axes=ellipseAxes(e);const cv::Vec2d delta(p.x-e.center.x,p.y-e.center.y);
    const auto local=axes.t()*delta;
    return std::hypot(local[0]/(e.size.width*.5),local[1]/(e.size.height*.5));
}
inline double edgeSupport(const cv::RotatedRect& e,const cv::Mat& gx,const cv::Mat& gy) {
    const auto axes=ellipseAxes(e);int positive=0,negative=0;
    for(int i=0;i<36;++i) {
        const double a=i*2*CV_PI/36;
        const auto offset=axes*cv::Vec2d(e.size.width*.5*std::cos(a),e.size.height*.5*std::sin(a));
        auto normal=axes*cv::Vec2d(std::cos(a)/(e.size.width*.5),std::sin(a)/(e.size.height*.5));normal*=1/cv::norm(normal);
        bool pos=false,neg=false;
        for(int d=-1;d<=1;++d) {
            const int x=cvRound(e.center.x+offset[0]+normal[0]*d),y=cvRound(e.center.y+offset[1]+normal[1]*d);
            if(x<0||y<0||x>=gx.cols||y>=gx.rows)continue;
            const double xg=gx.at<float>(y,x),yg=gy.at<float>(y,x),m=std::hypot(xg,yg),n=xg*normal[0]+yg*normal[1];
            pos|=m>60&&n>m*.65;neg|=m>60&&n<-m*.65;
        }
        positive+=pos;negative+=neg;
    }
    return double(std::max(positive,negative))/36;
}
inline double needleSpokes(const cv::Mat& gray,const cv::RotatedRect& hub,double radius) {
    const int r=int(radius*.35);std::vector<double> background;
    for(int y=std::max(0,int(hub.center.y)-r);y<std::min(gray.rows,int(hub.center.y)+r);y+=3)
        for(int x=std::max(0,int(hub.center.x)-r);x<std::min(gray.cols,int(hub.center.x)+r);x+=3)background.push_back(gray.at<uchar>(y,x));
    if(background.empty())return 0;
    const double threshold=std::clamp(percentile(background,.75)-25,35.0,160.0);
    const double start=std::max(hub.size.width,hub.size.height)*.55+3;
    auto reach=[&](double a) {
        const cv::Point2d u(std::cos(a),std::sin(a)),v(-u.y,u.x);int gap=0;double last=0;
        for(double d=start;d<radius*.45;d+=2) {
            int darkest=255;
            for(double side:{-3.,0.,3.}) {
                const auto p=cv::Point2d(hub.center)+u*d+v*side;const int x=cvRound(p.x),y=cvRound(p.y);
                if(x>=0&&y>=0&&x<gray.cols&&y<gray.rows)darkest=std::min(darkest,int(gray.at<uchar>(y,x)));
            }
            if(darkest<threshold){gap=0;last=d;}else ++gap;
            if(gap>3)break;
        }
        return std::max(0.0,last-start);
    };
    double best=0;
    for(int i=0;i<90;++i){const double a=reach(i*CV_PI/90),b=reach(i*CV_PI/90+CV_PI);
        if(std::max(a,b)>=radius*.16&&std::min(a,b)>=radius*.07)best=std::max(best,std::min(a,b)+std::max(a,b)*.3);}
    return best;
}
inline double rimEdgeCoverage(const cv::RotatedRect& rim,const cv::Mat& gx,const cv::Mat& gy) {
    const auto axes=ellipseAxes(rim);
    const int tolerance=std::max(2,cvRound(std::min(rim.size.width,rim.size.height)*.012));
    int supported=0;std::array<int,4> quadrants{};
    for(int i=0;i<72;++i) {
        const double a=i*2*CV_PI/72;
        const auto offset=axes*cv::Vec2d(rim.size.width*.5*std::cos(a),rim.size.height*.5*std::sin(a));
        auto normal=axes*cv::Vec2d(std::cos(a)/(rim.size.width*.5),std::sin(a)/(rim.size.height*.5));normal*=1/cv::norm(normal);
        bool found=false;
        for(int d=-tolerance;d<=tolerance&&!found;++d) {
            const int x=cvRound(rim.center.x+offset[0]+normal[0]*d),y=cvRound(rim.center.y+offset[1]+normal[1]*d);
            if(x<0||y<0||x>=gx.cols||y>=gx.rows)continue;
            const double xg=gx.at<float>(y,x),yg=gy.at<float>(y,x),m=std::hypot(xg,yg);
            found=m>60&&(xg*normal[0]+yg*normal[1])<-m*.75;
        }
        if(found){++supported;++quadrants[i/18];}
    }
    // A single clean arc cannot justify extrapolating an entire dial.
    if(*std::min_element(quadrants.begin(),quadrants.end())<8)return 0;
    return double(supported)/72;
}
inline int rimInteriorBrightness(const cv::Mat& gray,cv::Point2f center,const cv::Vec2d& offset) {
    // A fixed 0.96-radius pixel can land on the dark decorative lip rather
    // than the paper face. Require two bright samples in the interior band;
    // one isolated reflection cannot make an otherwise dark interior pass.
    std::array<int,6> samples;int index=0;
    for(double factor:{.88,.90,.92,.94,.96,.98}) {
        const int x=cvRound(center.x+offset[0]*factor),y=cvRound(center.y+offset[1]*factor);
        if(x<0||y<0||x>=gray.cols||y>=gray.rows)return -1;
        samples[index++]=gray.at<uchar>(y,x);
    }
    std::sort(samples.begin(),samples.end());return samples[4];
}
inline bool supportedMappingHandles(const std::vector<cv::Point2f>& points,const std::vector<cv::Point2f>& measured,
                                    const cv::RotatedRect& rim,cv::Size size) {
    if(points.size()!=4||measured.size()!=4||!cv::isContourConvex(points))return false;
    const double limit=std::min(rim.size.width,rim.size.height)*.06;
    for(int i=0;i<4;++i) {
        const auto p=points[i];
        if(!std::isfinite(p.x)||!std::isfinite(p.y)||p.x<0||p.y<0||p.x>=size.width||p.y>=size.height||
           cv::norm(p-measured[i])>limit||std::abs(normalizedRadius(rim,p)-1)>.06)return false;
    }
    return true;
}
inline cv::RotatedRect refineRim(cv::RotatedRect rim,const cv::Mat& gx,const cv::Mat& gy) {
    const auto initial=rim;
    for(int iteration=0;iteration<3;++iteration) {
        const auto axes=ellipseAxes(rim);
        const int tolerance=std::max(3,cvRound(std::min(rim.size.width,rim.size.height)*.04));
        std::vector<cv::Point2f> points;
        for(int i=0;i<144;++i) {
            const double a=i*2*CV_PI/144;
            const auto offset=axes*cv::Vec2d(rim.size.width*.5*std::cos(a),rim.size.height*.5*std::sin(a));
            auto normal=axes*cv::Vec2d(std::cos(a)/(rim.size.width*.5),std::sin(a)/(rim.size.height*.5));normal*=1/cv::norm(normal);
            double best=0;cv::Point2f point;
            for(int d=-tolerance;d<=tolerance;++d) {
                const int x=cvRound(rim.center.x+offset[0]+normal[0]*d),y=cvRound(rim.center.y+offset[1]+normal[1]*d);
                if(x<0||y<0||x>=gx.cols||y>=gx.rows)continue;
                const double xg=gx.at<float>(y,x),yg=gy.at<float>(y,x),m=std::hypot(xg,yg),outward=xg*normal[0]+yg*normal[1];
                if(m<60||outward>-m*.75)continue;
                const double score=-outward/(1+.025*d*d);
                if(score>best){best=score;point={float(x),float(y)};}
            }
            if(best>0)points.push_back(point);
        }
        if(points.size()<100)return initial;
        auto fitted=cv::fitEllipse(points);
        // A few high-contrast reflection/text edges can bias least squares.
        // Use deterministic robust hypotheses before the final full-rim check.
        cv::RNG random(0x4d455445);size_t bestCount=0;
        for(int attempt=0;attempt<64;++attempt) {
            auto hypothesis=fitted;
            if(attempt>0) {
                std::vector<cv::Point2f> sample;
                for(int bin=0;bin<8;++bin) {
                    const int begin=int(points.size()*bin/8),end=int(points.size()*(bin+1)/8);
                    sample.push_back(points[random.uniform(begin,end)]);
                }
                hypothesis=cv::fitEllipse(sample);
            }
            if(!std::isfinite(hypothesis.center.x)||!std::isfinite(hypothesis.center.y)||
               !std::isfinite(hypothesis.size.width)||!std::isfinite(hypothesis.size.height)||!std::isfinite(hypothesis.angle)||
               hypothesis.size.width<4||hypothesis.size.height<4||
               cv::norm(hypothesis.center-initial.center)>std::min(initial.size.width,initial.size.height)*.08||
               std::abs(hypothesis.size.width/initial.size.width-1)>.10||std::abs(hypothesis.size.height/initial.size.height-1)>.10)continue;
            size_t count=0;for(auto p:points)count+=std::abs(normalizedRadius(hypothesis,p)-1)<.015;
            if(count>bestCount){bestCount=count;fitted=hypothesis;}
        }
        // Refit after dropping isolated lettering and reflection edges.
        std::vector<cv::Point2f> inliers;
        for(auto p:points)if(std::abs(normalizedRadius(fitted,p)-1)<.02)inliers.push_back(p);
        if(inliers.size()<80)return initial;
        fitted=cv::fitEllipse(inliers);
        if(!std::isfinite(fitted.center.x)||!std::isfinite(fitted.center.y)||
           !std::isfinite(fitted.size.width)||!std::isfinite(fitted.size.height)||!std::isfinite(fitted.angle)||fitted.size.width<4||fitted.size.height<4||
           cv::norm(fitted.center-initial.center)>std::min(initial.size.width,initial.size.height)*.08||
           std::abs(fitted.size.width/initial.size.width-1)>.10||std::abs(fitted.size.height/initial.size.height-1)>.10)return initial;
        rim=fitted;
    }
    return rim;
}
inline cv::RotatedRect recoverRim(const cv::RotatedRect& seed,const cv::Mat& gx,const cv::Mat& gy) {
    // Broken contours can fit the housing/reflection instead of the inner lip.
    // Propose nearby conics, then require distributed signed gradient support
    // over the entire circumference before refining any of them.
    struct Candidate {cv::RotatedRect rim;double coverage;};std::vector<Candidate> candidates;
    const auto axes=ellipseAxes(seed);const double minor=std::min(seed.size.width,seed.size.height);
    const bool widthMinor=seed.size.width<=seed.size.height;
    const cv::Vec2d minorAxis(axes(0,widthMinor?0:1),axes(1,widthMinor?0:1));
    const cv::Vec2d majorAxis(axes(0,widthMinor?1:0),axes(1,widthMinor?1:0));
    for(int shift=-3;shift<=3;++shift)for(int lateral=-1;lateral<=1;++lateral)
    for(double small:{.85,.90,.95,1.,1.05})for(double large:{.95,.975,1.,1.025,1.05,1.075}) {
        auto rim=seed;const auto delta=minorAxis*(shift*minor*.04)+majorAxis*(lateral*minor*.04);
        rim.center+=cv::Point2f(float(delta[0]),float(delta[1]));
        rim.size.width*=float(widthMinor?small:large);rim.size.height*=float(widthMinor?large:small);
        const double coverage=rimEdgeCoverage(rim,gx,gy);
        if(coverage>=.78)candidates.push_back({rim,coverage});
    }
    std::sort(candidates.begin(),candidates.end(),[](const Candidate& a,const Candidate& b){return a.coverage>b.coverage;});
    cv::RotatedRect best=seed;double support=rimEdgeCoverage(seed,gx,gy);
    for(size_t i=0;i<std::min(size_t(8),candidates.size());++i) {
        const auto refined=refineRim(candidates[i].rim,gx,gy);const double coverage=rimEdgeCoverage(refined,gx,gy);
        if(coverage>support){support=coverage;best=refined;}
    }
    return best;
}
inline cv::Mat metricHomography(const cv::RotatedRect& rim,cv::Point2f hub) {
    // The polar of the image of a circle's physical center is its vanishing
    // line. This supplies the projective constraint missing from ellipse-only
    // stretching. The pin/rivet center is the measured physical-center proxy.
    const auto axes=ellipseAxes(rim);
    const cv::Matx22d diagonal(4/(rim.size.width*rim.size.width),0,0,4/(rim.size.height*rim.size.height));
    const auto A=axes*diagonal*axes.t();const cv::Vec2d delta(hub.x-rim.center.x,hub.y-rim.center.y);
    const auto b=A*delta;const double c=delta.dot(b)-1;
    if(c>=-.15)return {};
    const cv::Matx22d bb(b[0]*b[0],b[0]*b[1],b[1]*b[0],b[1]*b[1]);
    const cv::Mat M((A-bb*(1/c))*(-1/c));cv::Mat eigenvalues,eigenvectors;
    if(!cv::eigen(M,eigenvalues,eigenvectors)||eigenvalues.at<double>(1)<=0)return {};
    cv::Mat roots=cv::Mat::zeros(2,2,CV_64F);roots.at<double>(0,0)=std::sqrt(eigenvalues.at<double>(0));roots.at<double>(1,1)=std::sqrt(eigenvalues.at<double>(1));
    const cv::Mat S=eigenvectors.t()*roots*eigenvectors;
    const cv::Mat project=(cv::Mat_<double>(3,3)<<S.at<double>(0,0),S.at<double>(0,1),0,S.at<double>(1,0),S.at<double>(1,1),0,b[0]/c,b[1]/c,1);
    const cv::Mat translate=(cv::Mat_<double>(3,3)<<1,0,-hub.x,0,1,-hub.y,0,0,1);
    return project*translate;
}
struct GraduationFit {int support{0};double residual{-1},aspect{0},score{0};};
struct GraduationExclusion {
    LiquidSurface surface;
    cv::Mat sourceToPreview;
};
inline std::vector<cv::Vec4f> reliableGraduationEdges(const std::vector<cv::Vec4f>& lines,
                                                    const GraduationExclusion& exclusion,double sourceScale=1) {
    // Ambiguous liquid boundaries cannot justify using possibly refracted ticks.
    if(exclusion.surface.ambiguous)return {};
    if(!exclusion.surface.found)return lines;
    if(exclusion.sourceToPreview.empty()||sourceScale<=0)return {};
    std::vector<cv::Vec4f> reliable;
    for(const auto& line:lines) {
        std::vector<cv::Point2f> samples;
        for(int i=0;i<=4;++i) {
            const double t=i/4.;
            samples.emplace_back(float((line[0]+(line[2]-line[0])*t)/sourceScale),
                                 float((line[1]+(line[3]-line[1])*t)/sourceScale));
        }
        cv::perspectiveTransform(samples,samples,exclusion.sourceToPreview);
        bool safe=true;
        for(auto p:samples)if(!std::isfinite(p.x)||!std::isfinite(p.y)||!std::isfinite(exclusion.surface.yAt(p.x))||
            p.y<=exclusion.surface.yAt(p.x)+std::max(0.,exclusion.surface.margin)+2){safe=false;break;}
        if(safe)reliable.push_back(line);
    }
    return reliable;
}
inline GraduationFit graduationFit(const std::vector<cv::Vec4f>& lines,const cv::Mat& H) {
    GraduationFit result;if(H.empty())return result;
    std::array<cv::Point2f,72> outer{};std::array<double,72> lengths{};
    for(const auto& line:lines) {
        std::vector<cv::Point2f> p{{line[0],line[1]},{line[2],line[3]}};cv::perspectiveTransform(p,p,H);
        const auto mid=(p[0]+p[1])*.5f,delta=p[1]-p[0];const double r=cv::norm(mid),length=cv::norm(delta);
        if(r<.60||r>.93||length<.018||length>.22||std::abs(delta.x*mid.y-delta.y*mid.x)>length*r*.30)continue;
        const auto end=cv::norm(p[0])>cv::norm(p[1])?p[0]:p[1];
        const int bin=int((std::atan2(mid.y,mid.x)+CV_PI)*72/(2*CV_PI))%72;
        if(length>lengths[bin]){lengths[bin]=length;outer[bin]=end;}
    }
    std::vector<cv::Point2f> points;std::array<int,4> quadrants{};
    for(int i=0;i<72;++i)if(lengths[i]>0){points.push_back(outer[i]);++quadrants[i/18];}
    if(points.size()<24||std::count_if(quadrants.begin(),quadrants.end(),[](int n){return n>=5;})<3)return result;
    auto ellipse=cv::fitEllipse(points);
    if(ellipse.size.width<.9||ellipse.size.height<.9||ellipse.size.width>2.1||ellipse.size.height>2.1)return result;
    std::vector<cv::Point2f> inliers;
    for(auto p:points)if(std::abs(normalizedRadius(ellipse,p)-1)<.045)inliers.push_back(p);
    if(inliers.size()<24||inliers.size()<points.size()*.70)return result;
    ellipse=cv::fitEllipse(inliers);std::vector<double> errors;
    for(auto p:inliers)errors.push_back(std::abs(normalizedRadius(ellipse,p)-1));
    result.support=int(inliers.size());result.residual=percentile(errors,.75);
    result.aspect=std::min(ellipse.size.width,ellipse.size.height)/std::max(ellipse.size.width,ellipse.size.height);
    // The printed graduation circle supplies evidence from the dial plane,
    // independent of housing reflections and a protruding axis cap.
    result.score=1.8*result.aspect-8*result.residual;
    return result;
}
inline std::vector<cv::Point2f> graduationEnvelope(const std::vector<cv::Vec4f>& lines,const cv::Mat& reference) {
    std::array<cv::Point2f,72> tips{};std::array<double,72> lengths{};
    for(const auto& line:lines) {
        const std::vector<cv::Point2f> source{{line[0],line[1]},{line[2],line[3]}};std::vector<cv::Point2f> p;
        cv::perspectiveTransform(source,p,reference);const auto mid=(p[0]+p[1])*.5f,d=p[1]-p[0];const double r=cv::norm(mid),length=cv::norm(d);
        if(r<.6||r>.94||length<.018||length>.22||std::abs(d.x*mid.y-d.y*mid.x)>length*r*.30)continue;
        const int bin=int((std::atan2(mid.y,mid.x)+CV_PI)*72/(2*CV_PI))%72;
        if(length>lengths[bin]){lengths[bin]=length;tips[bin]=source[cv::norm(p[0])>cv::norm(p[1])?0:1];}
    }
    std::vector<cv::Point2f> result;std::array<int,4> quadrants{};
    for(int i=0;i<72;++i)if(lengths[i]>0){result.push_back(tips[i]);++quadrants[i/18];}
    if(result.size()<18||std::count_if(quadrants.begin(),quadrants.end(),[](int n){return n>=4;})<3)return {};
    return result;
}
inline double graduationEnvelopeRadius(const std::vector<cv::Point2f>& envelope,const cv::Mat& H) {
    if(envelope.empty()||H.empty())return -1;
    std::vector<cv::Point2f> mapped;cv::perspectiveTransform(envelope,mapped,H);std::vector<double> radii;
    for(auto p:mapped){const double r=cv::norm(p);if(!std::isfinite(r))return -1;radii.push_back(r);}
    return percentile(radii,.9);
}
inline cv::RotatedRect refinePrintedGeometry(cv::RotatedRect rim,cv::Point2f hub,const cv::Mat& gx,const cv::Mat& gy,
                                             const std::vector<cv::Vec4f>& segments,double geometryLimit=1) {
    const auto initial=rim;const auto original=graduationFit(segments,metricHomography(rim,hub));
    if(original.support<24||original.aspect>=.985)return rim;
    const double minor=std::min(rim.size.width,rim.size.height),coverage=rimEdgeCoverage(rim,gx,gy);
    auto objective=[&](const cv::RotatedRect& candidate) {
        if(cv::norm(candidate.center-initial.center)>minor*.025*geometryLimit||
           std::abs(candidate.size.width/initial.size.width-1)>.05*geometryLimit||std::abs(candidate.size.height/initial.size.height-1)>.05*geometryLimit||
           std::abs(candidate.angle-initial.angle)>2*geometryLimit)return -1e9;
        const double edge=rimEdgeCoverage(candidate,gx,gy);
        if(edge<std::max(.72,coverage-.06))return -1e9;
        const auto fit=graduationFit(segments,metricHomography(candidate,hub));
        if(fit.support<std::max(24,original.support-2)||fit.residual>original.residual+.003)return -1e9;
        const double displacement=cv::norm(candidate.center-initial.center)/minor+
            std::abs(candidate.size.width/initial.size.width-1)+std::abs(candidate.size.height/initial.size.height-1);
        return fit.score+.002*fit.support+.1*edge-.15*displacement;
    };
    double best=objective(rim);
    // Bounded local search: printed tick endpoints choose between nearby,
    // gradient-supported conics, not an unconstrained ellipse invented from text.
    for(double scaleStep:{.015,.0075,.00375})for(int sweep=0;sweep<2;++sweep) {
        const double step=scaleStep*geometryLimit;
        bool changed=false;
        for(int axis=0;axis<5;++axis)for(int sign:{-1,1}) {
            auto candidate=rim;
            if(axis==0)candidate.center.x+=float(sign*step*minor*.5);
            if(axis==1)candidate.center.y+=float(sign*step*minor*.5);
            if(axis==2)candidate.size.width*=float(1+sign*step);
            if(axis==3)candidate.size.height*=float(1+sign*step);
            if(axis==4)candidate.angle+=float(sign*step*30);
            const double score=objective(candidate);
            if(score>best+.001){rim=candidate;best=score;changed=true;}
        }
        if(!changed)break;
    }
    return rim;
}
inline Result detect(const cv::Mat& input,bool refineGeometry=true) {
    Result failure;
    if(input.empty()||input.depth()!=CV_8U||std::min(input.cols,input.rows)<120){failure.error=QStringLiteral(u"图像为空、过小或格式不支持，请使用清晰原图。");return failure;}
    cv::Mat image,gray;
    // Keep contour, hub and stroke thresholds on the same working resolution.
    const double scale=800.0/std::max(input.cols,input.rows);
    cv::resize(input,image,{},scale,scale,cv::INTER_AREA);
    if(image.channels()==3)cv::cvtColor(image,gray,cv::COLOR_BGR2GRAY);
    else if(image.channels()==4)cv::cvtColor(image,gray,cv::COLOR_BGRA2GRAY);
    else if(image.channels()==1)gray=image;
    else {failure.error=QStringLiteral(u"不支持的图像通道数。");return failure;}
    cv::Mat blurred,gx,gy;cv::GaussianBlur(gray,blurred,{5,5},0);cv::Scharr(gray,gx,CV_32F,1,0);cv::Scharr(gray,gy,CV_32F,0,1);
    std::vector<cv::Mat> masks(1);cv::Canny(blurred,masks[0],40,100);
    cv::Mat joined;
    cv::morphologyEx(masks.front(),joined,cv::MORPH_CLOSE,cv::getStructuringElement(cv::MORPH_ELLIPSE,{3,3}));
    masks.push_back(joined);
    cv::Mat faintEdges;cv::Canny(blurred,faintEdges,20,60);masks.push_back(faintEdges);
    for(int t:{60,75,90,105,120,135,150,165,180}){cv::Mat binary;cv::threshold(blurred,binary,t,255,cv::THRESH_BINARY);masks.push_back(binary);}
    // JPEG blocks and resampling can break an otherwise identical rim at one
    // smoothing scale. Collect alternative seeds, then use the same original
    // image gradient, hub and scale checks for every candidate.
    for(int kernel:{3,7,9}) {
        cv::Mat smooth,edges;cv::GaussianBlur(gray,smooth,{kernel,kernel},0);
        cv::Canny(smooth,edges,20,60);masks.push_back(edges);
        cv::Mat closed;cv::morphologyEx(edges,closed,cv::MORPH_CLOSE,cv::getStructuringElement(cv::MORPH_ELLIPSE,{3,3}));masks.push_back(closed);
        for(int t:{60,90,120,150,180}){cv::Mat binary;cv::threshold(smooth,binary,t,255,cv::THRESH_BINARY);masks.push_back(binary);}
    }
    std::vector<Shape> shapes;
    for(const auto& mask:masks) {
        std::vector<std::vector<cv::Point>> contours;cv::findContours(mask,contours,cv::RETR_LIST,cv::CHAIN_APPROX_NONE);
        for(const auto& points:contours) {
            if(points.size()<12)continue;
            const auto ellipse=cv::fitEllipse(points);
            if(!std::isfinite(ellipse.center.x)||!std::isfinite(ellipse.center.y)||!std::isfinite(ellipse.size.width)||
               !std::isfinite(ellipse.size.height)||!std::isfinite(ellipse.angle)||ellipse.size.width<4||ellipse.size.height<4||ellipse.size.area()>gray.total()*4)continue;
            std::vector<double> residuals;std::array<bool,36> bins{};const auto axes=ellipseAxes(ellipse);
            for(const auto& p:points){const auto v=axes.t()*cv::Vec2d(p.x-ellipse.center.x,p.y-ellipse.center.y);
                const double x=v[0]/(ellipse.size.width*.5),y=v[1]/(ellipse.size.height*.5);
                residuals.push_back(std::abs(std::hypot(x,y)-1));const int bin=int((std::atan2(y,x)+CV_PI)*36/(2*CV_PI))%36;bins[bin]=true;}
            const double residual=percentile(residuals,.9);
            if(residual>.12)continue;
            shapes.push_back({ellipse,residual,double(std::count(bins.begin(),bins.end(),true))/36});
        }
    }
    struct Candidate {Result result;double score,shapeScore;};std::vector<Candidate> candidates;
    int rimFits=0,boundaries=0,hubMatches=0;
    for(const auto& shape:shapes) {
        auto rim=shape.ellipse;const double minor=std::min(rim.size.width,rim.size.height),major=std::max(rim.size.width,rim.size.height),radius=major*.5;
        const double area=CV_PI*rim.size.area()/4;
        if(minor<std::min(gray.cols,gray.rows)*.30 || major>std::max(gray.cols,gray.rows)*1.1 || minor/major<.3 ||
           area<gray.total()*.03 || area>gray.total()*.85 || shape.residual>.04 || shape.coverage<.45)continue;
        ++rimFits;
        rim=refineRim(rim,gx,gy);
        if(rimEdgeCoverage(rim,gx,gy)<.78)rim=recoverRim(shape.ellipse,gx,gy);
        const double coverage=rimEdgeCoverage(rim,gx,gy);
        if(coverage<.78)continue;
        const auto axes=ellipseAxes(rim);int positive=0;bool clipped=false;
        for(int i=0;i<72;++i){const double a=i*2*CV_PI/72;const auto v=axes*cv::Vec2d(rim.size.width*.5*std::cos(a),rim.size.height*.5*std::sin(a));
            const cv::Point inside(cvRound(rim.center.x+v[0]*.96),cvRound(rim.center.y+v[1]*.96));
            const cv::Point outside(cvRound(rim.center.x+v[0]*1.04),cvRound(rim.center.y+v[1]*1.04));
            if(inside.x<1||inside.y<1||outside.x<1||outside.y<1||inside.x>=gray.cols-1||inside.y>=gray.rows-1||outside.x>=gray.cols-1||outside.y>=gray.rows-1){clipped=true;break;}
            // The dark inner lip can be narrower than 4% of the radius. A
            // single far-outside pixel can land on the bright metal housing
            // and invert the contrast even when the rim gradient is valid.
            int darkOutside=gray.at<uchar>(outside);
            for(double factor:{1.0,1.01,1.02,1.03}) {
                const int x=cvRound(rim.center.x+v[0]*factor),y=cvRound(rim.center.y+v[1]*factor);
                if(x>=0&&y>=0&&x<gray.cols&&y<gray.rows)darkOutside=std::min(darkOutside,int(gray.at<uchar>(y,x)));
            }
            const int brightness=rimInteriorBrightness(gray,rim.center,v);
            if(brightness<0){clipped=true;break;}
            const double contrast=brightness-darkOutside;positive+=contrast>12;}
        if(clipped||positive<22)continue;
        ++boundaries;
        struct Hub {cv::Point2f point;double score,support;};std::vector<Hub> hubs;
        for(const auto& pin:shapes) {
            const double size=std::max(pin.ellipse.size.width,pin.ellipse.size.height),offset=normalizedRadius(rim,pin.ellipse.center);
            if(offset>.62||size<major*.025||size>major*.15||std::min(pin.ellipse.size.width,pin.ellipse.size.height)/size<.35)continue;
            const double support=edgeSupport(pin.ellipse,gx,gy);if(support<.72)continue;
            const double spokes=needleSpokes(gray,pin.ellipse,radius);if(spokes<=0)continue;
            const double score=support+.3*std::min(1.0,spokes/(radius*.4))-.1*offset;
            bool duplicate=false;
            for(auto& hub:hubs)if(cv::norm(hub.point-pin.ellipse.center)<radius*.04){if(score>hub.score)hub={pin.ellipse.center,score,support};duplicate=true;break;}
            if(!duplicate)hubs.push_back({pin.ellipse.center,score,support});
        }
        if(hubs.empty())continue;
        std::sort(hubs.begin(),hubs.end(),[](const Hub& a,const Hub& b){return a.score>b.score;});
        if(hubs.size()>1&&hubs[0].score-hubs[1].score<.10)continue;
        ++hubMatches;
        const auto hub=hubs.front();const cv::Mat H=metricHomography(rim,hub.point);if(H.empty())continue;
        std::vector<cv::Point2f> unit{{0,-1},{1,0},{0,1},{-1,0}},points;
        cv::perspectiveTransform(unit,points,H.inv());
        bool valid=cv::isContourConvex(points);
        for(auto p:points)valid&=std::isfinite(p.x)&&std::isfinite(p.y)&&p.x>=0&&p.y>=0&&p.x<gray.cols&&p.y<gray.rows;
        if(!valid)continue;
        Result result;result.rim=rim;result.hub=hub.point;result.points=points;result.contourResidual=shape.residual;
        result.rimCoverage=coverage;result.hubSupport=hub.support;
        // Prefer a fit measured over a longer arc, rather than a very precise
        // short arc that can extrapolate onto lettering or graduation ends.
        const double score=2.5*shape.coverage+.3*double(positive)/72+.25*(1-shape.residual/.04)+.3*coverage+.3*hub.score;
        bool duplicate=false;
        for(auto& c:candidates)if(cv::norm(c.result.hub-hub.point)<radius*(refineGeometry?.008:.015)&&
            cv::norm(c.result.rim.center-rim.center)<radius*(refineGeometry?.012:.10)&&
            std::abs(c.result.rim.size.area()/rim.size.area()-1)<(refineGeometry?.025:.10)) {
            if(score>c.score)c={result,score,score};duplicate=true;break;
        }
        if(!duplicate)candidates.push_back({result,score,score});
    }
    if(candidates.empty()) {
        const QString reason=rimFits==0?QStringLiteral(u"未找到足够长的表盘弧线"):
            boundaries==0?QStringLiteral(u"候选边界的完整圆周支持或内外对比不足"):
            hubMatches==0?QStringLiteral(u"边界候选存在，但真实轴帽及针体关联未通过"):
            QStringLiteral(u"表盘校正点不可靠");
        failure.error=QStringLiteral(u"%1（标定底图 %2×%3）。可保存标定原图以复核，或手动调整。").arg(reason).arg(input.cols).arg(input.rows);
        return failure;
    }
    GraduationExclusion exclusion;std::vector<cv::Vec4f> graduationSegments;
    if(refineGeometry) {
        const auto reference=std::max_element(candidates.begin(),candidates.end(),[](const Candidate& a,const Candidate& b){return a.result.rimCoverage<b.result.rimCoverage;});
        const auto referenceHub=reference->result.hub;const double referenceRadius=std::max(reference->result.rim.size.width,reference->result.rim.size.height)*.5;
        const auto referenceH=metricHomography(reference->result.rim,referenceHub);
        const cv::Mat preview=(cv::Mat_<double>(3,3)<<360,0,400,0,360,400,0,0,1);
        exclusion.sourceToPreview=preview*referenceH;
        cv::Mat initialView;cv::warpPerspective(gray,initialView,exclusion.sourceToPreview,{800,800},cv::INTER_CUBIC);
        exclusion.surface=detectLiquidSurface(initialView,{400,400},360);
        std::vector<cv::Vec4f> segments;cv::createLineSegmentDetector()->detect(gray,segments);
        graduationSegments=reliableGraduationEdges(segments,exclusion);
        const auto envelope=graduationEnvelope(graduationSegments,referenceH);
        const double referenceExtent=graduationEnvelopeRadius(envelope,referenceH);
        const double referenceArea=reference->result.rim.size.area();
        // Freeze the same observed tips for all conics. An inner candidate may
        // not discard the strokes it cuts through and then call the larger,
        // well-supported boundary a housing. Leave other physical dials in the
        // candidate set so this check cannot hide multi-dial ambiguity.
        if(referenceExtent>0&&referenceExtent<.95) {
            candidates.erase(std::remove_if(candidates.begin(),candidates.end(),[&](const Candidate& candidate){
                if(cv::norm(candidate.result.hub-referenceHub)>referenceRadius*.1||candidate.result.rim.size.area()>=referenceArea*.9)return false;
                return graduationEnvelopeRadius(envelope,metricHomography(candidate.result.rim,candidate.result.hub))>.97;
            }),candidates.end());
        }
    }
    std::vector<Candidate> innerCandidates;
    for(const auto& candidate:candidates) {
        bool housing=false;
        const auto& rim=candidate.result.rim;
        const double radius=std::max(rim.size.width,rim.size.height)*.5;
        for(const auto& other:candidates) {
            const auto& inner=other.result.rim;
            const double ratio=inner.size.area()/rim.size.area();
            if(ratio<.90&&ratio>.5&&cv::norm(other.result.hub-candidate.result.hub)<radius*.02&&
               cv::norm(inner.center-rim.center)<radius*.15&&
               inner.size.width<rim.size.width&&inner.size.height<rim.size.height) {housing=true;break;}
        }
        if(!housing)innerCandidates.push_back(candidate);
    }
    candidates=std::move(innerCandidates);
    // Graduation quality may choose a better conic of one dial, but must not
    // hide an equally plausible second physical instrument in the frame.
    const auto strongest=std::max_element(candidates.begin(),candidates.end(),[](const Candidate& a,const Candidate& b){return a.shapeScore<b.shapeScore;});
    const double physicalRadius=std::max(strongest->result.rim.size.width,strongest->result.rim.size.height)*.5;
    for(const auto& candidate:candidates)if(strongest->shapeScore-candidate.shapeScore<.10&&
        cv::norm(candidate.result.hub-strongest->result.hub)>physicalRadius*.2) {
        failure.error=QStringLiteral(u"存在多个相近的表盘候选，请手动选择目标表盘。");return failure;
    }
    if(refineGeometry) {
        for(auto& candidate:candidates) {
            const auto fit=graduationFit(graduationSegments,metricHomography(candidate.result.rim,candidate.result.hub));
            candidate.result.graduationSupport=fit.support;candidate.result.graduationResidual=fit.residual;candidate.result.graduationAspect=fit.aspect;
            if(fit.support>=24)candidate.score+=fit.score;
        }
    }
    std::sort(candidates.begin(),candidates.end(),[](const Candidate& a,const Candidate& b){return a.score>b.score;});
    // Online representatives can move after a better seed is found. Merge
    // once more against the final winner before testing distinct-dial ambiguity.
    const auto& winner=candidates.front().result;
    const double winnerRadius=std::max(winner.rim.size.width,winner.rim.size.height)*.5;
    candidates.erase(std::remove_if(candidates.begin()+1,candidates.end(),[&](const Candidate& candidate){
        return cv::norm(candidate.result.hub-winner.hub)<winnerRadius*.015&&
            cv::norm(candidate.result.rim.center-winner.rim.center)<winnerRadius*.10&&
            std::abs(candidate.result.rim.size.area()/winner.rim.size.area()-1)<.10;
    }),candidates.end());
    if(candidates.size()>1&&candidates[0].score-candidates[1].score<.10){failure.error=QStringLiteral(u"存在多个相近的表盘候选，请手动选择目标表盘。");return failure;}
    auto result=candidates.front().result;
    result.graduationLiquidExcluded=exclusion.surface.found;
    result.graduationLiquidAmbiguous=exclusion.surface.ambiguous;
    if(refineGeometry&&result.graduationSupport>=24) {
        result.initialGraduationAspect=result.graduationAspect;
        // A liquid mask leaves a partial ring. Do not extrapolate a large
        // five-parameter conic correction from that incomplete observation.
        const auto refined=refinePrintedGeometry(result.rim,result.hub,gx,gy,graduationSegments,exclusion.surface.found?.2:1.);
        std::vector<cv::Point2f> refinedPoints;
        cv::perspectiveTransform(std::vector<cv::Point2f>{{0,-1},{1,0},{0,1},{-1,0}},refinedPoints,metricHomography(refined,result.hub).inv());
        bool inside=cv::isContourConvex(refinedPoints);
        for(auto p:refinedPoints)inside&=std::isfinite(p.x)&&std::isfinite(p.y)&&p.x>=0&&p.y>=0&&p.x<gray.cols&&p.y<gray.rows;
        if(inside) {
            result.rim=refined;result.points=refinedPoints;result.rimCoverage=rimEdgeCoverage(refined,gx,gy);
            const auto fit=graduationFit(graduationSegments,metricHomography(refined,result.hub));
            result.graduationSupport=fit.support;result.graduationResidual=fit.residual;result.graduationAspect=fit.aspect;
        }
    }
    // Coarse detection stays fast; remeasure the chosen rim on real higher
    // resolution pixels so downsampling does not set the final corner accuracy.
    const double fineScale=std::min(1.,1600./std::max(input.cols,input.rows)),ratio=fineScale/scale;
    if(refineGeometry&&ratio>1.25&&!exclusion.surface.ambiguous) {
        cv::Mat fine,fineGray,fgx,fgy;cv::resize(input,fine,{},fineScale,fineScale,cv::INTER_AREA);
        if(fine.channels()==3)cv::cvtColor(fine,fineGray,cv::COLOR_BGR2GRAY);
        else if(fine.channels()==4)cv::cvtColor(fine,fineGray,cv::COLOR_BGRA2GRAY);else fineGray=fine;
        cv::Scharr(fineGray,fgx,CV_32F,1,0);cv::Scharr(fineGray,fgy,CV_32F,0,1);
        auto seed=result.rim;seed.center*=float(ratio);seed.size.width*=float(ratio);seed.size.height*=float(ratio);
        const auto hub=result.hub*float(ratio);
        std::vector<cv::Vec4f> lines;cv::createLineSegmentDetector()->detect(fineGray,lines);
        lines=reliableGraduationEdges(lines,exclusion,ratio);
        const auto before=graduationFit(lines,metricHomography(seed,hub));
        const auto refined=refinePrintedGeometry(refineRim(seed,fgx,fgy),hub,fgx,fgy,lines,exclusion.surface.found?.2:1.);
        const auto after=graduationFit(lines,metricHomography(refined,hub));
        const double fineCoverage=rimEdgeCoverage(refined,fgx,fgy),minor=std::min(seed.size.width,seed.size.height);
        if(after.support>=24&&after.support>=before.support*.85&&after.aspect>=before.aspect-.003&&
           (before.residual<0||after.residual<=before.residual+.003)&&fineCoverage>=std::max(.72,rimEdgeCoverage(seed,fgx,fgy)-.05)&&
           cv::norm(refined.center-seed.center)<minor*.025&&std::abs(refined.size.width/seed.size.width-1)<.05&&std::abs(refined.size.height/seed.size.height-1)<.05) {
            std::vector<cv::Point2f> p;
            cv::perspectiveTransform(std::vector<cv::Point2f>{{0,-1},{1,0},{0,1},{-1,0}},p,metricHomography(refined,hub).inv());
            bool inside=cv::isContourConvex(p);for(auto v:p)inside&=std::isfinite(v.x)&&std::isfinite(v.y)&&v.x>=0&&v.y>=0&&v.x<fine.cols&&v.y<fine.rows;
            if(inside) {
                result.rim=refined;result.rim.center*=float(1/ratio);result.rim.size.width/=float(ratio);result.rim.size.height/=float(ratio);
                for(auto& v:p)v*=float(1/ratio);result.points=p;result.rimCoverage=fineCoverage;
                result.graduationSupport=after.support;result.graduationResidual=after.residual;result.graduationAspect=after.aspect;
            }
        }
    }
    if(refineGeometry&&!exclusion.surface.ambiguous) {
        const std::vector<cv::Point2f> unit{{0,-1},{1,0},{0,1},{-1,0}};
        const auto preliminary=cv::getPerspectiveTransform(result.points,unit);
        const auto fitted=GraduationRectification::fit(graduationSegments,preliminary,[](const cv::RotatedRect& circle,cv::Point2f center){return metricHomography(circle,center);});
        if(fitted.valid()) {
            const cv::Mat H=fitted.correction*preliminary;std::vector<cv::Point2f> points;
            cv::perspectiveTransform(unit,points,H.inv());
            // Lower-arc consistency alone cannot justify extrapolating the top
            // outside the measured rim. Reject such a correction; do not zoom
            // it back into the image and present it as a supported boundary.
            bool inside=supportedMappingHandles(points,result.points,result.rim,gray.size());
            const auto independent=graduationFit(graduationSegments,H);
            if(independent.support>=24&&result.graduationSupport>=24&&independent.aspect<result.graduationAspect-.005)inside=false;
            if(inside) {
                result.points=points;result.structuralRectification=true;result.structuralSupport=fitted.support;result.structuralSymmetricPairs=fitted.symmetricPairs;
                std::vector<cv::Point2f> origin{{0,0}};cv::perspectiveTransform(origin,origin,H.inv());result.graduationCenter=origin[0];
                result.structuralErrorBefore=fitted.before;result.structuralErrorAfter=fitted.after;
                const auto fit=graduationFit(graduationSegments,H);
                result.graduationSupport=fit.support;result.graduationResidual=fit.residual;result.graduationAspect=fit.aspect;
            }
        }
    }
    for(auto& p:result.points)p*=float(1/scale);
    result.hub*=float(1/scale);result.rim.center*=float(1/scale);result.rim.size.width/=float(scale);result.rim.size.height/=float(scale);
    if(result.structuralRectification)result.graduationCenter*=float(1/scale);
    return result;
}
}


