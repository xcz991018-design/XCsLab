#include "GaugeReaderEngine.hpp"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QDir>
#include <iostream>

bool sameReading(const GaugeResult& a, const GaugeResult& b) {
    return a.success == b.success && a.value == b.value && a.confidence == b.confidence &&
           a.pointer_angle_deg == b.pointer_angle_deg && a.center == b.center && a.radius == b.radius &&
           a.centerInconsistent == b.centerInconsistent && a.centerOffsetDist == b.centerOffsetDist &&
           a.errorMessage == b.errorMessage && a.warningMessage == b.warningMessage;
}

bool noDiagnosticImages(const GaugeResult& r) {
    return r.originalImage.empty() && r.rectifiedImage.empty() &&
           r.grayImage.empty() && r.blurredImage.empty() &&
           r.hubRoiImage.empty() && r.needleBinaryImage.empty() && r.overlayImage.empty() &&
           r.diagnosticHudImage.empty() && r.pointerDetectorResult.hudImage.empty() &&
           r.pointerDetectorResult.debugImage.empty() && r.intermediateSteps.empty();
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir output;
    if (!output.isValid() || !QDir::setCurrent(output.path())) return 1;
    cv::setNumThreads(1);
    cv::Mat frame(320, 320, CV_8UC3, cv::Scalar(245, 245, 245));
    cv::circle(frame, cv::Point(160, 160), 135, cv::Scalar(0, 0, 0), 2);
    cv::line(frame, cv::Point(160, 160), cv::Point(250, 100), cv::Scalar(0, 0, 0), 7);
    cv::circle(frame, cv::Point(160, 160), 12, cv::Scalar(0, 0, 0), -1);
    GaugeProfile profile;
    profile.perspective_correction_enabled = false;
    profile.auto_detect_roi = false;
    profile.center = cv::Point2f(160, 160);
    profile.radius = 135;
    profile.pointerBaseRadius = 18;
    profile.pointerMinLength = 40;
    profile.coarseAngleStep = 1;
    profile.save_debug_images = true;
    GaugeReaderEngine engine;
    const cv::Mat original = frame.clone();
    for (bool preprocessing : {false, true}) {
        profile.enable_steps_4_to_9 = preprocessing;
        auto p = profile;
        auto production = engine.process(frame, p, GaugeRunMode::Production);
        if (!noDiagnosticImages(production) || !production.step1_original.empty() || !production.step4_recognition.empty() ||
            !production.step2_rectified.empty() || !production.step3_preprocessed.empty()) return 1;
        if (!QDir(output.path()).entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot).empty()) return 1;
        p = profile;
        auto evidence = engine.process(frame, p, GaugeRunMode::Evidence);
        if (!noDiagnosticImages(evidence) || evidence.step1_original.empty() || evidence.step4_recognition.empty() ||
            evidence.step2_rectified.empty() || evidence.step3_preprocessed.empty()) return 1;
        if (cv::norm(evidence.step2_rectified, frame, cv::NORM_INF) != 0) return 1;
        cv::Mat expectedGray, actualGray;
        cv::cvtColor(frame, expectedGray, cv::COLOR_BGR2GRAY);
        cv::cvtColor(evidence.step3_preprocessed, actualGray, cv::COLOR_BGR2GRAY);
        if (cv::norm(expectedGray, actualGray, cv::NORM_INF) != 0) return 1;
        if (!evidence.success) return 1;
        // Green measurement strokes must be present alongside the axis overlay.
        cv::Mat widthStrokes;
        cv::inRange(evidence.step4_recognition, cv::Scalar(0, 240, 0), cv::Scalar(20, 255, 20), widthStrokes);
        if (cv::countNonZero(widthStrokes) < 10) return 1;
        if (!sameReading(production, evidence)) return 1;
        if (!QDir(output.path()).entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot).empty()) return 1;
        p = profile;
        p.save_debug_images = false;
        auto debug = engine.process(frame, p, GaugeRunMode::Debug);
        if (debug.intermediateSteps.size() != (preprocessing ? 12 : 6) || debug.diagnosticHudImage.empty()) return 1;
        if (!sameReading(production, debug)) return 1;
        if (cv::norm(frame, original, cv::NORM_INF) != 0) return 1;
        std::cout << "Modes preserve reading and quality checks; production has no output images; evidence has four core images\n";
    }
    for (auto mode : {GaugeRunMode::Production, GaugeRunMode::Evidence, GaugeRunMode::Debug}) {
        auto p = profile;
        const auto result = engine.process(cv::Mat(), p, mode);
        if (result.success || result.errorMessage.empty() || result.runMode != mode) return 1;
    }
    return 0;
}
