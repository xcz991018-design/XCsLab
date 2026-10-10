#include "GaugeReaderEngine.hpp"
#include <cmath>
#include <iostream>
#include <numeric>
#include <algorithm>
#include <filesystem>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

double GaugeReaderEngine::normalizeAngle(double angleDeg) {
    double a = std::fmod(angleDeg, 360.0);
    if (a < 0) a += 360.0;
    return a;
}



bool GaugeReaderEngine::locateDialCircle(const cv::Mat& gray, cv::Point2f& center, float& radius, cv::Mat* outHubVis) {
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
    int roiR = static_cast<int>(bestRadius * 0.25f);
    int x1 = std::max(0, static_cast<int>(bestCenter.x) - roiR);
    int y1 = std::max(0, static_cast<int>(bestCenter.y) - roiR);
    int x2 = std::min(gray.cols, static_cast<int>(bestCenter.x) + roiR);
    int y2 = std::min(gray.rows, static_cast<int>(bestCenter.y) + roiR);

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
            refinedCenter = cv::Point2f(x1 + pinCircles[0][0], y1 + pinCircles[0][1]);
        }
    }

    center = refinedCenter;
    radius = bestRadius;

    if (outHubVis) {
        cv::cvtColor(gray, *outHubVis, cv::COLOR_GRAY2BGR);
        // Draw primary Hough circle
        cv::circle(*outHubVis, bestCenter, static_cast<int>(bestRadius), cv::Scalar(255, 200, 0), 2, cv::LINE_AA);
        // Draw ROI bounding box
        cv::rectangle(*outHubVis, cv::Rect(x1, y1, x2 - x1, y2 - y1), cv::Scalar(0, 255, 255), 2);
        // Draw refined pin center
        cv::circle(*outHubVis, center, 5, cv::Scalar(0, 0, 255), -1, cv::LINE_AA);
        cv::drawMarker(*outHubVis, center, cv::Scalar(0, 255, 0), cv::MARKER_CROSS, 20, 2);
    }

    return found;
}

double GaugeReaderEngine::detectPointerAngleRobust(const cv::Mat& gray, const cv::Point2f& center, float radius, const GaugeProfile& profile, cv::Mat* outScanVis) {
    GaugeConfig cfg;
    cfg.center = center;
    cfg.radius = radius;
    cfg.pointerMinAngle = profile.min_angle_deg;
    cfg.pointerMaxAngle = profile.max_angle_deg;
    cfg.clockwise = profile.clockwise;
    cfg.pointerBaseRadius = profile.pointerBaseRadius;
    cfg.pointerMinWidth = profile.pointerMinWidth;
    cfg.pointerMaxWidth = profile.pointerMaxWidth;
    cfg.pointerMinLength = profile.pointerMinLength;
    cfg.coarseAngleStep = profile.coarseAngleStep;
    cfg.fineAngleStep = profile.fineAngleStep;
    cfg.fineAngleRange = profile.fineAngleRange;
    cfg.minValidSectionRatio = profile.minValidSectionRatio;

    // Forward optical polarity & dark entity constraints
    cfg.requireDarkPointer = profile.require_dark_pointer;
    cfg.minPointerDarkContrast = profile.min_pointer_dark_contrast;
    cfg.maxPointerInsideGray = profile.max_pointer_inside_gray;

    PointerDetector detector(cfg);
    PointerResult ptrRes = detector.detect(gray, outScanVis);
    return ptrRes.angle;
}

