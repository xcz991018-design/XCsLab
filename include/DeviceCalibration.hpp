#pragma once
#include <QJsonObject>
#include "ScaleCalibration.hpp"
#include <opencv2/opencv.hpp>
#include <cmath>

inline bool validDeviceCalibration(const QJsonObject& data) {
    if(data.contains("meter_range")) {
        const auto range=data["meter_range"].toObject();const auto lo=range["min_val"],hi=range["max_val"];
        if(!lo.isDouble()||!hi.isDouble()||!std::isfinite(lo.toDouble())||!std::isfinite(hi.toDouble())||lo.toDouble()>=hi.toDouble())return false;
    }
    const int width = data["calibrated_width"].toInt();
    const int height = data["calibrated_height"].toInt();
    if (width <= 0 || height <= 0 || !data["perspective_correction_enabled"].toBool()) return false;
    std::vector<cv::Point2f> points;
    for (int i = 0; i < 4; ++i) {
        const auto x = data[QString("pt_%1_x").arg(i)];
        const auto y = data[QString("pt_%1_y").arg(i)];
        if (!x.isDouble() || !y.isDouble() || !std::isfinite(x.toDouble()) || !std::isfinite(y.toDouble()) ||
            x.toDouble() < 0 || x.toDouble() >= width || y.toDouble() < 0 || y.toDouble() >= height) return false;
        points.emplace_back(static_cast<float>(x.toDouble()), static_cast<float>(y.toDouble()));
    }
    const double x = data["center_x"].toDouble(-1), y = data["center_y"].toDouble(-1);
    const double radius = data["pointer_base_radius"].toDouble(-1);
    if (!data["scale_points"].isArray() ||
        !ScaleCalibration::valid(data["scale_points"].toArray(), width, height, cv::Point2f(x,y))) return false;
    return std::isfinite(x) && std::isfinite(y) && x >= 0 && x < width && y >= 0 && y < height &&
           std::isfinite(radius) && radius > 0 && cv::isContourConvex(points) && std::abs(cv::contourArea(points)) > 1;
}

inline bool hasSavedDeviceCalibration(const QJsonObject& data) {
    return validDeviceCalibration(data) && data["calibration_confirmed"].toBool(false);
}
