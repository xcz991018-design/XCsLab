#include "GaugeReaderEngine.hpp"
#include "ScaleCalibration.hpp"
#include "AutoDialCalibration.hpp"
#include "RecognitionQuality.hpp"
#include <cmath>
#include <iostream>
#include <numeric>
#include <algorithm>
#include <filesystem>
#include <chrono>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

double GaugeReaderEngine::normalizeAngle(double angleDeg) {
    double a = std::fmod(angleDeg, 360.0);
    if (a < 0) a += 360.0;
    return a;
}



bool GaugeReaderEngine::locateDialCircle(const cv::Mat& gray, cv::Point2f& center, float& radius,
    bool* hubDetected, const cv::Point2f* expectedHub) {
    if (hubDetected) *hubDetected = false;
    cv::Mat blurred;
    cv::GaussianBlur(gray, blurred, cv::Size(9, 9), 2.0);

    std::vector<cv::Vec3f> circles;
    int minR = std::min(gray.cols, gray.rows) / 6;
    int maxR = std::min(gray.cols, gray.rows) / 2;

    cv::HoughCircles(blurred, circles, cv::HOUGH_GRADIENT, 1.2, gray.rows / 4, 100, 40, minR, maxR);

    cv::Point2f bestCenter(gray.cols / 2.0f, gray.rows / 2.0f);
    float bestRadius = std::min(gray.cols, gray.rows) * 0.38f;
    bool found = false;

    if (!circles.empty()) {
        cv::Point2f imgCenter(gray.cols / 2.0f, gray.rows / 2.0f);
        float minDist = 1e9f;
        int bestIdx = 0;

        for (size_t i = 0; i < circles.size(); ++i) {
            cv::Point2f c(circles[i][0], circles[i][1]);
            float dist = cv::norm(c - imgCenter);
            if (dist < minDist) {
                minDist = dist;
                bestIdx = static_cast<int>(i);
            }
        }

        bestCenter = cv::Point2f(circles[bestIdx][0], circles[bestIdx][1]);
        bestRadius = circles[bestIdx][2];
        found = true;
    }

    // Refine center by finding the needle rotation axis (central hub pin/rivet cap)
    const auto hubAnchor = expectedHub ? *expectedHub : bestCenter;
    int roiR = static_cast<int>(bestRadius * 0.25f);
    int x1 = std::max(0, static_cast<int>(hubAnchor.x) - roiR);
    int y1 = std::max(0, static_cast<int>(hubAnchor.y) - roiR);
    int x2 = std::min(gray.cols, static_cast<int>(hubAnchor.x) + roiR);
    int y2 = std::min(gray.rows, static_cast<int>(hubAnchor.y) + roiR);

    cv::Point2f refinedCenter = bestCenter;

    if (x2 > x1 && y2 > y1) {
        cv::Mat roi = gray(cv::Rect(x1, y1, x2 - x1, y2 - y1));
        cv::Mat roiBlur;
        cv::GaussianBlur(roi, roiBlur, cv::Size(5, 5), 1.0);

        std::vector<cv::Vec3f> pinCircles;
        int pinMinR = std::max(8, static_cast<int>(bestRadius * 0.03f));
        int pinMaxR = std::max(20, static_cast<int>(bestRadius * 0.09f));

        cv::HoughCircles(roiBlur, pinCircles, cv::HOUGH_GRADIENT, 1.0, 20, 50, 20, pinMinR, pinMaxR);

        if (!pinCircles.empty()) {
            cv::Mat gradX,gradY;
            cv::Scharr(roi,gradX,CV_32F,1,0); cv::Scharr(roi,gradY,CV_32F,0,1);
            double bestScore=-1, bestSupport=0;
            cv::Vec3f bestPin=pinCircles.front();
            for (const auto& pin : pinCircles) {
                int positive=0,negative=0;
                for (int angle=0;angle<360;angle+=10) {
                    const double nx=std::cos(angle*M_PI/180), ny=std::sin(angle*M_PI/180);
                    bool pos=false,neg=false;
                    for (int offset=-2;offset<=2;++offset) {
                        const int x=cvRound(pin[0]+(pin[2]+offset)*nx), y=cvRound(pin[1]+(pin[2]+offset)*ny);
                        if(x<0||y<0||x>=roi.cols||y>=roi.rows) continue;
                        const double gx=gradX.at<float>(y,x), gy=gradY.at<float>(y,x);
                        const double magnitude=std::hypot(gx,gy), normal=gx*nx+gy*ny;
                        pos |= magnitude>=60 && normal>=0.7*magnitude;
                        neg |= magnitude>=60 && normal<=-0.7*magnitude;
                    }
                    positive+=pos; negative+=neg;
                }
                const double support=double(std::max(positive,negative))/36;
                const double score=support-0.005*cv::norm(cv::Point2f(x1+pin[0],y1+pin[1])-hubAnchor);
                if(score>bestScore) {bestScore=score;bestPin=pin;bestSupport=support;}
            }
            // Text arcs and cast-shadow intersections lack a coherent circular rim.
            if(bestSupport>=0.75) {
                refinedCenter=cv::Point2f(x1+bestPin[0],y1+bestPin[1]);
                if(hubDetected) *hubDetected=true;
            }
        }
    }

    center = refinedCenter;
    radius = bestRadius;

    return found;
}