std::vector<cv::Point2f> GaugeReaderEngine::autoDetectCardinalPoints(const cv::Mat& image) {
    if (image.empty()) return {};

    int h = image.rows;
    int w = image.cols;
    double imgArea = static_cast<double>(h * w);

    cv::Mat gray;
    if (image.channels() == 3) {
        cv::cvtColor(image, gray, cv::COLOR_BGR2GRAY);
    } else {
        gray = image.clone();
    }

    // 1. Try detecting full outer dial circle via HoughCircles first
    cv::Point2f autoCenter;
    float autoRadius = 0.0f;
    if (locateDialCircle(gray, autoCenter, autoRadius) && autoRadius > 30.0f) {
        float rPad = autoRadius * 1.05f; // Add 5% padding margin to include full outer bezel & numbers
        std::vector<cv::Point2f> pts(4);
        pts[0] = cv::Point2f(autoCenter.x, autoCenter.y - rPad); // 12H Top
        pts[1] = cv::Point2f(autoCenter.x + rPad, autoCenter.y); // 3H Right
        pts[2] = cv::Point2f(autoCenter.x, autoCenter.y + rPad); // 6H Bottom
        pts[3] = cv::Point2f(autoCenter.x - rPad, autoCenter.y); // 9H Left

        float margin = 5.0f;
        for (auto& pt : pts) {
            pt.x = std::max(margin, std::min(pt.x, static_cast<float>(w) - margin));
            pt.y = std::max(margin, std::min(pt.y, static_cast<float>(h) - margin));
        }
        return pts;
    }

    cv::Mat blurred;
    cv::GaussianBlur(gray, blurred, cv::Size(5, 5), 0);

    cv::Mat thresh;
    cv::threshold(blurred, thresh, 0, 255, cv::THRESH_BINARY + cv::THRESH_OTSU);

    std::vector<std::vector<cv::Point>> cnts;
    cv::findContours(thresh, cnts, cv::RETR_TREE, cv::CHAIN_APPROX_NONE);

    std::vector<cv::Point> bestCnt;
    double maxArea = 0.0;

    auto evaluateContours = [&](const std::vector<std::vector<cv::Point>>& candidates) {
        for (const auto& c : candidates) {
            double area = cv::contourArea(c);
            if (area > 0.01 * imgArea && area < 0.90 * imgArea && c.size() >= 5) {
                cv::RotatedRect ellipse = cv::fitEllipse(c);
                float d1 = ellipse.size.width;
                float d2 = ellipse.size.height;
                float maxD = std::max(d1, d2);
                float minD = std::min(d1, d2);
                float ratio = (maxD > 0.0f) ? (minD / maxD) : 0.0f;

                if (ratio > 0.4f && area > maxArea) {
                    maxArea = area;
                    bestCnt = c;
                }
            }
        }
    };

    evaluateContours(cnts);

    if (bestCnt.empty()) {
        cv::Mat edged;
        cv::Canny(blurred, edged, 50, 200);
        std::vector<std::vector<cv::Point>> cntsCanny;
        cv::findContours(edged, cntsCanny, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_NONE);
        evaluateContours(cntsCanny);
    }

    std::vector<cv::Point2f> cardinalPts(4);

    if (!bestCnt.empty()) {
        cv::Point topPt = bestCnt[0];
        cv::Point bottomPt = bestCnt[0];
        cv::Point leftPt = bestCnt[0];
        cv::Point rightPt = bestCnt[0];

        for (const auto& p : bestCnt) {
            if (p.y < topPt.y) topPt = p;
            if (p.y > bottomPt.y) bottomPt = p;
            if (p.x < leftPt.x) leftPt = p;
            if (p.x > rightPt.x) rightPt = p;
        }

        cardinalPts[0] = cv::Point2f(static_cast<float>(topPt.x), static_cast<float>(topPt.y));        // 12H Top
        cardinalPts[1] = cv::Point2f(static_cast<float>(rightPt.x), static_cast<float>(rightPt.y));    // 3H Right
        cardinalPts[2] = cv::Point2f(static_cast<float>(bottomPt.x), static_cast<float>(bottomPt.y));  // 6H Bottom
        cardinalPts[3] = cv::Point2f(static_cast<float>(leftPt.x), static_cast<float>(leftPt.y));      // 9H Left

        float pad = 10.0f;
        for (auto& pt : cardinalPts) {
            pt.x = std::max(pad, std::min(pt.x, static_cast<float>(w) - pad));
            pt.y = std::max(pad, std::min(pt.y, static_cast<float>(h) - pad));
        }
        return cardinalPts;
    }

    // Fallback: Default 4 points around center
    float cx = w * 0.5f;
    float cy = h * 0.5f;
    float r = std::min(w, h) * 0.35f;

    cardinalPts[0] = cv::Point2f(cx, cy - r); // Top (12H)
    cardinalPts[1] = cv::Point2f(cx + r, cy); // Right (3H)
    cardinalPts[2] = cv::Point2f(cx, cy + r); // Bottom (6H)
    cardinalPts[3] = cv::Point2f(cx - r, cy); // Left (9H)

    return cardinalPts;
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

GaugeResult GaugeReaderEngine::process(const cv::Mat& frame, GaugeProfile& profile) {
    GaugeResult res;
    res.unit = profile.unit;

    if (frame.empty()) {
        res.errorStep = "步骤 1: 原始采集图像 (Original Stream Frame)";
        res.errorMessage = "输入图像帧为空 (frame is empty)";
        res.detailedDebugInfo = "❌ [Pipeline Error] 摄像头或输入图像源未采集到有效图像帧 (cv::Mat::empty)。";
        return res;
    }

    // Step 1: 原图 (Original Frame)
    res.step1_original = frame.clone();
    res.originalImage = res.step1_original;

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
            workFrame = frame.clone();
        }
    } else {
        workFrame = frame.clone();
    }

    res.step2_rectified = workFrame.clone();
    res.rectifiedImage = res.step2_rectified;

    // Step 3: 图像预处理 (Multi-Stage Anti-Glare & Pointer Recovery Preprocessing Pipeline)
    cv::Mat gray;
    if (workFrame.channels() == 3) {
        cv::cvtColor(workFrame, gray, cv::COLOR_BGR2GRAY);
    } else {
        gray = workFrame.clone();
    }
    res.grayImage = gray.clone();

    cv::Mat claheGray;
    cv::Mat blurred;
    cv::Mat threshOtsu;
    cv::Mat threshBlackHat;
    cv::Mat adaptMask;
    cv::Mat threshCombined;

    if (profile.enable_steps_4_to_9) {
        // 1. CLAHE (Contrast Limited Adaptive Histogram Equalization for Glare & Reflection Balancing)
        double clipLimit = profile.clahe_clip_limit > 0 ? profile.clahe_clip_limit : 2.5;
        int gridS = profile.clahe_grid_size > 0 ? profile.clahe_grid_size : 8;
        cv::Ptr<cv::CLAHE> clahe = cv::createCLAHE(clipLimit, cv::Size(gridS, gridS));
        clahe->apply(gray, claheGray);

        // 2. Gaussian Blur to reduce high-frequency specular noise
        int gSize = (profile.gaussian_blur_size > 0 && profile.gaussian_blur_size % 2 == 1) ? profile.gaussian_blur_size : 7;
        double gSigma = profile.gaussian_blur_sigma > 0 ? profile.gaussian_blur_sigma : 1.5;
        cv::GaussianBlur(claheGray, blurred, cv::Size(gSize, gSize), gSigma);
        cv::cvtColor(blurred, res.blurredImage, cv::COLOR_GRAY2BGR);

        // 3. Channel A: Standard Otsu Thresholding
        cv::threshold(blurred, threshOtsu, 0, 255, cv::THRESH_BINARY + cv::THRESH_OTSU);

        // 4. Channel B: Black-Hat Morphological Filter (Exposes solid dark pointer under reflections)
        cv::Mat blackHat;
        int hatSize = (profile.blackhat_kernel_size > 0 && profile.blackhat_kernel_size % 2 == 1) ? profile.blackhat_kernel_size : 21;
        double hatThresh = profile.blackhat_threshold >= 0 ? profile.blackhat_threshold : 32.0;
        cv::Mat kernelHat = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(hatSize, hatSize));
        cv::morphologyEx(claheGray, blackHat, cv::MORPH_BLACKHAT, kernelHat);

        cv::threshold(blackHat, threshBlackHat, hatThresh, 255, cv::THRESH_BINARY);
        cv::bitwise_not(threshBlackHat, threshBlackHat); // Invert so dark pointer is 0, background is 255

        // 5. Channel C: High-Gradient Shadow Discrimination Mask
        int bgSize = (profile.local_bg_kernel_size > 0 && profile.local_bg_kernel_size % 2 == 1) ? profile.local_bg_kernel_size : 35;
        double diffThresh = profile.adapt_diff_threshold >= 0 ? profile.adapt_diff_threshold : 35.0;
        float gradThresh = profile.grad_mag_threshold >= 0 ? static_cast<float>(profile.grad_mag_threshold) : 22.0f;

        cv::Mat localBg;
        cv::GaussianBlur(claheGray, localBg, cv::Size(bgSize, bgSize), 0);

        cv::Mat adaptDiff;
        cv::subtract(localBg, claheGray, adaptDiff);

        cv::Mat gradX, gradY, gradMag;
        cv::Sobel(claheGray, gradX, CV_32F, 1, 0, 3);
        cv::Sobel(claheGray, gradY, CV_32F, 0, 1, 3);
        cv::magnitude(gradX, gradY, gradMag);

        cv::Mat diffMask, magMask;
        cv::compare(adaptDiff, diffThresh, diffMask, cv::CMP_GT);
        cv::compare(gradMag, gradThresh, magMask, cv::CMP_GT);
        cv::Mat solidPointer = diffMask & magMask;
        cv::bitwise_not(solidPointer, adaptMask);

        // 6. Multi-Channel Anti-Glare Fusion & Shadow Discrimination
        cv::Mat threshFilteredOtsu = threshOtsu | adaptMask;
        threshCombined = threshFilteredOtsu & threshBlackHat & adaptMask;

        // 7. Morphological Micro-Closing to repair tiny glare gaps along pointer shaft
        int closeSize = (profile.morph_close_size > 0 && profile.morph_close_size % 2 == 1) ? profile.morph_close_size : 3;
        cv::Mat kernelClose = cv::getStructuringElement(cv::MORPH_ELLIPSE, cv::Size(closeSize, closeSize));
        cv::morphologyEx(threshCombined, threshCombined, cv::MORPH_CLOSE, kernelClose);

        cv::Mat threshBgr;
        cv::cvtColor(threshCombined, threshBgr, cv::COLOR_GRAY2BGR);
        res.step3_preprocessed = threshBgr;
    } else {
        // Fast path: skip heavy morphological filtering (Steps 4-9) to save execution time
        cv::cvtColor(gray, res.blurredImage, cv::COLOR_GRAY2BGR);
        res.step3_preprocessed = res.blurredImage.clone();
    }

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

    // Configure PointerDetector
    GaugeConfig cfg;
    cfg.center = res.center;
    cfg.radius = res.radius;
    cfg.pointerMinAngle = profile.min_angle_deg;
    cfg.pointerMaxAngle = profile.max_angle_deg;
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

    // Single-pass detection: perform detection directly and overlay markers onto rectified image canvas
    cv::Mat rectOverlay = res.rectifiedImage.clone();
    res.pointerDetectorResult = detector.detect(gray, cv::Point2d(res.center.x, res.center.y), &rectOverlay);
    res.diagnosticHudImage = res.pointerDetectorResult.hudImage.clone();
    res.diagnosticText = res.pointerDetectorResult.hudText;

    // Fast diagnostic visualization for Step 3 & Step 11 using existing detection result (no redundant detect call!)
    cv::Mat binaryVis;
    detector.renderDebugVisualization(binaryVis, cv::Point2d(res.center.x, res.center.y), res.pointerDetectorResult);
    res.needleBinaryImage = binaryVis.clone();
    if (!binaryVis.empty()) {
        res.step3_preprocessed = binaryVis;
    }

    res.pointer_angle_deg = res.pointerDetectorResult.angle;

    double minA = normalizeAngle(profile.min_angle_deg);
    double maxA = normalizeAngle(profile.max_angle_deg);
    double ptrA = res.pointer_angle_deg;

    double totalSpanDeg = profile.clockwise ? std::fmod(maxA - minA + 360.0, 360.0)
                                            : std::fmod(minA - maxA + 360.0, 360.0);
    if (totalSpanDeg < 1e-3) totalSpanDeg = 270.0;

    double ptrSpanDeg = profile.clockwise ? std::fmod(ptrA - minA + 360.0, 360.0)
                                          : std::fmod(minA - ptrA + 360.0, 360.0);
    if (ptrSpanDeg > totalSpanDeg && ptrSpanDeg > (360.0 - 25.0)) ptrSpanDeg = 0.0;

    double fraction = ptrSpanDeg / totalSpanDeg;
    fraction = std::max(0.0, std::min(1.0, fraction));

    res.value = profile.min_val + fraction * (profile.max_val - profile.min_val);
    res.confidence = res.pointerDetectorResult.confidence;
    res.success = res.pointerDetectorResult.valid;

    if (!res.success) {
        res.errorStep = "步骤 11: 径向截面与轨迹指针检测 (Radial Trajectory Visual)";
        res.errorMessage = res.pointerDetectorResult.failureReason.empty() ?
            "指针特征检测未通过 (置信度不足或有效截面缺失)" : res.pointerDetectorResult.failureReason;
        res.detailedDebugInfo = res.pointerDetectorResult.detailedDebugText;
    } else {
        res.detailedDebugInfo = res.pointerDetectorResult.detailedDebugText;
        double maxSpanAllowed = totalSpanDeg + 15.0;
        if (ptrSpanDeg > maxSpanAllowed && ptrSpanDeg < (360.0 - 15.0)) {
            char rangeWarn[256];
            snprintf(rangeWarn, sizeof(rangeWarn), "⚠️ [量程警告] 指针角度 (%.1f°) 超出标定量程跨度 [%.1f°, %.1f°] (超出 %.1f°)",
                     res.pointer_angle_deg, profile.min_angle_deg, profile.max_angle_deg, ptrSpanDeg - totalSpanDeg);
            if (!res.centerInconsistent) {
                res.warningMessage = rangeWarn;
            }
        }
    }

    float tipR = static_cast<float>(res.pointerDetectorResult.tipRadius > 0 ? res.pointerDetectorResult.tipRadius : (res.radius * 0.8f));
    double tipRad = res.pointer_angle_deg * M_PI / 180.0;
    res.pointer_tip = cv::Point2f(res.center.x + tipR * static_cast<float>(std::cos(tipRad)),
                                  res.center.y + tipR * static_cast<float>(std::sin(tipRad)));

    res.overlayImage = frame.clone();
    drawOverlay(res.overlayImage, profile, res, invH);

    cv::Mat emptyInvH;
    drawOverlay(rectOverlay, profile, res, emptyInvH); // Draws reading result badge
    res.step4_recognition = rectOverlay; // Step 4 contains clean thin diagnostic markers without HUD blocking the dial

    // Build complete 12 intermediate steps container for inspection & tuning
    res.intermediateSteps.clear();

    auto toBgrMat = [](const cv::Mat& m) -> cv::Mat {
        if (m.empty()) return cv::Mat();
        if (m.channels() == 3) return m.clone();
        cv::Mat out;
        cv::cvtColor(m, out, cv::COLOR_GRAY2BGR);
        return out;
    };

    // 1. Step 1: 原始采集图像
    res.intermediateSteps.push_back({
        1,
        "步骤 1: 原始采集图像 (Original Stream Frame)",
        "摄像头/图像源采集到的未经任何处理的原始图像帧",
        toBgrMat(res.step1_original)
    });

    // 2. Step 2: 4切点透视校正图
    res.intermediateSteps.push_back({
        2,
        "步骤 2: 4切点视场透视矫正图 (FOV Rectified Image)",
        "基于12点/3点/6点/9点4个切点透视变换提取正视圆形表盘",
        toBgrMat(res.step2_rectified)
    });

    // 3. Step 3: 灰度化图像
    res.intermediateSteps.push_back({
        3,
        "步骤 3: 灰度化图像 (Grayscale Image)",
        "透视矫正图转8位单通道灰度图像",
        toBgrMat(gray)
    });

    if (profile.enable_steps_4_to_9) {
        // 4. Step 4: CLAHE 自适应对比度增强图
        res.intermediateSteps.push_back({
            4,
            "步骤 4: CLAHE 对比度增强图 (CLAHE Balanced)",
            "限制对比度自适应直方图均衡化，消除局部阴影与光照不均",
            toBgrMat(claheGray)
        });

        // 5. Step 5: 高斯平滑降噪图
        res.intermediateSteps.push_back({
            5,
            "步骤 5: 高斯平滑降噪图 (Gaussian Blur)",
            "7x7高斯核低通滤波，滤除高频高光噪点与噪波",
            toBgrMat(blurred)
        });

        // 6. Step 6: 通道 A - Otsu 二值化图
        res.intermediateSteps.push_back({
            6,
            "步骤 6: 通道 A - Otsu 二值化图 (Channel A: Otsu Threshold)",
            "全局自适应 Otsu 阈值分割得到初步指针与刻度二值掩码",
            toBgrMat(threshOtsu)
        });

        // 7. Step 7: 通道 B - 黑帽形态学滤波图
        res.intermediateSteps.push_back({
            7,
            "步骤 7: 通道 B - 黑帽形态学滤波图 (Channel B: Black-Hat Filter)",
            "21x21椭圆核黑帽滤波提取细微深色指针特征，有效抑制反光",
            toBgrMat(threshBlackHat)
        });

        // 8. Step 8: 通道 C - 高梯度阴影鉴别掩码
        res.intermediateSteps.push_back({
            8,
            "步骤 8: 通道 C - 阴影鉴别掩码 (Channel C: High-Gradient Mask)",
            "Sobel梯度与局部背景差分掩码，精准区分物理指针边缘与漫反射软阴影",
            toBgrMat(adaptMask)
        });

        // 9. Step 9: 多通道二值融合与微闭合二值图
        res.intermediateSteps.push_back({
            9,
            "步骤 9: 多通道二值融合与微闭合二值图 (Multi-Channel Fused)",
            "Otsu & 黑帽 & 梯度掩码与运算，配合3x3椭圆核微闭合修复小空洞",
            toBgrMat(threshCombined)
        });
    }

    // 10. Step 10: 表盘圆心与旋转轴心定位图与二次防错校验
    cv::Mat hubVis;
    cv::Point2f autoCenter;
    float autoRadius{0.0f};
    locateDialCircle(gray, autoCenter, autoRadius, &hubVis);

    cv::Point2f expectedCenter(gray.cols * 0.5f, gray.rows * 0.5f);
    if (!profile.auto_detect_roi && profile.center.x > 0 && profile.center.y > 0 && !invH.empty()) {
        std::vector<cv::Point2f> srcC = { profile.center };
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

        // Overlay visual warning banner on hubVis
        if (!hubVis.empty()) {
            int barY = hubVis.rows - 38;
            cv::rectangle(hubVis, cv::Rect(10, barY, hubVis.cols - 20, 30), cv::Scalar(0, 0, 220), -1);
            cv::putText(hubVis, "WARNING: Dial Center Offset Exceeds Safety Tolerance!",
                        cv::Point(20, barY + 20), cv::FONT_HERSHEY_SIMPLEX, 0.48, cv::Scalar(255, 255, 255), 1, cv::LINE_AA);
        }
    }

    res.hubRoiImage = hubVis.clone();

    std::string step10Desc = "霍夫圆检测表盘外圈轮廓，并在中心 ROI 精准定位指针旋转轴心";
    if (res.centerInconsistent) {
        step10Desc += " 【⚠️ 防错提示：与标定中心偏差 " + std::to_string(static_cast<int>(centerOffsetDist)) + "px，高于阈值】";
    }

    res.intermediateSteps.push_back({
        10,
        "步骤 10: 表盘圆心与旋转轴心定位图 (Dial & Hub Pin Location)",
        step10Desc,
        toBgrMat(hubVis)
    });

    // 11. Step 11: 径向截面与轨迹指针检测二值图
    res.intermediateSteps.push_back({
        11,
        "步骤 11: 径向截面与轨迹指针检测 (Radial Trajectory Visual)",
        "PointerDetector 算法提取指针截面宽度、连续性与中心线轨迹",
        toBgrMat(res.needleBinaryImage)
    });

    // 12. Step 12: 最终仪表读数识别叠加图
    res.intermediateSteps.push_back({
        12,
        "步骤 12: 最终识别读数叠加图 (Final Recognition Overlay)",
        "绘制指针轴线、终点、刻度射线、截面分布图与 Top-Right 读数面板",
        toBgrMat(res.step4_recognition)
    });

    // Save step photo outputs to files only if explicitly enabled in profile (avoids ~1000ms disk I/O lag)
    if (profile.save_debug_images) {
        std::filesystem::create_directories("debug_steps");

        if (!res.step1_original.empty()) cv::imwrite("step1_original.png", res.step1_original);
        if (!res.step2_rectified.empty()) cv::imwrite("step2_rectified.png", res.step2_rectified);
        if (!res.step3_preprocessed.empty()) cv::imwrite("step3_preprocessed.png", res.step3_preprocessed);
        if (!res.step4_recognition.empty()) cv::imwrite("step4_recognition.png", res.step4_recognition);

        for (const auto& stepItem : res.intermediateSteps) {
            if (!stepItem.image.empty()) {
                char filename[128];
                snprintf(filename, sizeof(filename), "debug_steps/step%02d_%s.png", stepItem.stepNumber,
                         (stepItem.stepNumber == 1 ? "original" :
                          stepItem.stepNumber == 2 ? "rectified" :
                          stepItem.stepNumber == 3 ? "gray" :
                          stepItem.stepNumber == 4 ? "clahe" :
                          stepItem.stepNumber == 5 ? "gaussian_blur" :
                          stepItem.stepNumber == 6 ? "otsu_threshold" :
                          stepItem.stepNumber == 7 ? "blackhat_filter" :
                          stepItem.stepNumber == 8 ? "shadow_mask" :
                          stepItem.stepNumber == 9 ? "fused_binary" :
                          stepItem.stepNumber == 10 ? "hub_location" :
                          stepItem.stepNumber == 11 ? "trajectory_binary" : "recognition_result"));
                cv::imwrite(filename, stepItem.image);
            }
        }

        if (!res.diagnosticHudImage.empty()) {
            cv::imwrite("debug_steps/step12_diagnostics_hud.png", res.diagnosticHudImage);
        }
    }

    return res;
}

