#pragma once
#include "PointerDetector.hpp"
#include <algorithm>
#include <cmath>

// Same rectified pixel coordinates as PointerDetector's measured edge pairs.
// No angular wedge, extrapolation beyond the tip, or bridging missing edges.
inline cv::Mat pointerOcclusionMask(const PointerResult& pointer, cv::Size size) {
    cv::Mat mask(size,CV_8UC1,cv::Scalar(0));
    if(!pointer.success)return mask;
    const auto draw=[&](const std::vector<RadialSection>& sections) {
        const RadialSection* previous=nullptr;
        std::vector<double> gaps;
        for(size_t i=1;i<sections.size();++i) {
            const double gap=sections[i].radius-sections[i-1].radius;
            if(gap>0&&std::isfinite(gap))gaps.push_back(gap);
        }
        if(gaps.empty())return;
        std::sort(gaps.begin(),gaps.end());
        const double maxGap=std::min(3*gaps[gaps.size()/2],std::max(size.width,size.height)*.02);
        for(const auto& section:sections) {
            const auto finite=[](cv::Point2d p){return std::isfinite(p.x)&&std::isfinite(p.y);};
            if(!section.valid||!finite(section.leftPoint)||!finite(section.rightPoint)) {previous=nullptr;continue;}
            if(previous&&section.radius>previous->radius&&section.radius-previous->radius<=maxGap) {
                const std::vector<cv::Point> polygon{previous->leftPoint,previous->rightPoint,section.rightPoint,section.leftPoint};
                cv::fillConvexPoly(mask,polygon,cv::Scalar(255));
            }
            previous=&section;
        }
    };
    draw(pointer.sections);
    if(pointer.tailDetected)draw(pointer.tailProfile.sections);
    return mask;
}