std::vector<cv::Point2f> GaugeReaderEngine::autoDetectCardinalPoints(const cv::Mat& image) {
    const auto result=AutoDialCalibration::detect(image);
    return result.valid()?result.points:std::vector<cv::Point2f>{};
}

cv::Mat GaugeReaderEngine::rectifyPerspective(const cv::Mat& src, const std::vector<cv::Point2f>& quadPts, cv::Mat& invH, int outSize, int margin) {
    if (quadPts.size() != 4 || src.empty()) return src.clone();

    // quadPts are explicitly defined in order [0: 12H Top, 1: 3H Right, 2: 6H Bottom, 3: 9H Left]
    std::vector<cv::Point2f> srcPts = quadPts;

    // Target square mapping matching fov_corrector.py cardinal_transform
    float S = static_cast<float>(outSize);
    float m = static_cast<float>(margin);

    std::vector<cv::Point2f> dstPts(4);
    dstPts[0] = cv::Point2f(S / 2.0f, m);       // 12H Top
    dstPts[1] = cv::Point2f(S - m, S / 2.0f);   // 3H Right
    dstPts[2] = cv::Point2f(S / 2.0f, S - m);   // 6H Bottom
    dstPts[3] = cv::Point2f(m, S / 2.0f);       // 9H Left

    cv::Mat H = cv::getPerspectiveTransform(srcPts, dstPts);
    invH = H.inv();

    cv::Mat rectified;
    cv::warpPerspective(src, rectified, H, cv::Size(outSize, outSize), cv::INTER_CUBIC);
    return rectified;
}

