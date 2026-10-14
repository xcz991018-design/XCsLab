#include "GaugeReaderEngine.hpp"
#include "RecognitionQuality.hpp"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <iostream>

int fail(int line) {
    std::cerr << "Mode regression failed at line " << line << std::endl;
    return EXIT_FAILURE;
}

bool sameReading(const GaugeResult& a, const GaugeResult& b) {
    return a.recognitionQuality == b.recognitionQuality && a.success == b.success && a.value == b.value && a.confidence == b.confidence &&
           a.pointer_angle_deg == b.pointer_angle_deg && a.center == b.center && a.radius == b.radius &&
           a.centerInconsistent == b.centerInconsistent && a.centerOffsetDist == b.centerOffsetDist &&
           a.errorMessage == b.errorMessage && a.warningMessage == b.warningMessage;
}

bool noDiagnosticImages(const GaugeResult& r) {
    return r.originalImage.empty() && r.rectifiedImage.empty() &&
           r.grayImage.empty() && r.overlayImage.empty() &&
           r.diagnosticHudImage.empty() && r.pointerDetectorResult.hudImage.empty() &&
           r.pointerDetectorResult.debugImage.empty();
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir output;
    if (!output.isValid() || !QDir::setCurrent(output.path())) return fail(__LINE__);
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
    profile.scalePoints=QJsonArray{QJsonObject{{"x",80},{"y",240},{"ratio",0}},QJsonObject{{"x",240},{"y",240},{"ratio",1}}};
    profile.pointerBaseRadius = 18;
    profile.pointerMinLength = 40;
    profile.coarseAngleStep = 1;
    profile.save_debug_images = true;
    GaugeReaderEngine engine;
    const cv::Mat original = frame.clone();
    {
        auto p = profile;
        auto production = engine.process(frame, p, GaugeRunMode::Production);
        if (!noDiagnosticImages(production) || !production.step1_original.empty() || !production.step4_recognition.empty() ||
            !production.step2_rectified.empty() || !production.step3_gray.empty()) return fail(__LINE__);
        if (!QDir(output.path()).entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot).empty()) return fail(__LINE__);
        p = profile;
        auto evidence = engine.process(frame, p, GaugeRunMode::Evidence);
        if (!noDiagnosticImages(evidence) || evidence.step1_original.empty() || evidence.step4_recognition.empty() ||
            evidence.step2_rectified.empty() || evidence.step3_gray.empty()) return fail(__LINE__);
        if (cv::norm(evidence.step2_rectified, frame, cv::NORM_INF) != 0) return fail(__LINE__);
        cv::Mat expectedGray, actualGray;
        cv::cvtColor(frame, expectedGray, cv::COLOR_BGR2GRAY);
        cv::cvtColor(evidence.step3_gray, actualGray, cv::COLOR_BGR2GRAY);
        if (cv::norm(expectedGray, actualGray, cv::NORM_INF) != 0) return fail(__LINE__);
        const auto diagnostic=QString::fromUtf8(evidence.detailedDebugInfo.data(),static_cast<int>(evidence.detailedDebugInfo.size()));
        if (!diagnostic.endsWith(qualitySummary(evidence.recognitionQuality)) || diagnostic.contains(QChar(0xfffd))) return fail(__LINE__);
        if (!evidence.success) return fail(__LINE__);
        // Green measurement strokes must be present alongside the axis overlay.
        cv::Mat widthStrokes;
        cv::inRange(evidence.step4_recognition, cv::Scalar(0, 240, 0), cv::Scalar(20, 255, 20), widthStrokes);
        if (cv::countNonZero(widthStrokes) < 10) return fail(__LINE__);
        for (const auto* key : {"perspective_ms", "grayscale_ms", "pointer_detection_ms", "quality_ms", "rendering_ms", "center_check_ms", "engine_total_ms"})
            if (!production.stageTimings[key].isDouble() || production.stageTimings[key].toDouble()<0 ||
                !evidence.stageTimings[key].isDouble() || evidence.stageTimings[key].toDouble()<0) return fail(__LINE__);
        if (production.stageTimings["engine_total_ms"].toDouble() < production.stageTimings["pointer_detection_ms"].toDouble()) return fail(__LINE__);
        if (!sameReading(production, evidence)) return fail(__LINE__);
        if (!QDir(output.path()).entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot).empty()) return fail(__LINE__);
        p = profile;
        p.save_debug_images = false;
        auto debug = engine.process(frame, p, GaugeRunMode::Debug);
        if (debug.diagnosticHudImage.empty() || cv::norm(debug.step3_gray, evidence.step3_gray, cv::NORM_INF) != 0) return fail(__LINE__);
        if (!sameReading(production, debug)) return fail(__LINE__);
        if (cv::norm(frame, original, cv::NORM_INF) != 0) return fail(__LINE__);
        std::cout << "Modes preserve reading and quality checks; production has no output images; evidence has four core images\n";
    }
    auto savedProfile = profile;
    savedProfile.save_debug_images = true;
    auto savedDebug = engine.process(frame, savedProfile, GaugeRunMode::Debug);
    const QStringList expectedFiles = {"diagnostic_hud.png", "gray.png", "original.png", "recognition.png", "rectified.png"};
    if (QDir("debug_images").entryList(QDir::Files, QDir::Name) != expectedFiles || QDir("debug_steps").exists()) {
        std::cerr << "Unexpected diagnostic files: " << QDir("debug_images").entryList(QDir::Files, QDir::Name).join(",").toStdString() << '\n';
        return fail(__LINE__);
    }
    if (cv::norm(cv::imread("debug_images/gray.png"), savedDebug.step3_gray, cv::NORM_INF) != 0) return fail(__LINE__);
    for (bool rectified : {false, true}) {
        savedProfile.perspective_pts = rectified ? std::vector<cv::Point2f>{{160,25},{295,160},{160,295},{25,160}} : std::vector<cv::Point2f>{};
        QFile destination("saved_profile.json");
        if (!destination.open(QIODevice::WriteOnly | QIODevice::Truncate)) return fail(__LINE__);
        destination.close();
        if (!savedProfile.saveToFile("saved_profile.json")) return fail(__LINE__);
        QFile saved("saved_profile.json");
        if (!saved.open(QIODevice::ReadOnly)) return fail(__LINE__);
        const auto json = QJsonDocument::fromJson(saved.readAll());
        if (json.isNull() || !json.isObject()) { std::cerr << "Saved profile is invalid JSON\n"; return fail(__LINE__); }
    }
    for (auto mode : {GaugeRunMode::Production, GaugeRunMode::Evidence, GaugeRunMode::Debug}) {
        auto p = profile;
        const auto result = engine.process(cv::Mat(), p, mode);
        if (result.success || result.errorMessage.empty() || result.runMode != mode) return fail(__LINE__);
    }
    return 0;
}
