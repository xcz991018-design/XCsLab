#pragma once
#include <opencv2/opencv.hpp>
#include <algorithm>
#include <cmath>
#include <vector>

// A local image-space boundary, not a physical refraction correction model.
struct LiquidSurface {
    bool found{false}, ambiguous{false};
    double centerX{0}, radius{1}, y0{0}, slope{0}, curve{0}, margin{0}, support{0};
    double yAt(double x) const {const double t=(x-centerX)/radius;return y0+slope*t+curve*t*t;}
    bool excludes(double x,double y) const {return found && y<=yAt(x)+margin;}
};
inline LiquidSurface detectLiquidSurface(const cv::Mat& image, cv::Point2f center, double radius) {
    LiquidSurface none;
    if(image.empty() || image.depth()!=CV_8U || !std::isfinite(radius) || radius<40 ||
       !std::isfinite(center.x)||!std::isfinite(center.y)||center.x<0||center.y<0||
       center.x>=image.cols||center.y>=image.rows) return none;
    cv::Mat gray,blur,edges,gx,gy;
    if(image.channels()==3)cv::cvtColor(image,gray,cv::COLOR_BGR2GRAY);
    else if(image.channels()==4)cv::cvtColor(image,gray,cv::COLOR_BGRA2GRAY);
    else if(image.channels()==1)gray=image;
    else return none;
    cv::GaussianBlur(gray,blur,{3,3},0);cv::Canny(blur,edges,35,90);
    cv::Sobel(blur,gx,CV_32F,1,0);cv::Sobel(blur,gy,CV_32F,0,1);
    std::vector<cv::Vec4i> lines;
    cv::HoughLinesP(edges,lines,1,CV_PI/720,std::max(20,int(radius*.15)),radius*.18,radius*.05);
    struct Segment {cv::Point2f a,b;double level;};std::vector<Segment> segments;
    for(auto l:lines) {
        if(std::abs(l[2]-l[0])<radius*.16)continue;
        const double slope=double(l[3]-l[1])/(l[2]-l[0]);
        const double y=l[1]+slope*(center.x-l[0]);
        if(std::abs(slope)>.30 || y<center.y-radius*.80 || y>center.y+radius*.55)continue;
        const cv::Point2f a{float(l[0]),float(l[1])},b{float(l[2]),float(l[3])};
        if(cv::norm((a+b)*.5f-center)>radius*.94)continue;
        segments.push_back({a,b,y});
    }
    std::vector<LiquidSurface> candidates;
    for(const auto& seed:segments) {
        std::vector<cv::Point2f> points;
        double left=center.x,right=center.x;
        for(const auto& s:segments)if(std::abs(s.level-seed.level)<radius*.055) {
            for(int k=0;k<=6;++k)points.push_back(s.a+(s.b-s.a)*(k/6.0f));
            left=std::min({left,double(s.a.x),double(s.b.x)});right=std::max({right,double(s.a.x),double(s.b.x)});
        }
        if(points.size()<6 || left>center.x-radius*.5 || right<center.x+radius*.5)continue;
        LiquidSurface surface;surface.centerX=center.x;surface.radius=radius;surface.margin=std::max(6.0,radius*.025);
        bool fitted=false;
        for(int iteration=0;iteration<3 && points.size()>=6;++iteration) {
            cv::Mat A(int(points.size()),3,CV_64F),B(int(points.size()),1,CV_64F),coef;
            for(int i=0;i<int(points.size());++i){const double t=(points[i].x-center.x)/radius;
                A.at<double>(i,0)=1;A.at<double>(i,1)=t;A.at<double>(i,2)=t*t;B.at<double>(i)=points[i].y;}
            if(!cv::solve(A,B,coef,cv::DECOMP_SVD))break;
            surface.y0=coef.at<double>(0);surface.slope=coef.at<double>(1);surface.curve=coef.at<double>(2);fitted=true;
            points.erase(std::remove_if(points.begin(),points.end(),[&](cv::Point2f p){return std::abs(p.y-surface.yAt(p.x))>radius*.035;}),points.end());
        }
        if(!fitted || points.size()<6 || !std::isfinite(surface.y0) || !std::isfinite(surface.slope) || !std::isfinite(surface.curve) ||
           surface.y0<center.y-radius*.80 || surface.y0>center.y+radius*.55 ||
           std::abs(surface.slope)>radius*.25 || std::abs(surface.curve)>radius*.3)continue;
        // Require distributed horizontal edge support on both sides of the hub.
        int supported=0,leftSupport=0,rightSupport=0;
        for(int i=0;i<24;++i) {
            const double x=center.x+radius*(-.65+1.3*(i+.5)/24);
            bool hit=false;
            for(int dx=-2;dx<=2&&!hit;++dx)for(int dy=-int(radius*.022);dy<=int(radius*.022);++dy) {
                const int px=int(x)+dx,py=int(surface.yAt(x))+dy;
                if(px<1||py<1||px>=gray.cols-1||py>=gray.rows-1)continue;
                if(edges.at<uchar>(py,px) && std::abs(gy.at<float>(py,px))>std::max(25.0f,std::abs(gx.at<float>(py,px))*1.5f)){hit=true;break;}
            }
            if(hit){++supported;if(i<12)++leftSupport;else ++rightSupport;}
        }
        if(supported<19 || leftSupport<8 || rightSupport<8)continue;
        surface.found=true;surface.support=supported/24.0;
        bool duplicate=false;
        for(auto& old:candidates)if(std::abs(old.y0-surface.y0)<radius*.05){if(surface.support>old.support)old=surface;duplicate=true;break;}
        if(!duplicate)candidates.push_back(surface);
    }
    if(candidates.empty())return none;
    std::sort(candidates.begin(),candidates.end(),[](const LiquidSurface& a,const LiquidSurface& b){return a.support>b.support;});
    if(candidates.size()>1 && candidates[1].support>=candidates[0].support-.08){none.ambiguous=true;return none;}
    return candidates.front();
}