GaugeResult GaugeReaderEngine::process(const cv::Mat& frame, GaugeProfile& profile, GaugeRunMode mode) {
    GaugeResult res;
    using Clock = std::chrono::steady_clock;
    const auto engineStarted = Clock::now();
    auto lastStage = engineStarted;
    auto mark = [&](const char* name) {
        const auto now = Clock::now();
        res.stageTimings[name] = std::chrono::duration<double, std::milli>(now-lastStage).count();
        lastStage = now;
    };
    auto finish = [&]() { res.stageTimings["engine_total_ms"] = std::chrono::duration<double, std::milli>(Clock::now()-engineStarted).count(); };
    res.runMode = mode;
    const bool debug = mode == GaugeRunMode::Debug;
    const bool evidence = mode != GaugeRunMode::Production;
    res.unit = profile.unit;

    if (frame.empty()) {
        res.errorStep = "步骤 1: 原始采集图像 (Original Stream Frame)";
        res.errorMessage = "输入图像帧为空 (frame is empty)";
        res.detailedDebugInfo = "❌ [Pipeline Error] 摄像头或输入图像源未采集到有效图像帧 (cv::Mat::empty)。";
        finish(); return res;
    }

    // Step 1: 原图 (Original Frame)
    if (evidence) res.step1_original = frame.clone();
    if (debug) res.originalImage = res.step1_original;

    if (profile.scalePoints.isEmpty()) {
        res.errorStep="Scale calibration";
        res.errorMessage="未完成手动刻度标定，请重新标定后识别";
        finish(); return res;
    }

    mark("input_copy_ms");
    // Step 2: 视场矫正 (FOV Perspective Correction)
    cv::Mat workFrame = frame;
    cv::Mat invH;

    if (profile.perspective_correction_enabled) {
        std::vector<cv::Point2f> pts = profile.perspective_pts;

        // Auto-scale points if current frame resolution differs from calibration frame resolution
        if (pts.size() == 4 && profile.calibrated_width > 0 && profile.calibrated_height > 0 &&
            (frame.cols != profile.calibrated_width || frame.rows != profile.calibrated_height)) {
            float sx = static_cast<float>(frame.cols) / profile.calibrated_width;
            float sy = static_cast<float>(frame.rows) / profile.calibrated_height;
            for (auto& p : pts) {
                p.x *= sx;
                p.y *= sy;
            }
            std::cout << "[GaugeReaderEngine] Rescaled 4 cardinal points from " 
                      << profile.calibrated_width << "x" << profile.calibrated_height 
                      << " to " << frame.cols << "x" << frame.rows << std::endl;
        }

        bool ptsValid = (pts.size() == 4);
        if (ptsValid) {
            for (const auto& p : pts) {
                if (p.x < 0 || p.x > frame.cols || p.y < 0 || p.y > frame.rows) {
                    ptsValid = false;
                    break;
                }
            }
        }

        if (!ptsValid || (profile.auto_detect_roi && pts.empty())) {
            std::cout << "[GaugeReaderEngine] Specified 4 cardinal points missing/out of bounds. Auto-detecting 4 cardinal points..." << std::endl;
            pts = autoDetectCardinalPoints(frame);
            if (pts.size() == 4) {
                profile.perspective_pts = pts;
            }
        }

        if (pts.size() == 4) {
            workFrame = rectifyPerspective(frame, pts, invH, profile.rectified_size, profile.rectified_margin);
        } else {
            workFrame = frame;
        }
    } else {
        workFrame = frame;
    }

    mark("perspective_ms");
    if (evidence) res.step2_rectified = workFrame.clone();
    if (debug) res.rectifiedImage = workFrame;

    mark("rectified_copy_ms");
    // Grayscale is the direct input to pointer detection.
    cv::Mat gray;
    if (workFrame.channels() == 3) {
        cv::cvtColor(workFrame, gray, cv::COLOR_BGR2GRAY);
    } else {
        gray = workFrame;
    }
    if (debug) res.grayImage = gray;
    mark("grayscale_ms");
    // The core stages also need the actual detector input in evidence mode.
    if (evidence) cv::cvtColor(gray, res.step3_gray, cv::COLOR_GRAY2BGR);

    mark("gray_preview_ms");
    // Step 4: 识别 (Dial Center, Pointer Angle, Value & Overlay Result)
    if (profile.perspective_correction_enabled && !profile.auto_detect_roi && profile.center.x > 0 && profile.center.y > 0 && !invH.empty()) {
        float sx = (profile.calibrated_width > 0) ? (static_cast<float>(frame.cols) / profile.calibrated_width) : 1.0f;
        float sy = (profile.calibrated_height > 0) ? (static_cast<float>(frame.rows) / profile.calibrated_height) : 1.0f;
        cv::Point2f scaledCenter(profile.center.x * sx, profile.center.y * sy);

        cv::Mat H = invH.inv();
        std::vector<cv::Point2f> srcC = { scaledCenter };
        std::vector<cv::Point2f> dstC;
        cv::perspectiveTransform(srcC, dstC, H);

        if (dstC[0].x >= 20.0f && dstC[0].x <= gray.cols - 20.0f &&
            dstC[0].y >= 20.0f && dstC[0].y <= gray.rows - 20.0f) {
            res.center = dstC[0];
        } else {
            res.center = cv::Point2f(gray.cols * 0.5f, gray.rows * 0.5f);
        }
        res.radius = (gray.cols * 0.5f) - profile.rectified_margin;
    } else if (profile.perspective_correction_enabled || profile.auto_detect_roi) {
        res.center = cv::Point2f(gray.cols * 0.5f, gray.rows * 0.5f);
        res.radius = (gray.cols * 0.5f) - profile.rectified_margin;
    } else {
        if (profile.center.x > 0 && profile.center.y > 0 && profile.center.x < gray.cols && profile.center.y < gray.rows && profile.radius > 0) {
            res.center = profile.center;
            res.radius = profile.radius;
        } else {
            locateDialCircle(gray, res.center, res.radius);
        }
    }

    // A displaced manual hub can force the radial search onto a cast shadow.
    // Require a detected pin/rivet (not a dial-circle fallback) before rejecting.
    cv::Point2f checkedCenter;
    float checkedRadius = 0;
    bool confirmedHub = false;
    locateDialCircle(gray, checkedCenter, checkedRadius, &confirmedHub, &res.center);
    const double checkedOffset = cv::norm(checkedCenter - res.center);
    const double centerTolerance = profile.center_search_tolerance > 0 ? profile.center_search_tolerance : 35;
    if (!profile.auto_detect_roi && confirmedHub && checkedOffset > centerTolerance) {
        res.centerInconsistent = true;
        res.centerOffsetDist = checkedOffset;
        res.errorStep = "轴心标定检查";
        res.errorMessage = "轴心标定与实际轴帽中心不一致（偏差 " + std::to_string(checkedOffset) +
            " px），请重新完成视场切点标定中的中心点；未输出识别读数";
        res.warningMessage = res.errorMessage;
        if (evidence) {
            res.step4_recognition = workFrame.clone();
            cv::circle(res.step4_recognition, res.center, 8, cv::Scalar(0,165,255), 2);
            cv::circle(res.step4_recognition, checkedCenter, 8, cv::Scalar(0,255,0), 2);
            cv::line(res.step4_recognition, res.center, checkedCenter, cv::Scalar(0,165,255), 2);
            cv::putText(res.step4_recognition, "CALIBRATION CENTER MISMATCH - NO READING", {15,30},
                cv::FONT_HERSHEY_SIMPLEX, 0.55, cv::Scalar(0,165,255), 2);
        }
        finish(); return res;
    }

    // Transform all device scale points with the same homography as the hub.
    ScaleCalibration::Mapping scaleMapping;
    double minAngle=0, maxAngle=0;
    {
        const double sx = profile.calibrated_width>0 ? double(frame.cols)/profile.calibrated_width : 1;
        const double sy = profile.calibrated_height>0 ? double(frame.rows)/profile.calibrated_height : 1;
        const auto points = ScaleCalibration::scaled(profile.scalePoints,sx,sy);
        if (profile.perspective_correction_enabled && invH.empty()) {
            res.errorMessage="刻度标定无法应用：透视校正失败"; finish(); return res;
        }
        std::vector<cv::Point2f> hub{{float(profile.center.x*sx),float(profile.center.y*sy)}};
        if (!invH.empty()) cv::perspectiveTransform(hub,hub,invH.inv());
        if (!std::isfinite(hub[0].x) || !std::isfinite(hub[0].y) || hub[0].x<0 || hub[0].y<0 || hub[0].x>=gray.cols || hub[0].y>=gray.rows) {
            res.errorMessage="刻度标定无法应用：轴心不在校正图像内"; finish(); return res;
        }
        res.center=hub[0];
        if (!ScaleCalibration::valid(points,frame.cols,frame.rows,{float(profile.center.x*sx),float(profile.center.y*sy)}) ||
            !ScaleCalibration::build(points,res.center,invH.empty()?cv::Mat():invH.inv(),profile.clockwise,scaleMapping)) {
            res.errorStep = "Scale calibration";
            res.errorMessage = "刻度标定无效：请检查端点、中间点顺序和数值增长方向";
            finish(); return res;
        }
        minAngle=scaleMapping.angles.front(); maxAngle=scaleMapping.angles.back();
    }
    // Configure PointerDetector
    GaugeConfig cfg;
    cfg.center = res.center;
    cfg.radius = res.radius;
    cfg.pointerMinAngle = minAngle;
    cfg.pointerMaxAngle = maxAngle;
    cfg.clockwise = profile.clockwise;
    cfg.pointerBaseRadius = profile.pointerBaseRadius;
    cfg.pointerMinWidth = profile.pointerMinWidth;
    cfg.pointerMaxWidth = profile.pointerMaxWidth;
    cfg.pointerMinLength = profile.pointerMinLength;
    cfg.coarseAngleStep = profile.coarseAngleStep;
    cfg.fineAngleStep = profile.fineAngleStep;
    cfg.fineAngleRange = profile.fineAngleRange;
    cfg.minValidSectionRatio = profile.minValidSectionRatio;
    cfg.maxAllowedWidthStep = profile.maxAllowedWidthStep;
    cfg.maxCenterlineOffset = profile.maxCenterlineOffset;

    // Forward anti-interference & text occlusion parameters from profile JSON
    cfg.maxConsecutiveGapSamples = profile.maxConsecutiveGapSamples;
    cfg.minEdgeSupportDensity = profile.minEdgeSupportDensity;
    cfg.rawGradientThreshold = profile.rawGradientThreshold;
    cfg.minGradientNormalAlignment = profile.minGradientNormalAlignment;
    cfg.minLocalStepContrast = profile.minLocalStepContrast;
    cfg.maxStartBaseGapPx = profile.maxStartBaseGapPx;
    cfg.maxParallelAngleDiffDeg = profile.maxParallelAngleDiffDeg;
    cfg.minCommonLengthPx = profile.minCommonLengthPx;
    cfg.maxLineFitResidualPx = profile.maxLineFitResidualPx;
    cfg.maxOcclusionGap = profile.maxOcclusionGap;
    cfg.corridorHalfWidthPx = profile.corridorHalfWidthPx;

    // Forward tail detection parameters
    cfg.enableTailDetection = profile.enable_tail_detection;
    cfg.tailMaxRadius = profile.tail_max_radius;
    cfg.tailMinWidth = profile.tail_min_width;
    cfg.tailMaxWidth = profile.tail_max_width;
    cfg.tailCorridorHalfWidth = profile.tail_corridor_half_width;
    cfg.tailMinGradientNormalAlignment = profile.tail_min_gradient_normal_alignment;

    // Forward optical polarity & dark entity constraints
    cfg.requireDarkPointer = profile.require_dark_pointer;
    cfg.minPointerDarkContrast = profile.min_pointer_dark_contrast;
    cfg.maxPointerInsideGray = profile.max_pointer_inside_gray;

    // Forward pointer scoring weights
    cfg.geomWeight = profile.w_geom_weight;
    cfg.wEdgePairCoherence = profile.w_edge_pair_coherence;
    cfg.wCenterStability = profile.w_center_stability;
    cfg.wAxisCenter = profile.w_axis_center;
    cfg.wOrientation = profile.w_orientation;
    cfg.wReach = profile.w_reach;
    cfg.wValidLength = profile.w_valid_length;
    cfg.wSlenderness = profile.w_slenderness;
    cfg.wPersistence = profile.w_persistence;
    cfg.wWidthTrend = profile.w_width_trend;
    cfg.wWidthContinuity = profile.w_width_continuity;
    cfg.wEdgeStrength = profile.w_edge_strength;
    cfg.wGapPenalty = profile.w_gap_penalty;
    cfg.wTrackingJumpPenalty = profile.w_tracking_jump_penalty;
    cfg.wTailScore = profile.w_tail_score;
    cfg.wTailPolarityBoost = profile.w_tail_polarity_boost;
    cfg.confidenceDivisor = profile.confidence_divisor;

    PointerDetector detector(cfg);

    mark("calibration_setup_ms");
    // Detection is identical across modes. Visualizations are optional outputs.
    cv::Mat rectOverlay;
    if (evidence) rectOverlay = workFrame.clone();
    mark("overlay_copy_ms");
    res.pointerDetectorResult = detector.detect(gray, cv::Point2d(res.center.x, res.center.y),
                                                debug ? &rectOverlay : nullptr);
    mark("pointer_detection_ms");
    res.stageTimings["pointer_details"] = res.pointerDetectorResult.stageTimings;
    if (debug) {
        res.diagnosticHudImage = res.pointerDetectorResult.hudImage;
        res.diagnosticText = res.pointerDetectorResult.hudText;
    }

    res.pointer_angle_deg = res.pointerDetectorResult.angle;

    double minA = normalizeAngle(minAngle);
    double maxA = normalizeAngle(maxAngle);
    double ptrA = res.pointer_angle_deg;

    double totalSpanDeg = profile.clockwise ? std::fmod(maxA - minA + 360.0, 360.0)
                                            : std::fmod(minA - maxA + 360.0, 360.0);

    double ptrSpanDeg = profile.clockwise ? std::fmod(ptrA - minA + 360.0, 360.0)
                                          : std::fmod(minA - ptrA + 360.0, 360.0);
    if (ptrSpanDeg > totalSpanDeg && ptrSpanDeg > (360.0 - 25.0)) ptrSpanDeg = 0.0;

    const double fraction = ScaleCalibration::ratio(scaleMapping, ptrSpanDeg);
    res.value = profile.min_val + fraction * (profile.max_val - profile.min_val);
    res.confidence = res.pointerDetectorResult.confidence;
    res.success = res.pointerDetectorResult.valid;
    mark("value_mapping_ms");
    res.recognitionQuality = evaluateRecognitionQuality(res.pointerDetectorResult, profile.qualityConfig);

    mark("quality_ms");
    if (!res.success) {
        res.errorStep = "灰度图指针检测 (Grayscale Pointer Detection)";
        res.errorMessage = res.pointerDetectorResult.failureReason.empty() ?
            "指针特征检测未通过 (置信度不足或有效截面缺失)" : res.pointerDetectorResult.failureReason;
        res.detailedDebugInfo = res.pointerDetectorResult.detailedDebugText;
    } else {
        res.detailedDebugInfo = res.pointerDetectorResult.detailedDebugText;
        double maxSpanAllowed = totalSpanDeg + 15.0;
        if (ptrSpanDeg > maxSpanAllowed && ptrSpanDeg < (360.0 - 15.0)) {
            char rangeWarn[256];
            snprintf(rangeWarn, sizeof(rangeWarn), "⚠️ [量程警告] 检测结果超出标定量程，请复核图像及标定配置。");
            if (!res.centerInconsistent) {
                res.warningMessage = rangeWarn;
            }
        }
    }

    res.detailedDebugInfo += "\n" + qualitySummary(res.recognitionQuality).toUtf8().toStdString();
    // Use the detector endpoint for both reading geometry and the result overlay.
    res.pointer_tip = cv::Point2f(res.pointerDetectorResult.tipPoint);

    mark("result_diagnostics_ms");
    if (debug) {
        res.overlayImage = frame.clone();
        drawOverlay(res.overlayImage, profile, res, invH, minAngle, maxAngle);
    }
    if (evidence) {
        cv::Mat emptyInvH;
        drawOverlay(rectOverlay, profile, res, emptyInvH, minAngle, maxAngle);
        res.step4_recognition = rectOverlay;
    }

    mark("rendering_ms");
    // Check calibration against the independently located hub.
    cv::Point2f autoCenter;
    float autoRadius{0.0f};
    autoCenter = checkedCenter;
    autoRadius = checkedRadius;

    cv::Point2f expectedCenter = res.center;
    if (!profile.auto_detect_roi && profile.center.x > 0 && profile.center.y > 0 && !invH.empty()) {
        std::vector<cv::Point2f> srcC = { {profile.center.x * (profile.calibrated_width>0?float(frame.cols)/profile.calibrated_width:1), profile.center.y * (profile.calibrated_height>0?float(frame.rows)/profile.calibrated_height:1)} };
        std::vector<cv::Point2f> dstC;
        cv::perspectiveTransform(srcC, dstC, invH.inv());
        expectedCenter = dstC[0];
    }

    double centerOffsetDist = cv::norm(autoCenter - expectedCenter);
    double tolerance = (profile.center_search_tolerance > 0.0f) ? profile.center_search_tolerance : 35.0;

    if (centerOffsetDist > tolerance) {
        res.centerInconsistent = true;
        res.centerOffsetDist = centerOffsetDist;
        char warnBuf[256];
        snprintf(warnBuf, sizeof(warnBuf), "⚠️ [防错报警] 自动检测轴心 (%.1f, %.1f) 与标定中心 (%.1f, %.1f) 偏差 %.1f px > 容忍极值 %.1f px！",
                 autoCenter.x, autoCenter.y, expectedCenter.x, expectedCenter.y, centerOffsetDist, tolerance);
        res.warningMessage = warnBuf;

    }

    mark("center_check_ms");
    if (debug && profile.save_debug_images) {
        std::filesystem::create_directories("debug_images");
        cv::imwrite("debug_images/original.png", res.step1_original);
        cv::imwrite("debug_images/rectified.png", res.step2_rectified);
        cv::imwrite("debug_images/gray.png", res.step3_gray);
        cv::imwrite("debug_images/recognition.png", res.step4_recognition);
        if (!res.diagnosticHudImage.empty()) cv::imwrite("debug_images/diagnostic_hud.png", res.diagnosticHudImage);
    }
    mark("debug_save_ms");
    finish();
    return res;
}