void GaugeReaderEngine::drawOverlay(cv::Mat& canvas, const GaugeProfile& profile, const GaugeResult& res, const cv::Mat& invH) {
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
        snprintf(buf, sizeof(buf), "Detected Angle: %.1f deg (Score: %.2f, Confidence: %.1f%%)",
                 res.pointer_angle_deg, res.pointerDetectorResult.score, res.confidence * 100.0);
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

    // 2. Draw Zero & Max Scale Rays - Thin Lines (1 px)
    double minRad = profile.min_angle_deg * M_PI / 180.0;
    double maxRad = profile.max_angle_deg * M_PI / 180.0;

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

    // 3. Draw Text Badge Overlay (Positioned at Top-Right for rectified images to avoid HUD overlap)
    int boxW = 380;
    int boxH = 120;
    int boxX = (invH.empty() && canvas.cols > 750) ? (canvas.cols - boxW - 10) : 10;
    int boxY = (invH.empty() && canvas.cols <= 750) ? 170 : 10;

    cv::rectangle(canvas, cv::Rect(boxX, boxY, boxW, boxH), cv::Scalar(20, 20, 25), -1);
    cv::rectangle(canvas, cv::Rect(boxX, boxY, boxW, boxH), cv::Scalar(0, 230, 118), 1);

    int textY = boxY + 26;
    char buf[128];
    snprintf(buf, sizeof(buf), "[%s] %s", profile.gauge_id.c_str(), profile.location.device_tag.c_str());
    cv::putText(canvas, buf, cv::Point(boxX + 12, textY), cv::FONT_HERSHEY_SIMPLEX, 0.55, cv::Scalar(200, 200, 200), 1, cv::LINE_AA);

    snprintf(buf, sizeof(buf), "Value: %.3f %s", res.value, profile.unit.c_str());
    cv::putText(canvas, buf, cv::Point(boxX + 12, textY + 30), cv::FONT_HERSHEY_SIMPLEX, 0.75, cv::Scalar(0, 230, 118), 2, cv::LINE_AA);

    snprintf(buf, sizeof(buf), "Pointer Angle: %.1f deg", res.pointer_angle_deg);
    cv::putText(canvas, buf, cv::Point(boxX + 12, textY + 56), cv::FONT_HERSHEY_SIMPLEX, 0.52, cv::Scalar(255, 255, 255), 1, cv::LINE_AA);

    snprintf(buf, sizeof(buf), "PointerDetector: Cross-Section Trajectory");
    cv::putText(canvas, buf, cv::Point(boxX + 12, textY + 76), cv::FONT_HERSHEY_SIMPLEX, 0.44, cv::Scalar(180, 180, 180), 1, cv::LINE_AA);
}
