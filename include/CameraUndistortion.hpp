#pragma once
#include <opencv2/opencv.hpp>
#include <QJsonArray>
#include <QJsonObject>

// Calibration for the installed lens at 2304 x 1296. Keep the same K and
// image extent after undistortion: no crop or additional change of coordinates.
// Persisted device annotations remain in RAW image pixels for compatibility.
namespace CameraUndistortion {
inline cv::Mat matrix(cv::Size size) {
    const double sx = double(size.width) / 2304;
    const double sy = double(size.height) / 1296;
    return (cv::Mat_<double>(3,3) << 1000.25230*sx, 0, 1168.05784*sx,
        0, 1000.48207*sy, 660.617147*sy, 0, 0, 1);
}
inline cv::Mat coefficients() {
    return (cv::Mat_<double>(1,5) << -0.303680902, 0.096542819,
        -0.000133464040, 0.0000526209523, -0.0141173611);
}
inline cv::Mat image(const cv::Mat& raw) {
    if (raw.empty()) return {};
    cv::Mat corrected;
    const auto K = matrix(raw.size());
    cv::undistort(raw, corrected, K, coefficients(), K);
    return corrected;
}
inline std::vector<cv::Point2f> points(const std::vector<cv::Point2f>& input,
                                      cv::Size size, bool toRaw = false) {
    if (input.empty()) return {};
    const auto K = matrix(size);
    std::vector<cv::Point2f> output;
    if (!toRaw) {
        cv::undistortPoints(input, output, K, coefficients(), cv::noArray(), K,
            cv::TermCriteria(cv::TermCriteria::COUNT | cv::TermCriteria::EPS, 50, 1e-9));
    } else {
        std::vector<cv::Point3f> rays;
        for (const auto& p : input)
            rays.emplace_back(float((p.x-K.at<double>(0,2))/K.at<double>(0,0)),
                float((p.y-K.at<double>(1,2))/K.at<double>(1,1)), 1.f);
        cv::projectPoints(rays, cv::Vec3d(0,0,0), cv::Vec3d(0,0,0), K, coefficients(), output);
    }
    return output;
}
inline cv::Point2f point(cv::Point2f p, cv::Size size, bool toRaw = false) {
    if (p.x < 0 || p.y < 0) return p; // Preserve the unset-center sentinel.
    return points({p}, size, toRaw).front();
}
inline QJsonArray scales(const QJsonArray& input, cv::Size size, bool toRaw = false) {
    std::vector<cv::Point2f> positions;
    for (const auto& value : input) {
        const auto p = value.toObject();
        positions.emplace_back(float(p["x"].toDouble()), float(p["y"].toDouble()));
    }
    const auto mapped = points(positions, size, toRaw);
    QJsonArray output;
    for (int i = 0; i < input.size(); ++i) {
        auto p = input[i].toObject();
        p["x"] = mapped[i].x; p["y"] = mapped[i].y;
        if(p.contains("origin_x")) {
            const auto o=point({float(p["origin_x"].toDouble()),float(p["origin_y"].toDouble())},size,toRaw);
            p["origin_x"]=o.x;p["origin_y"]=o.y;
        }
        output.append(p);
    }
    return output;
}
}
