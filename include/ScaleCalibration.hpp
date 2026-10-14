#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <opencv2/opencv.hpp>
#include <cmath>
#include <algorithm>

// Points are stored in calibration-frame pixels. Ratios remain independent of
// the selected template's unit and range; 0 and 1 are the required endpoints.
namespace ScaleCalibration {
inline double span(double from, double to, bool clockwise) {
    return std::fmod((clockwise ? to-from : from-to) + 720.0, 360.0);
}
inline QJsonArray scaled(const QJsonArray& points, double sx, double sy) {
    QJsonArray result;
    for (const auto& value : points) {
        auto p = value.toObject(); p["x"] = p["x"].toDouble()*sx; p["y"] = p["y"].toDouble()*sy;
        result.append(p);
    }
    return result;
}
inline QJsonArray transformed(const QJsonArray& points, const cv::Mat& H) {
    if (points.isEmpty() || H.empty()) return points;
    std::vector<cv::Point2f> positions;
    for (const auto& value:points) {
        const auto p=value.toObject(); positions.emplace_back(p["x"].toDouble(),p["y"].toDouble());
    }
    cv::perspectiveTransform(positions,positions,H);
    QJsonArray result;
    for (int i=0;i<points.size();++i) {
        auto p=points[i].toObject(); p["x"]=positions[i].x; p["y"]=positions[i].y; result.append(p);
    }
    return result;
}
inline bool valid(const QJsonArray& points, int width, int height, cv::Point2f center) {
    if (points.size() < 2) return false;
    double previous = -1;
    for (const auto& value : points) {
        if (!value.isObject()) return false;
        const auto p = value.toObject();
        if (!p["x"].isDouble() || !p["y"].isDouble() || !p["ratio"].isDouble()) return false;
        const double x=p["x"].toDouble(), y=p["y"].toDouble(), r=p["ratio"].toDouble();
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(r) || x<0 || x>=width || y<0 || y>=height ||
            r<0 || r>1 || r<=previous || std::hypot(x-center.x,y-center.y)<2) return false;
        previous=r;
    }
    return points.first().toObject()["ratio"].toDouble()==0 && points.last().toObject()["ratio"].toDouble()==1;
}
struct Mapping {
    std::vector<double> angles, offsets, ratios;
};
inline bool build(const QJsonArray& points, cv::Point2f center, const cv::Mat& H,
                  bool clockwise, Mapping& result) {
    result = {};
    std::vector<cv::Point2f> transformed;
    for (const auto& value : points) {
        auto p=value.toObject(); transformed.emplace_back(p["x"].toDouble(),p["y"].toDouble());
        result.ratios.push_back(p["ratio"].toDouble());
    }
    if (transformed.size()<2) return false;
    if (!H.empty()) cv::perspectiveTransform(transformed,transformed,H);
    for (auto p : transformed) {
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || cv::norm(p-center)<2) return false;
        double a=std::atan2(p.y-center.y,p.x-center.x)*180/CV_PI;
        result.angles.push_back(std::fmod(a+360,360));
    }
    double previous=-1;
    for (double a : result.angles) {
        const double offset=span(result.angles.front(),a,clockwise);
        if (offset<=previous || (previous>=0 && offset-previous<0.1)) return false;
        result.offsets.push_back(offset); previous=offset;
    }
    return previous>=1 && previous<359;
}
inline double ratio(const Mapping& mapping, double offset) {
    offset=std::clamp(offset,0.0,mapping.offsets.back());
    for (size_t i=1;i<mapping.offsets.size();++i)
        if (offset<=mapping.offsets[i]) {
            const double t=(offset-mapping.offsets[i-1])/(mapping.offsets[i]-mapping.offsets[i-1]);
            return mapping.ratios[i-1]+t*(mapping.ratios[i]-mapping.ratios[i-1]);
        }
    return 1;
}
}