void GaugeReaderEngine::drawOverlay(cv::Mat& canvas, const GaugeProfile& profile, const GaugeResult& res, const cv::Mat& invH, double minAngle, double maxAngle) {
    if (!res.success) {
        // Draw visual error diagnostic banner on canvas
        int boxW = std::min(canvas.cols - 20, 580);
        int boxH = 145;
        int boxX = 10;
        int boxY = 10;

        cv::rectangle(canvas, cv::Rect(boxX, boxY, boxW, boxH), cv::Scalar(25, 20, 30), -1);
        cv::rectangle(canvas, cv::Rect(boxX, boxY, boxW, boxH), cv::Scalar(0, 0, 235), 2);

        int textY = boxY + 26;
        char buf[256];
        snprintf(buf, sizeof(buf), "[RECOGNITION FAILED / 仪表识别失败] %s", profile.gauge_id.c_str());
        cv::putText(canvas, buf, cv::Point(boxX + 12, textY), cv::FONT_HERSHEY_SIMPLEX, 0.55, cv::Scalar(0, 80, 255), 2, cv::LINE_AA);

        textY += 28;
        snprintf(buf, sizeof(buf), "Error Step: %s", res.errorStep.empty() ? "Pointer Detection" : res.errorStep.c_str());
        cv::putText(canvas, buf, cv::Point(boxX + 12, textY), cv::FONT_HERSHEY_SIMPLEX, 0.44, cv::Scalar(220, 220, 220), 1, cv::LINE_AA);

        textY += 24;
        snprintf(buf, sizeof(buf), "Reason: %s", res.errorMessage.empty() ? "Low confidence or feature mismatch" : res.errorMessage.c_str());
        cv::putText(canvas, buf, cv::Point(boxX + 12, textY), cv::FONT_HERSHEY_SIMPLEX, 0.44, cv::Scalar(100, 100, 255), 1, cv::LINE_AA);

        textY += 24;
        snprintf(buf, sizeof(buf), "Dial Center: (%.1f, %.1f), Radius: %.1f px", res.center.x, res.center.y, res.radius);
        cv::putText(canvas, buf, cv::Point(boxX + 12, textY), cv::FONT_HERSHEY_SIMPLEX, 0.42, cv::Scalar(180, 180, 180), 1, cv::LINE_AA);

        textY += 22;
        snprintf(buf, sizeof(buf), "Score: %.2f, Confidence: %.1f%%",
                 res.pointerDetectorResult.score, res.confidence * 100.0);
        cv::putText(canvas, buf, cv::Point(boxX + 12, textY), cv::FONT_HERSHEY_SIMPLEX, 0.40, cv::Scalar(160, 160, 160), 1, cv::LINE_AA);

        if (res.center.x > 0 && res.center.y > 0 && res.radius > 5.0f) {
            cv::circle(canvas, res.center, 5, cv::Scalar(0, 0, 255), -1);
            cv::circle(canvas, res.center, static_cast<int>(res.radius), cv::Scalar(0, 0, 180), 1, cv::LINE_AA);
        }
        return;
    }

    cv::Point2f drawCenter = res.center;
    cv::Point2f drawTip = res.pointer_tip;

    if (!invH.empty()) {
        std::vector<cv::Point2f> pts = { res.center, res.pointer_tip };
        std::vector<cv::Point2f> origPts;
        cv::perspectiveTransform(pts, origPts, invH);
        drawCenter = origPts[0];
        drawTip = origPts[1];

        for (size_t i = 0; i < profile.perspective_pts.size(); ++i) {
            cv::line(canvas, profile.perspective_pts[i], profile.perspective_pts[(i + 1) % 4], cv::Scalar(255, 200, 0), 1, cv::LINE_AA);
            cv::circle(canvas, profile.perspective_pts[i], 4, cv::Scalar(0, 255, 255), -1);
        }
    }

    // 1. Draw Pointer Red Center Line & Yellow Tip - Thin Line (1 px)
    cv::circle(canvas, drawCenter, 4, cv::Scalar(0, 0, 255), -1);
    cv::line(canvas, drawCenter, drawTip, cv::Scalar(0, 0, 255), 1, cv::LINE_AA);
    cv::circle(canvas, drawTip, 4, cv::Scalar(0, 255, 255), -1, cv::LINE_AA);

    // Use measured edge coordinates, transforming them with the overlay when
    // drawing on the original frame. Do not connect across invalid sections.
    auto overlayPoint = [&](const cv::Point2d& point) {
        std::vector<cv::Point2f> points = {cv::Point2f(static_cast<float>(point.x), static_cast<float>(point.y))};
        if (!invH.empty()) cv::perspectiveTransform(points, points, invH);
        return cv::Point(cvRound(points[0].x), cvRound(points[0].y));
    };
    auto drawMeasuredSections = [&](const std::vector<RadialSection>& sections, const char* labelPrefix, double maxGap = 1e6) {
        cv::Point previousLeft, previousRight;
        bool previousValid = false;
        double previousRadius = 0.0;
        double lastWidthRadius = -1e6;
        for (const auto& section : sections) {
            if (!section.valid) {
                previousValid = false;
                continue;
            }
            const cv::Point left = overlayPoint(section.leftPoint);
            const cv::Point right = overlayPoint(section.rightPoint);
            if (previousValid && section.radius - previousRadius <= maxGap) {
                cv::line(canvas, previousLeft, left, cv::Scalar(255, 255, 0), 1, cv::LINE_AA);
                cv::line(canvas, previousRight, right, cv::Scalar(255, 0, 255), 1, cv::LINE_AA);
            }
            if (section.radius - lastWidthRadius >= 12.0) {
                cv::line(canvas, left, right, cv::Scalar(0, 255, 0), 1, cv::LINE_AA);
                lastWidthRadius = section.radius;
            }
            cv::circle(canvas, left, 1, cv::Scalar(255, 255, 0), -1, cv::LINE_AA);
            cv::circle(canvas, right, 1, cv::Scalar(255, 0, 255), -1, cv::LINE_AA);
            previousLeft = left;
            previousRight = right;
            previousRadius = section.radius;
            previousValid = true;
        }
        double lastLabelRadius = -1e6;
        for (const auto& section : sections) {
            if (!section.valid || section.radius - lastLabelRadius < 60.0) continue;
            char widthText[48];
            snprintf(widthText, sizeof(widthText), "%sw=%.1f px", labelPrefix, section.width);
            const cv::Point label = overlayPoint(section.rightPoint) + cv::Point(8, -5);
            cv::putText(canvas, widthText, label, cv::FONT_HERSHEY_SIMPLEX, 0.35,
                        cv::Scalar(0, 0, 0), 3, cv::LINE_AA);
            cv::putText(canvas, widthText, label, cv::FONT_HERSHEY_SIMPLEX, 0.35,
                        cv::Scalar(0, 255, 0), 1, cv::LINE_AA);
            lastLabelRadius = section.radius;
        }
        };
    drawMeasuredSections(res.pointerDetectorResult.sections, "");
    if (res.pointerDetectorResult.tailDetected) {
        drawMeasuredSections(res.pointerDetectorResult.tailProfile.sections, "tail ", 4.0);
    }

    // 2. Draw Zero & Max Scale Rays - Thin Lines (1 px)
    double minRad = minAngle * M_PI / 180.0;
    double maxRad = maxAngle * M_PI / 180.0;

    cv::Point2f minP(res.center.x + res.radius * std::cos(minRad), res.center.y + res.radius * std::sin(minRad));
    cv::Point2f maxP(res.center.x + res.radius * std::cos(maxRad), res.center.y + res.radius * std::sin(maxRad));

    if (!invH.empty()) {
        std::vector<cv::Point2f> pts = { minP, maxP };
        std::vector<cv::Point2f> origPts;
        cv::perspectiveTransform(pts, origPts, invH);
        minP = origPts[0];
        maxP = origPts[1];
    }

    cv::line(canvas, drawCenter, minP, cv::Scalar(255, 255, 0), 1, cv::LINE_AA);
    cv::line(canvas, drawCenter, maxP, cv::Scalar(255, 0, 255), 1, cv::LINE_AA);

    // 3. Anchor the final reading badge at the bottom-left, leaving the upper dial visible.
    const int boxW = std::min(380, canvas.cols);
    const int boxH = std::min(100, canvas.rows);
    const int boxX = std::min(10, canvas.cols - boxW);
    const int boxY = std::max(0, canvas.rows - boxH - 10);

    cv::rectangle(canvas, cv::Rect(boxX, boxY, boxW, boxH), cv::Scalar(20, 20, 25), -1);
    cv::rectangle(canvas, cv::Rect(boxX, boxY, boxW, boxH), cv::Scalar(0, 230, 118), 1);

    int textY = boxY + 26;
    char buf[128];
    snprintf(buf, sizeof(buf), "[%s] %s", profile.gauge_id.c_str(), profile.location.device_tag.c_str());
    cv::putText(canvas, buf, cv::Point(boxX + 12, textY), cv::FONT_HERSHEY_SIMPLEX, 0.55, cv::Scalar(200, 200, 200), 1, cv::LINE_AA);

    snprintf(buf, sizeof(buf), "Value: %.3f %s", res.value, profile.unit.c_str());
    cv::putText(canvas, buf, cv::Point(boxX + 12, textY + 30), cv::FONT_HERSHEY_SIMPLEX, 0.75, cv::Scalar(0, 230, 118), 2, cv::LINE_AA);

    snprintf(buf, sizeof(buf), "PointerDetector: Cross-Section Trajectory");
    cv::putText(canvas, buf, cv::Point(boxX + 12, textY + 56), cv::FONT_HERSHEY_SIMPLEX, 0.44, cv::Scalar(180, 180, 180), 1, cv::LINE_AA);
}
