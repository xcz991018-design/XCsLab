#include "GaugeReaderEngine.hpp"
#include "ScaleCalibration.hpp"
#include <iostream>

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    cv::Mat frame = cv::imread(argv[1]);
    if (frame.empty()) return 2;
    GaugeProfile profile;
    profile.perspective_correction_enabled = true;
    profile.auto_detect_roi = false;
    profile.perspective_pts = {{365,56},{595,190},{365,321},{140,190}};
    profile.center = {365,232};
    profile.pointerBaseRadius = 47;
    profile.pointer_invert_direction = false;
    profile.tail_max_radius = 240;
    profile.tail_max_width = 130;
    profile.tail_corridor_half_width = 90;
    GaugeReaderEngine engine;
    cv::Mat calibrationInverse;
    engine.rectifyPerspective(frame,profile.perspective_pts,calibrationInverse);
    std::vector<cv::Point2f> center{profile.center}; cv::perspectiveTransform(center,center,calibrationInverse.inv());
    QJsonArray rectifiedPoints{QJsonObject{{"x",center[0].x-140},{"y",center[0].y+140},{"ratio",0}},
        QJsonObject{{"x",center[0].x+140},{"y",center[0].y+140},{"ratio",1}}};
    profile.scalePoints=ScaleCalibration::transformed(rectifiedPoints,calibrationInverse);
    const auto result = engine.process(frame, profile, GaugeRunMode::Evidence);
    const auto& tail = result.pointerDetectorResult.tailProfile;
    // Independently traced silhouette in the source photograph; tolerance
    // allows the blurred rim and perspective interpolation, not interior edges.
    std::vector<cv::Point2f> outline = {{365,218},{375,211},{385,203},{395,193},
        {399,185},{406,177},{415,175},{425,179},{435,185},{444,193},
        {451,202},{450,214},{440,218},{420,221},{399,226},{381,234},{371,234}};
    std::vector<cv::Point2f> destination = {{400,40},{760,400},{400,760},{40,400}};
    cv::Mat inverse = cv::getPerspectiveTransform(profile.perspective_pts, destination).inv();
    int checked = 0;
    double maxError = 0;
    for (const auto& section : tail.sections) {
        if (!section.valid) continue;
        std::vector<cv::Point2f> points = {cv::Point2f(section.leftPoint),cv::Point2f(section.rightPoint)};
        cv::perspectiveTransform(points, points, inverse);
        for (const auto& point : points) {
            double error = std::abs(cv::pointPolygonTest(outline, point, true));
            if (error > 3.5) std::cout << "outline mismatch r=" << section.radius << " point=" << point << " error=" << error << '\n';
            maxError = std::max(maxError, error);
            ++checked;
        }
    }
    if (argc > 2) {
        cv::imwrite(std::string(argv[2]) + "/after.png", result.step4_recognition);
        cv::imwrite(std::string(argv[2]) + "/tail_detail.png", result.step4_recognition(cv::Rect(385,370,170,150)));
    }
    std::cout << "tail=" << result.pointerDetectorResult.tailDetected << " endpoints=" << checked
              << " max_source_outline_error=" << maxError << " px\n";
    const double coverage = tail.sections.empty() ? 0.0 : tail.sections.back().radius - tail.sections.front().radius;
    return result.success && result.pointerDetectorResult.tailDetected && checked >= 30 && coverage >= 60.0 && maxError <= 3.5 ? 0 : 1;
}
