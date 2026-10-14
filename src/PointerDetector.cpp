#include "PointerDetector.hpp"
#include <chrono>
#include <cmath>
#include <iostream>
#include <numeric>
#include <algorithm>
#include <iomanip>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

PointerDetector::PointerDetector()
    : m_config(PointerDetectorConfig()), m_model(PointerShapeModel()) {}

PointerDetector::PointerDetector(const PointerDetectorConfig& config)
    : m_config(config), m_model(PointerShapeModel()) {}

PointerDetector::PointerDetector(const PointerShapeModel& model)
    : m_model(model) {
    m_config.pointerBaseRadius = std::max(15.0, static_cast<double>(model.centerExclusionRadius));
    m_config.maxSearchRadius = std::max(100.0, static_cast<double>(model.radius > 50.0f ? model.radius * 0.92f : 360.0f));
    m_config.minPointerWidth = std::max(1.5, static_cast<double>(model.pointerMinWidth));
    m_config.maxPointerWidth = std::min(45.0, static_cast<double>(model.pointerMaxWidth));
    m_config.radialStepPx = (model.radialStep > 0.0f) ? static_cast<double>(model.radialStep) : 2.0;
    m_config.radialStep = m_config.radialStepPx;
    m_config.angleStep = (model.coarseAngleStep > 0.0f) ? static_cast<double>(model.coarseAngleStep) : 1.0;
    m_config.tailMinRadius = std::max(8.0, m_config.pointerBaseRadius * 0.25);
    m_config.tailMaxRadius = std::min(m_config.pointerBaseRadius * 1.8, m_config.maxSearchRadius * 0.30);
    m_config.tailMinWidth = std::max(4.0, m_config.minPointerWidth * 1.2);
    m_config.tailMaxWidth = std::max(25.0, m_config.maxPointerWidth * 1.5);
    m_config.tailCorridorHalfWidth = std::max(25.0, m_config.tailMaxWidth * 0.7);
}

PointerDetector::PointerDetector(const GaugeConfig& config)
    : m_model(config.toShapeModel()) {
    m_config.pointerBaseRadius = std::max(15.0, config.pointerBaseRadius);
    m_config.maxSearchRadius = std::max(100.0, config.radius > 50.0 ? config.radius * 0.92 : 360.0);
    m_config.minPointerWidth = (config.pointerMinWidth > 0.0) ? config.pointerMinWidth : 1.5;
    m_config.maxPointerWidth = (config.pointerMaxWidth > 0.0) ? config.pointerMaxWidth : 35.0;
    m_config.radialStepPx = 2.0;
    m_config.radialStep = 2.0;
    m_config.angleStep = (config.coarseAngleStep > 0.0) ? config.coarseAngleStep : 1.0;
    m_config.enableTailDetection = config.enableTailDetection;
    m_config.tailMinRadius = std::max(8.0, m_config.pointerBaseRadius * 0.25);
    m_config.tailMaxRadius = (config.tailMaxRadius > 0.0) ? config.tailMaxRadius : std::max(m_config.pointerBaseRadius + 60.0, 130.0);
    m_config.tailMinWidth = (config.tailMinWidth > 0.0) ? config.tailMinWidth : 4.0;
    m_config.tailMaxWidth = (config.tailMaxWidth > 0.0) ? config.tailMaxWidth : 65.0;
    m_config.tailCorridorHalfWidth = (config.tailCorridorHalfWidth > 0.0) ? config.tailCorridorHalfWidth : 45.0;

    // Forward anti-interference & text occlusion parameters
    if (config.maxConsecutiveGapSamples > 0) m_config.maxConsecutiveGapSamples = config.maxConsecutiveGapSamples;
    if (config.minEdgeSupportDensity > 0.0) m_config.minEdgeSupportDensity = config.minEdgeSupportDensity;
    if (config.rawGradientThreshold > 0.0) m_config.rawGradientThreshold = config.rawGradientThreshold;
    if (config.minGradientNormalAlignment > 0.0) m_config.minGradientNormalAlignment = config.minGradientNormalAlignment;
    if (config.minLocalStepContrast > 0.0) m_config.minLocalStepContrast = config.minLocalStepContrast;
    if (config.maxStartBaseGapPx > 0.0) m_config.maxStartBaseGapPx = config.maxStartBaseGapPx;
    if (config.maxParallelAngleDiffDeg > 0.0) m_config.maxParallelAngleDiffDeg = config.maxParallelAngleDiffDeg;
    if (config.minCommonLengthPx > 0.0) m_config.minCommonLengthPx = config.minCommonLengthPx;
    if (config.maxLineFitResidualPx > 0.0) m_config.maxLineFitResidualPx = config.maxLineFitResidualPx;
    if (config.maxOcclusionGap > 0) m_config.maxOcclusionGap = config.maxOcclusionGap;
    if (config.corridorHalfWidthPx > 0.0) m_config.corridorHalfWidthPx = config.corridorHalfWidthPx;
    if (config.tailMinGradientNormalAlignment > 0.0) m_config.tailMinGradientNormalAlignment = config.tailMinGradientNormalAlignment;

    // Forward scoring weights
    if (config.geomWeight > 0.0) m_config.geomWeight = config.geomWeight;
    if (config.wEdgePairCoherence > 0.0) m_config.wEdgePairCoherence = config.wEdgePairCoherence;
    if (config.wCenterStability > 0.0) m_config.wCenterStability = config.wCenterStability;
    if (config.wAxisCenter > 0.0) m_config.wAxisCenter = config.wAxisCenter;
    if (config.wOrientation > 0.0) m_config.wOrientation = config.wOrientation;
    if (config.wReach > 0.0) m_config.wReach = config.wReach;
    if (config.wValidLength > 0.0) m_config.wValidLength = config.wValidLength;
    if (config.wSlenderness > 0.0) m_config.wSlenderness = config.wSlenderness;
    if (config.wPersistence > 0.0) m_config.wPersistence = config.wPersistence;
    if (config.wWidthTrend > 0.0) m_config.wWidthTrend = config.wWidthTrend;
    if (config.wWidthContinuity > 0.0) m_config.wWidthContinuity = config.wWidthContinuity;
    if (config.wEdgeStrength >= 0.0) m_config.wEdgeStrength = config.wEdgeStrength;
    if (config.wGapPenalty >= 0.0) m_config.wGapPenalty = config.wGapPenalty;
    if (config.wTrackingJumpPenalty >= 0.0) m_config.wTrackingJumpPenalty = config.wTrackingJumpPenalty;
    if (config.wTailScore > 0.0) m_config.wTailScore = config.wTailScore;
    if (config.wTailPolarityBoost > 0.0) m_config.wTailPolarityBoost = config.wTailPolarityBoost;
    if (config.confidenceDivisor > 0.0) m_config.confidenceDivisor = config.confidenceDivisor;

    // Forward optical polarity & dark entity constraints
    m_config.requireDarkPointer = config.requireDarkPointer;
    if (config.minPointerDarkContrast > 0.0) m_config.minPointerDarkContrast = config.minPointerDarkContrast;
    if (config.maxPointerInsideGray > 0.0) m_config.maxPointerInsideGray = config.maxPointerInsideGray;
}

void PointerDetector::setConfig(const PointerDetectorConfig& config) {
    m_config = config;
}

void PointerDetector::setConfig(const GaugeConfig& config) {
    m_model = config.toShapeModel();
    m_config.pointerBaseRadius = std::max(15.0, config.pointerBaseRadius);
    m_config.maxSearchRadius = std::max(100.0, config.radius > 50.0 ? config.radius * 0.92 : 360.0);
    m_config.minPointerWidth = (config.pointerMinWidth > 0.0) ? config.pointerMinWidth : 1.5;
    m_config.maxPointerWidth = (config.pointerMaxWidth > 0.0) ? config.pointerMaxWidth : 35.0;
    m_config.radialStepPx = 2.0;
    m_config.radialStep = 2.0;
    m_config.angleStep = (config.coarseAngleStep > 0.0) ? config.coarseAngleStep : 1.0;
    m_config.enableTailDetection = config.enableTailDetection;
    m_config.tailMinRadius = std::max(8.0, m_config.pointerBaseRadius * 0.25);
    m_config.tailMaxRadius = (config.tailMaxRadius > 0.0) ? config.tailMaxRadius : std::max(m_config.pointerBaseRadius + 60.0, 130.0);
    m_config.tailMinWidth = (config.tailMinWidth > 0.0) ? config.tailMinWidth : 4.0;
    m_config.tailMaxWidth = (config.tailMaxWidth > 0.0) ? config.tailMaxWidth : 65.0;
    m_config.tailCorridorHalfWidth = (config.tailCorridorHalfWidth > 0.0) ? config.tailCorridorHalfWidth : 45.0;
    m_config.tailMinGradientNormalAlignment = (config.tailMinGradientNormalAlignment > 0.0) ? config.tailMinGradientNormalAlignment : 0.50;

    // Forward anti-interference & text occlusion parameters
    if (config.maxConsecutiveGapSamples > 0) m_config.maxConsecutiveGapSamples = config.maxConsecutiveGapSamples;
    if (config.minEdgeSupportDensity > 0.0) m_config.minEdgeSupportDensity = config.minEdgeSupportDensity;
    if (config.rawGradientThreshold > 0.0) m_config.rawGradientThreshold = config.rawGradientThreshold;
    if (config.minGradientNormalAlignment > 0.0) m_config.minGradientNormalAlignment = config.minGradientNormalAlignment;
    if (config.minLocalStepContrast > 0.0) m_config.minLocalStepContrast = config.minLocalStepContrast;
    if (config.maxStartBaseGapPx > 0.0) m_config.maxStartBaseGapPx = config.maxStartBaseGapPx;
    if (config.maxParallelAngleDiffDeg > 0.0) m_config.maxParallelAngleDiffDeg = config.maxParallelAngleDiffDeg;
    if (config.minCommonLengthPx > 0.0) m_config.minCommonLengthPx = config.minCommonLengthPx;
    if (config.maxLineFitResidualPx > 0.0) m_config.maxLineFitResidualPx = config.maxLineFitResidualPx;
    if (config.maxOcclusionGap > 0) m_config.maxOcclusionGap = config.maxOcclusionGap;
    if (config.corridorHalfWidthPx > 0.0) m_config.corridorHalfWidthPx = config.corridorHalfWidthPx;

    // Forward scoring weights
    if (config.geomWeight > 0.0) m_config.geomWeight = config.geomWeight;
    if (config.wEdgePairCoherence > 0.0) m_config.wEdgePairCoherence = config.wEdgePairCoherence;
    if (config.wCenterStability > 0.0) m_config.wCenterStability = config.wCenterStability;
    if (config.wAxisCenter > 0.0) m_config.wAxisCenter = config.wAxisCenter;
    if (config.wOrientation > 0.0) m_config.wOrientation = config.wOrientation;
    if (config.wReach > 0.0) m_config.wReach = config.wReach;
    if (config.wValidLength > 0.0) m_config.wValidLength = config.wValidLength;
    if (config.wSlenderness > 0.0) m_config.wSlenderness = config.wSlenderness;
    if (config.wPersistence > 0.0) m_config.wPersistence = config.wPersistence;
    if (config.wWidthTrend > 0.0) m_config.wWidthTrend = config.wWidthTrend;
    if (config.wWidthContinuity > 0.0) m_config.wWidthContinuity = config.wWidthContinuity;
    if (config.wEdgeStrength >= 0.0) m_config.wEdgeStrength = config.wEdgeStrength;
    if (config.wGapPenalty >= 0.0) m_config.wGapPenalty = config.wGapPenalty;
    if (config.wTrackingJumpPenalty >= 0.0) m_config.wTrackingJumpPenalty = config.wTrackingJumpPenalty;
    if (config.wTailScore > 0.0) m_config.wTailScore = config.wTailScore;
    if (config.wTailPolarityBoost > 0.0) m_config.wTailPolarityBoost = config.wTailPolarityBoost;
    if (config.confidenceDivisor > 0.0) m_config.confidenceDivisor = config.confidenceDivisor;

    // Forward optical polarity & dark entity constraints
    m_config.requireDarkPointer = config.requireDarkPointer;
    if (config.minPointerDarkContrast > 0.0) m_config.minPointerDarkContrast = config.minPointerDarkContrast;
    if (config.maxPointerInsideGray > 0.0) m_config.maxPointerInsideGray = config.maxPointerInsideGray;
}

void PointerDetector::setModel(const PointerShapeModel& model) {
    m_model = model;
}

PointerDetectorConfig PointerDetector::getConfig() const {
    return m_config;
}

PointerShapeModel PointerDetector::getModel() const {
    return m_model;
}

double PointerDetector::normalizeAngle(double angleDeg) {
    double a = std::fmod(angleDeg, 360.0);
    if (a < 0.0) a += 360.0;
    return a;
}

// -------------------------------------------------------------------------
// Helper: Bilinear interpolation
// -------------------------------------------------------------------------
static inline float sampleBilinear(const cv::Mat& mat, float x, float y) {
    int x0 = static_cast<int>(std::floor(x));
    int y0 = static_cast<int>(std::floor(y));
    int x1 = x0 + 1;
    int y1 = y0 + 1;

    if (x0 < 0 || y0 < 0 || x1 >= mat.cols || y1 >= mat.rows) {
        int cx = std::max(0, std::min(static_cast<int>(std::round(x)), mat.cols - 1));
        int cy = std::max(0, std::min(static_cast<int>(std::round(y)), mat.rows - 1));
        if (mat.type() == CV_8U) return static_cast<float>(mat.at<uchar>(cy, cx));
        if (mat.type() == CV_32F) return mat.at<float>(cy, cx);
        return 0.0f;
    }

    float fx = x - x0;
    float fy = y - y0;

    if (mat.type() == CV_8U) {
        float v00 = mat.at<uchar>(y0, x0);
        float v10 = mat.at<uchar>(y0, x1);
        float v01 = mat.at<uchar>(y1, x0);
        float v11 = mat.at<uchar>(y1, x1);
        return (1.0f - fx) * (1.0f - fy) * v00 +
               fx * (1.0f - fy) * v10 +
               (1.0f - fx) * fy * v01 +
               fx * fy * v11;
    } else if (mat.type() == CV_32F) {
        float v00 = mat.at<float>(y0, x0);
        float v10 = mat.at<float>(y0, x1);
        float v01 = mat.at<float>(y1, x0);
        float v11 = mat.at<float>(y1, x1);
        return (1.0f - fx) * (1.0f - fy) * v00 +
               fx * (1.0f - fy) * v10 +
               (1.0f - fx) * fy * v01 +
               fx * fy * v11;
    }
    return 0.0f;
}

// -------------------------------------------------------------------------
// Compute Single-Pass Global Scharr Gradient Magnitude and Orientation
// -------------------------------------------------------------------------
GradientFeatureMap PointerDetector::computeGradientFeatures(const cv::Mat& gray) {
    GradientFeatureMap f;

    cv::Mat gradX, gradY;
    cv::Scharr(gray, gradX, CV_32F, 1, 0);
    cv::Scharr(gray, gradY, CV_32F, 0, 1);

    // Magnitude: sqrt(Gx^2 + Gy^2)
    cv::magnitude(gradX, gradY, f.gradMag);

    // Unit gradient components: Gx / Mag, Gy / Mag
    f.gradUnitX = cv::Mat::zeros(gray.size(), CV_32F);
    f.gradUnitY = cv::Mat::zeros(gray.size(), CV_32F);

    int rows = gray.rows;
    int cols = gray.cols;
    for (int y = 0; y < rows; ++y) {
        const float* pGx = gradX.ptr<float>(y);
        const float* pGy = gradY.ptr<float>(y);
        const float* pMag = f.gradMag.ptr<float>(y);
        float* pUx = f.gradUnitX.ptr<float>(y);
        float* pUy = f.gradUnitY.ptr<float>(y);

        for (int x = 0; x < cols; ++x) {
            float m = pMag[x];
            if (m > 1e-3f) {
                float invM = 1.0f / m;
                pUx[x] = pGx[x] * invM;
                pUy[x] = pGy[x] * invM;
            }
        }
    }

    return f;
}

// -------------------------------------------------------------------------
// Section VI: Track a single continuous edge in the candidate corridor
// (isLeft: t in [-corridorHalfWidth, -minWidth/2], !isLeft: t in [+minWidth/2, +corridorHalfWidth])
// -------------------------------------------------------------------------
EdgeTrack PointerDetector::trackEdgeInCorridor(
    const cv::Mat& gray,
    const GradientFeatureMap& features,
    const cv::Point2d& effectivePivot,
    double thetaDeg,
    bool isLeft) {

    EdgeTrack track;
    track.valid = false;

    double rad = thetaDeg * M_PI / 180.0;
    cv::Point2d u(std::cos(rad), std::sin(rad));
    cv::Point2d v(-std::sin(rad), std::cos(rad));

    int imgW = gray.cols;
    int imgH = gray.rows;

    double rStart = m_config.pointerBaseRadius;
    double rEnd = m_config.maxSearchRadius;
    double stepR = (m_config.radialStepPx > 0.0) ? m_config.radialStepPx : 2.0;

    double corridorHalfW = (m_config.corridorHalfWidthPx > 5.0) ? m_config.corridorHalfWidthPx : 15.0;
    double minHalfW = std::max(0.75, m_config.minPointerWidth * 0.5);

    double tMin = isLeft ? -corridorHalfW : minHalfW;
    double tMax = isLeft ? -minHalfW : corridorHalfW;

    double tStep = 0.5; // Fine subpixel lateral step
    int numSamples = static_cast<int>(std::round((tMax - tMin) / tStep)) + 1;

    double trackingWindow = (m_config.trackingWindowPx > 0.0) ? m_config.trackingWindowPx : 3.0;
    double maxJump = (m_config.maxTrackingJumpPx > 0.0) ? m_config.maxTrackingJumpPx : 3.5;
    float minGradThresh = static_cast<float>(m_config.gradientThreshold);

    double prevT = -999.0;
    int consecutiveMisses = 0;
    int maxConsecutiveGap = (m_config.maxConsecutiveGapSamples > 0) ? m_config.maxConsecutiveGapSamples : 16;

    double rawGradThresh = (m_config.rawGradientThreshold > 0.0) ? m_config.rawGradientThreshold : 45.0;
    double minNormalAlign = (m_config.minGradientNormalAlignment > 0.0) ? m_config.minGradientNormalAlignment : 0.72;
    double minStepContrast = (m_config.minLocalStepContrast > 0.0) ? m_config.minLocalStepContrast : 12.0;
    double maxStartGap = (m_config.maxStartBaseGapPx > 0.0) ? m_config.maxStartBaseGapPx : 16.0;

    std::vector<EdgePoint> rawPoints;
    std::vector<EdgePoint> pendingGapPoints;
    double sumStrength = 0.0;
    std::vector<double> strengths;
    int jumpCount = 0;
    int verifiedSupportCount = 0;

    for (double r = rStart; r <= rEnd; r += stepR) {
        // Principle 11: Base proximity check with hub clearance
        if (rawPoints.empty() && (r - rStart) > maxStartGap) {
            break;
        }

        cv::Point2d P_r = effectivePivot + u * r;

        // Check bounds
        if (P_r.x < 5.0 || P_r.x >= imgW - 5.0 || P_r.y < 5.0 || P_r.y >= imgH - 5.0) {
            break;
        }

        // 1. Sample lateral profile in candidate corridor
        struct LocalSample {
            double t;
            float mag;
            float dirScore;
            float edgeScore;
            float grayVal;
        };
        std::vector<LocalSample> profile(numSamples);

        float minGray = 255.0f;
        float maxGray = 0.0f;

        for (int i = 0; i < numSamples; ++i) {
            double t = tMin + i * tStep;
            cv::Point2d Q = P_r + v * t;

            float gVal = sampleBilinear(gray, static_cast<float>(Q.x), static_cast<float>(Q.y));
            if (gVal < minGray) minGray = gVal;
            if (gVal > maxGray) maxGray = gVal;

            float mag = sampleBilinear(features.gradMag, static_cast<float>(Q.x), static_cast<float>(Q.y));
            float ux = sampleBilinear(features.gradUnitX, static_cast<float>(Q.x), static_cast<float>(Q.y));
            float uy = sampleBilinear(features.gradUnitY, static_cast<float>(Q.x), static_cast<float>(Q.y));

            float proj = static_cast<float>(ux * v.x + uy * v.y);
            // Gradient polarity: for dark needle on bright dial,
            // left edge (t < 0) gradient points outward towards -v (proj < 0),
            // right edge (t > 0) gradient points outward towards +v (proj > 0).
            float dirScore = m_config.requireDarkPointer ? (isLeft ? -proj : proj) : std::abs(proj);
            if (dirScore < 0.1f) dirScore = 0.0f;
            float edgeScore = mag * dirScore;

            profile[i] = { t, mag, dirScore, edgeScore, gVal };
        }

        // If slice contrast is low (e.g. inside dark text/number or pure flat background)
        if ((maxGray - minGray) < static_cast<float>(minStepContrast)) {
            consecutiveMisses++;
            if (consecutiveMisses > maxConsecutiveGap) {
                break;
            }
            if (prevT > -900.0) {
                // Synthesize inertial gap point while crossing low-contrast text
                double predT = prevT;
                if (rawPoints.size() >= 4) {
                    double dt_hist = rawPoints.back().t - rawPoints[rawPoints.size() - 4].t;
                    double dr_hist = rawPoints.back().radius - rawPoints[rawPoints.size() - 4].radius;
                    if (dr_hist > 1e-3) {
                        double slope = dt_hist / dr_hist;
                        if (std::abs(slope) < 0.12) {
                            predT = rawPoints.back().t + slope * (r - rawPoints.back().radius);
                        }
                    }
                }
                EdgePoint gp;
                gp.radius = r;
                gp.t = predT;
                cv::Point2d imgPt = P_r + v * predT;
                gp.x = imgPt.x;
                gp.y = imgPt.y;
                gp.strength = 15.0;
                gp.orientation = 0.72;
                gp.hasRawSupport = false;
                gp.valid = false;
                pendingGapPoints.push_back(gp);
            }
            continue;
        }

        // 2. Find local peaks with prominence
        struct LocalPeak {
            double t;
            float edgeScore;
            float dirScore;
            float mag;
        };
        std::vector<LocalPeak> peaks;

        for (int i = 1; i < numSamples - 1; ++i) {
            if (profile[i].mag >= rawGradThresh && profile[i].dirScore >= minNormalAlign && profile[i].edgeScore >= minGradThresh) {
                if (profile[i].edgeScore > profile[i - 1].edgeScore &&
                    profile[i].edgeScore >= profile[i + 1].edgeScore) {
                    float prominence = profile[i].edgeScore - std::min(profile[i - 1].edgeScore, profile[i + 1].edgeScore);
                    if (prominence >= 2.0f) {
                        peaks.push_back({ profile[i].t, profile[i].edgeScore, profile[i].dirScore, profile[i].mag });
                    }
                }
            }
        }

        // 3. Select best peak using tracking window and reference position
        LocalPeak bestPeak{ 0.0, -1.0f, 0.0f, 0.0f };
        double bestPeakScore = -1.0;

        double refT = prevT;
        if (!pendingGapPoints.empty()) {
            refT = pendingGapPoints.back().t;
        }

        for (const auto& p : peaks) {
            if (prevT > -900.0) {
                // Tracking mode: prioritize proximity to refT (inertial trajectory)
                double dt = std::abs(p.t - refT);
                if (dt > trackingWindow * 1.6) continue; // Reject points outside narrow tracking window around needle
                if (dt > maxJump * 1.5) continue;

                double sJump = std::max(0.0, 1.0 - dt / maxJump);
                double sDir = p.dirScore;
                double sMag = std::min(1.0, static_cast<double>(p.edgeScore) / 100.0);

                double score = 0.50 * sJump + 0.30 * sDir + 0.20 * sMag;
                if (score > bestPeakScore) {
                    bestPeakScore = score;
                    bestPeak = p;
                }
            } else {
                // Initial detection mode
                double sDir = p.dirScore;
                double sMag = std::min(1.0, static_cast<double>(p.edgeScore) / 100.0);
                double score = 0.60 * sDir + 0.40 * sMag;
                if (score > bestPeakScore) {
                    bestPeakScore = score;
                    bestPeak = p;
                }
            }
        }

        if (bestPeak.edgeScore > 0.0f) {
            // Re-acquisition after occlusion gap: commit bridged points!
            if (!pendingGapPoints.empty()) {
                for (const auto& gp : pendingGapPoints) {
                    rawPoints.push_back(gp);
                }
                pendingGapPoints.clear();
            }

            if (prevT > -900.0) {
                double dt = std::abs(bestPeak.t - prevT);
                if (dt > maxJump * 0.75) {
                    jumpCount++;
                }
            }
            prevT = bestPeak.t;
            consecutiveMisses = 0;

            EdgePoint pt;
            pt.radius = r;
            pt.t = bestPeak.t;
            cv::Point2d imgPt = P_r + v * bestPeak.t;
            pt.x = imgPt.x;
            pt.y = imgPt.y;
            pt.strength = bestPeak.edgeScore;
            pt.orientation = bestPeak.dirScore;

            // Raw edge support verification
            float rawMag = sampleBilinear(features.gradMag, static_cast<float>(pt.x), static_cast<float>(pt.y));
            float rawUx = sampleBilinear(features.gradUnitX, static_cast<float>(pt.x), static_cast<float>(pt.y));
            float rawUy = sampleBilinear(features.gradUnitY, static_cast<float>(pt.x), static_cast<float>(pt.y));
            float rawProj = static_cast<float>(rawUx * v.x + rawUy * v.y);
            float rawDir = m_config.requireDarkPointer ? (isLeft ? -rawProj : rawProj) : std::abs(rawProj);
            rawDir = std::max(0.0f, rawDir);

            // Step contrast: gIn is inside needle (towards t=0), gOut is outside needle
            float gIn = isLeft ? sampleBilinear(gray, static_cast<float>(pt.x + v.x * 2.0), static_cast<float>(pt.y + v.y * 2.0))
                               : sampleBilinear(gray, static_cast<float>(pt.x - v.x * 2.0), static_cast<float>(pt.y - v.y * 2.0));
            float gOut = isLeft ? sampleBilinear(gray, static_cast<float>(pt.x - v.x * 2.0), static_cast<float>(pt.y - v.y * 2.0))
                                : sampleBilinear(gray, static_cast<float>(pt.x + v.x * 2.0), static_cast<float>(pt.y + v.y * 2.0));
            float localStep = m_config.requireDarkPointer ? (gOut - gIn) : std::abs(gOut - gIn);

            bool darkOk = !m_config.requireDarkPointer || (gIn <= static_cast<float>(m_config.maxPointerInsideGray) && localStep >= (minStepContrast * 0.5f));
            pt.hasRawSupport = (rawMag >= rawGradThresh && rawDir >= minNormalAlign && localStep >= (minStepContrast * 0.6f) && darkOk);
            pt.valid = pt.hasRawSupport;

            if (pt.hasRawSupport) {
                verifiedSupportCount++;
            }

            rawPoints.push_back(pt);
            sumStrength += bestPeak.edgeScore;
            strengths.push_back(bestPeak.edgeScore);
        } else {
            consecutiveMisses++;
            if (consecutiveMisses > maxConsecutiveGap) {
                // Exceeded max consecutive gap tolerance -> terminate track
                break;
            }
            if (prevT > -900.0) {
                // Synthesize inertial gap point for potential re-acquisition across text/scale
                double predT = prevT;
                if (rawPoints.size() >= 4) {
                    double dt_hist = rawPoints.back().t - rawPoints[rawPoints.size() - 4].t;
                    double dr_hist = rawPoints.back().radius - rawPoints[rawPoints.size() - 4].radius;
                    if (dr_hist > 1e-3) {
                        double slope = dt_hist / dr_hist;
                        if (std::abs(slope) < 0.12) {
                            predT = rawPoints.back().t + slope * (r - rawPoints.back().radius);
                        }
                    }
                }
                EdgePoint gp;
                gp.radius = r;
                gp.t = predT;
                cv::Point2d imgPt = P_r + v * predT;
                gp.x = imgPt.x;
                gp.y = imgPt.y;
                gp.strength = 15.0;
                gp.orientation = 0.72;
                gp.hasRawSupport = false;
                gp.valid = false;
                pendingGapPoints.push_back(gp);
            }
        }
    }

    // Minimum points check: a real pointer edge must extend over a meaningful distance
    if (rawPoints.size() < 6) {
        return track;
    }

    track.points = rawPoints;
    track.validPointCount = static_cast<int>(rawPoints.size());
    track.verifiedSupportCount = verifiedSupportCount;
    track.startRadius = rawPoints.front().radius;
    track.endRadius = rawPoints.back().radius;
    track.length = track.endRadius - track.startRadius;
    track.baseDistance = track.startRadius - m_config.pointerBaseRadius;

    // Minimum physical length
    if (track.length < 20.0) {
        return track;
    }

    track.meanStrength = sumStrength / rawPoints.size();
    std::sort(strengths.begin(), strengths.end());
    track.medianStrength = strengths.empty() ? 0.0 : strengths[strengths.size() / 2];
    track.jumpCount = jumpCount;
    track.trackingJumpPenalty = static_cast<double>(jumpCount) / std::max(1, static_cast<int>(rawPoints.size()) - 1);

    // Principle 2 & 5: Edge Support Density Hard Gate
    int expectedSpan = static_cast<int>(std::round((track.endRadius - track.startRadius) / stepR)) + 1;
    track.continuity = std::min(1.0, static_cast<double>(rawPoints.size()) / std::max(1, expectedSpan));
    track.edgeSupportDensity = static_cast<double>(verifiedSupportCount) / std::max(1, expectedSpan);

    // Check max consecutive unsupported samples inside the track
    int curConsecutiveUnsupported = 0;
    int maxConsecutiveGapInTrack = 0;
    for (const auto& pt : rawPoints) {
        if (!pt.hasRawSupport) {
            curConsecutiveUnsupported++;
            if (curConsecutiveUnsupported > maxConsecutiveGapInTrack) {
                maxConsecutiveGapInTrack = curConsecutiveUnsupported;
            }
        } else {
            curConsecutiveUnsupported = 0;
        }
    }
    track.maxConsecutiveGap = maxConsecutiveGapInTrack;

    double minSupportThresh = (m_config.minEdgeSupportDensity > 0.0) ? m_config.minEdgeSupportDensity : 0.45;
    if (track.edgeSupportDensity < minSupportThresh ||
        track.maxConsecutiveGap > maxConsecutiveGap ||
        track.continuity < 0.60) {
        track.valid = false;
        return track;
    }

    // Principle 1 & 8: Robust line fitting (cv::DIST_HUBER) to resist text boundary outliers
    std::vector<cv::Point2f> cvPts;
    cvPts.reserve(rawPoints.size());
    for (const auto& p : rawPoints) {
        if (p.hasRawSupport) {
            cvPts.emplace_back(static_cast<float>(p.x), static_cast<float>(p.y));
        }
    }

    if (cvPts.size() < 5) {
        track.valid = false;
        return track;
    }

    cv::Vec4f lineParams;
    cv::fitLine(cvPts, lineParams, cv::DIST_HUBER, 0, 0.01, 0.01);
    float vx = lineParams[0];
    float vy = lineParams[1];
    float x0 = lineParams[2];
    float y0 = lineParams[3];

    // Compute line angle in [0, 360)
    double lineRad = std::atan2(vy, vx);
    double lineDeg = lineRad * 180.0 / M_PI;
    if (lineDeg < 0.0) lineDeg += 360.0;
    track.lineAngle = lineDeg;

    // Calculate mean orthogonal residual to fitted line
    double sumResidual = 0.0;
    double maxResidual = 0.0;
    for (const auto& pt : cvPts) {
        double dist = std::abs((pt.x - x0) * vy - (pt.y - y0) * vx);
        sumResidual += dist;
        if (dist > maxResidual) maxResidual = dist;
    }
    double meanRes = sumResidual / cvPts.size();
    track.lineFitResidual = meanRes;

    // Orientation consistency across track points
    double sumOrient = 0.0;
    for (const auto& p : rawPoints) sumOrient += p.orientation;
    double meanOrient = sumOrient / rawPoints.size();

    // Straightness score
    double straightResScore = 1.0 / (1.0 + 0.35 * meanRes * meanRes);
    track.straightness = std::max(0.0, std::min(1.0, 0.60 * straightResScore + 0.40 * meanOrient));
    track.curvature = meanRes;

    // Principle 8 & 9: Straightness residual hard gate
    double maxResThresh = (m_config.maxLineFitResidualPx > 0.0) ? m_config.maxLineFitResidualPx : 2.2;
    track.valid = (track.length >= 25.0 &&
                   track.edgeSupportDensity >= minSupportThresh &&
                   track.lineFitResidual <= maxResThresh &&
                   track.maxConsecutiveGap <= maxConsecutiveGap);
    return track;
}

// -------------------------------------------------------------------------
// Section XIII & XIV: Form Candidate Region from Left & Right EdgeTracks
// -------------------------------------------------------------------------
PointerCandidateRegion PointerDetector::evaluateCandidateRegion(
    const cv::Mat& gray,
    const GradientFeatureMap& features,
    const cv::Point2d& effectivePivot,
    double thetaDeg) {

    PointerCandidateRegion region;
    region.axisAngle = normalizeAngle(thetaDeg);
    region.geometryValid = false;

    // 1. Independently track Left Edge (t < 0) and Right Edge (t > 0)
    region.leftEdge = trackEdgeInCorridor(gray, features, effectivePivot, thetaDeg, true);
    region.rightEdge = trackEdgeInCorridor(gray, features, effectivePivot, thetaDeg, false);

    double maxResThresh = (m_config.maxLineFitResidualPx > 0.0) ? m_config.maxLineFitResidualPx : 2.2;
    // Unilateral edge complementation: if one side is fully valid and long, and the other side has decent points,
    // prevent unilateral text occlusion from rejecting the entire candidate
    if (!region.leftEdge.valid && region.rightEdge.valid && region.leftEdge.points.size() >= 6 && region.leftEdge.lineFitResidual <= maxResThresh * 1.25) {
        region.leftEdge.valid = true;
    } else if (region.leftEdge.valid && !region.rightEdge.valid && region.rightEdge.points.size() >= 6 && region.rightEdge.lineFitResidual <= maxResThresh * 1.25) {
        region.rightEdge.valid = true;
    }

    if (!region.leftEdge.valid || !region.rightEdge.valid) {
        return region;
    }

    // 2. Section X: Common Overlap Span
    double commonStart = std::max(region.leftEdge.startRadius, region.rightEdge.startRadius);
    double commonEnd = std::min(region.leftEdge.endRadius, region.rightEdge.endRadius);
    double commonLen = std::max(0.0, commonEnd - commonStart);

    region.commonStartRadius = commonStart;
    region.commonEndRadius = commonEnd;
    region.commonLength = commonLen;

    region.leftStraightness = region.leftEdge.straightness;
    region.rightStraightness = region.rightEdge.straightness;
    region.straightness = (region.leftStraightness + region.rightStraightness) * 0.5;

    region.leftContinuity = region.leftEdge.continuity;
    region.rightContinuity = region.rightEdge.continuity;

    region.leftSupportDensity = region.leftEdge.edgeSupportDensity;
    region.rightSupportDensity = region.rightEdge.edgeSupportDensity;
    region.edgeSupportDensity = (region.leftSupportDensity + region.rightSupportDensity) * 0.5;

    // 3. Section XI: Parallelism Check
    // Compare line angles of left and right tracks modulo 180
    double angL = std::fmod(region.leftEdge.lineAngle, 180.0);
    if (angL < 0.0) angL += 180.0;
    double angR = std::fmod(region.rightEdge.lineAngle, 180.0);
    if (angR < 0.0) angR += 180.0;

    double diffAng = std::abs(angL - angR);
    if (diffAng > 90.0) diffAng = 180.0 - diffAng;

    double maxParallelDiff = (m_config.maxParallelAngleDiffDeg > 0.0) ? m_config.maxParallelAngleDiffDeg : 15.0;
    region.parallelism = std::max(0.0, 1.0 - diffAng / maxParallelDiff);

    // 4. Section XII: Base Proximity Check
    double baseDistL = std::abs(region.leftEdge.baseDistance);
    double baseDistR = std::abs(region.rightEdge.baseDistance);
    double maxBaseDist = (m_config.maxBaseDistancePx > 0.0) ? m_config.maxBaseDistancePx : 25.0;
    double baseProxL = std::max(0.0, 1.0 - baseDistL / maxBaseDist);
    double baseProxR = std::max(0.0, 1.0 - baseDistR / maxBaseDist);
    region.baseProximity = (baseProxL + baseProxR) * 0.5;

    // 5. Section XVI: First Layer - Geometry Score Gate with Strict Straightness & Raw Support
    double minCommonLen = (m_config.minCommonLengthPx > 0.0) ? m_config.minCommonLengthPx : 35.0;
    double lenBase = std::min(1.0, commonLen / minCommonLen);
    double lenExt = std::min(1.5, commonLen / 120.0);
    double lenScore = 0.40 * lenBase + 0.60 * lenExt;

    region.geometryScore = 0.30 * lenScore +
                           0.20 * region.parallelism +
                           0.20 * region.straightness +
                           0.15 * region.baseProximity +
                           0.15 * region.edgeSupportDensity;

    double minSupportThresh = (m_config.minEdgeSupportDensity > 0.0) ? m_config.minEdgeSupportDensity : 0.45;

    // HARD GEOMETRY GATE (Principle 11):
    // 1. Both edges must be approximately straight: lineFitResidual <= maxResThresh
    // 2. Both edges must be approximately parallel: diffAng <= maxParallelDiff
    // 3. Both edges must have sufficient common overlap: commonLen >= minCommonLen
    // 4. Both edges must start near pointerBaseRadius: baseProximity >= 0.40
    // 5. Both edges must have high Raw Edge Support Density: supportDensity >= minSupportThresh * 0.85
    bool straightValid = (region.leftEdge.lineFitResidual <= maxResThresh &&
                          region.rightEdge.lineFitResidual <= maxResThresh &&
                          region.straightness >= 0.58);
    bool parallelValid = (diffAng <= maxParallelDiff);
    bool lengthValid = (commonLen >= minCommonLen);
    bool baseValid = (region.baseProximity >= 0.40);
    bool supportValid = (region.leftSupportDensity >= minSupportThresh * 0.85 &&
                         region.rightSupportDensity >= minSupportThresh * 0.85);

    if (straightValid && parallelValid && lengthValid && baseValid && supportValid) {
        region.geometryValid = true;
    } else {
        region.geometryValid = false;
        region.bodyScore = 0.0;
        return region;
    }

    // 6. Section XIV & XV: Second Layer - Width Profile across common overlap
    // Build radial interpolation lookup for leftT(r) and rightT(r)
    std::map<int, double> leftTMap, rightTMap;
    for (const auto& p : region.leftEdge.points) {
        int rKey = static_cast<int>(std::round(p.radius * 2.0));
        leftTMap[rKey] = p.t;
    }
    for (const auto& p : region.rightEdge.points) {
        int rKey = static_cast<int>(std::round(p.radius * 2.0));
        rightTMap[rKey] = p.t;
    }

    double stepR = (m_config.radialStepPx > 0.0) ? m_config.radialStepPx : 2.0;
    std::vector<double> widths;
    std::vector<double> radii;

    double sumInside = 0.0;
    double sumOutside = 0.0;
    int sliceCount = 0;
    std::vector<double> sliceInVals;

    double rad = thetaDeg * M_PI / 180.0;
    cv::Point2d u(std::cos(rad), std::sin(rad));
    cv::Point2d v(-std::sin(rad), std::cos(rad));

    for (double r = commonStart; r <= commonEnd; r += stepR) {
        int rKey = static_cast<int>(std::round(r * 2.0));
        if (leftTMap.find(rKey) != leftTMap.end() && rightTMap.find(rKey) != rightTMap.end()) {
            double w = rightTMap[rKey] - leftTMap[rKey];
            if (w >= m_config.minPointerWidth && w <= m_config.maxPointerWidth) {
                widths.push_back(w);
                radii.push_back(r);

                double tL = leftTMap[rKey];
                double tR = rightTMap[rKey];
                cv::Point2d P_r = effectivePivot + u * r;
                double tC = (tL + tR) * 0.5;

                float valCenter = sampleBilinear(gray, static_cast<float>(P_r.x + v.x * tC), static_cast<float>(P_r.y + v.y * tC));
                double sliceIn = valCenter;
                if (w >= 4.0) {
                    float valInL = sampleBilinear(gray, static_cast<float>(P_r.x + v.x * (tL + 1.2)), static_cast<float>(P_r.y + v.y * (tL + 1.2)));
                    float valInR = sampleBilinear(gray, static_cast<float>(P_r.x + v.x * (tR - 1.2)), static_cast<float>(P_r.y + v.y * (tR - 1.2)));
                    sliceIn = (valCenter * 2.0 + valInL + valInR) / 4.0;
                }

                float valOutL = sampleBilinear(gray, static_cast<float>(P_r.x + v.x * (tL - 3.5)), static_cast<float>(P_r.y + v.y * (tL - 3.5)));
                float valOutR = sampleBilinear(gray, static_cast<float>(P_r.x + v.x * (tR + 3.5)), static_cast<float>(P_r.y + v.y * (tR + 3.5)));
                double sliceOut = (valOutL + valOutR) * 0.5;

                sliceInVals.push_back(sliceIn);
                sumInside += sliceIn;
                sumOutside += sliceOut;
                sliceCount++;
            }
        }
    }

    if (widths.size() < 4) {
        region.geometryValid = false;
        region.bodyScore = 0.0;
        return region;
    }

    if (sliceCount > 0) {
        // 使用 40% 分位数（或稳健中位数），有效排除穿越表盘刻度线或字母时的白斑干扰
        std::vector<double> sortedIns = sliceInVals;
        std::sort(sortedIns.begin(), sortedIns.end());
        size_t pIdx = std::min(sortedIns.size() - 1, static_cast<size_t>(sortedIns.size() * 0.40));
        region.meanInsideGray = sortedIns[pIdx];
        region.meanOutsideGray = sumOutside / sliceCount;
        region.entityContrast = region.meanOutsideGray - region.meanInsideGray;
    }

    // Optical polarity & dark entity hard gate
    if (m_config.requireDarkPointer) {
        region.darkEntityValid = (region.entityContrast >= m_config.minPointerDarkContrast &&
                                  region.meanInsideGray <= m_config.maxPointerInsideGray);
        if (!region.darkEntityValid) {
            region.geometryValid = false;
            region.bodyScore = 0.0;
            return region;
        }
    } else {
        region.darkEntityValid = true;
    }

    double sumW = 0.0;
    for (double w : widths) sumW += w;
    region.meanWidth = sumW / widths.size();

    std::vector<double> sortedW = widths;
    std::sort(sortedW.begin(), sortedW.end());
    region.medianWidth = sortedW[sortedW.size() / 2];

    // Hard gate on needle body width: must strictly satisfy [minPointerWidth, maxPointerWidth]
    if (region.medianWidth < m_config.minPointerWidth || region.medianWidth > m_config.maxPointerWidth) {
        region.geometryValid = false;
        region.bodyScore = 0.0;
        return region;
    }

    // Width continuity: small step changes
    double sumDw = 0.0;
    int dwCount = 0;
    int countDecreasing = 0;
    double tol = 0.5;

    for (size_t i = 0; i < widths.size() - 1; ++i) {
        double dw = std::abs(widths[i + 1] - widths[i]);
        sumDw += std::max(0.0, 1.0 - dw / m_config.maxWidthChange);
        dwCount++;
        if (widths[i + 1] <= widths[i] + tol) {
            countDecreasing++;
        }
    }
    region.widthContinuity = (dwCount > 0) ? (sumDw / dwCount) : 0.0;
    double monoRatio = (dwCount > 0) ? (static_cast<double>(countDecreasing) / dwCount) : 0.0;

    // Linear regression of width slope: W(r)
    double sumR = 0.0, sumW_cov = 0.0, sumR2 = 0.0, sumRW = 0.0;
    int nW = static_cast<int>(widths.size());
    for (int i = 0; i < nW; ++i) {
        sumR += radii[i];
        sumW_cov += widths[i];
        sumR2 += radii[i] * radii[i];
        sumRW += radii[i] * widths[i];
    }
    double denom = nW * sumR2 - sumR * sumR;
    double slope = (std::abs(denom) > 1e-4) ? ((nW * sumRW - sumR * sumW_cov) / denom) : 0.0;
    region.widthSlope = slope;

    double slopeScore = 0.0;
    if (slope < 0.0) {
        slopeScore = std::min(1.0, -slope * 25.0);
    }
    if (slope >= -0.04 && slope <= 0.02) {
        slopeScore = std::max(slopeScore, 0.85); // Uniform slender body
    } else if (slope > 0.04) {
        slopeScore = std::max(0.0, 1.0 - (slope - 0.04) * 15.0); // Penalize outward widening
    }
    double widthTrend = 0.50 * monoRatio + 0.50 * slopeScore;

    double slender = commonLen / std::max(1.0, region.medianWidth);
    double slendernessScore = slender / (slender + 12.0);

    double contrastNorm = std::min(1.0, std::max(0.0, region.entityContrast / 60.0));

    // Section XVI: Final Body Score
    region.bodyScore = 0.30 * region.geometryScore +
                       0.20 * slendernessScore +
                       0.15 * region.widthContinuity +
                       0.15 * widthTrend +
                       0.10 * (commonLen / 250.0) +
                       0.10 * contrastNorm;

    return region;
}

// -------------------------------------------------------------------------
// Extract Cross Section Bilateral Edge Pair along v
// -------------------------------------------------------------------------
PointerCrossSection PointerDetector::extractCrossSection(
    const cv::Mat& gray,
    const GradientFeatureMap& features,
    const cv::Point2d& effectivePivot,
    double thetaDeg,
    double r,
    double prevLeftT,
    double prevRightT,
    double previousWidth,
    double previousCenterT) {

    RadialSection sec;
    sec.valid = false;
    sec.leftValid = false;
    sec.rightValid = false;
    sec.edgePairCoherent = false;
    sec.centerStable = false;
    sec.trackingStable = false;
    sec.radius = r;

    double rad = thetaDeg * M_PI / 180.0;
    // 1. Direction unit vector along candidate angle theta: u = (cos(theta), sin(theta))
    cv::Point2d u(std::cos(rad), std::sin(rad));
    // Transverse unit vector perpendicular to candidate angle: v = (-sin(theta), cos(theta))
    cv::Point2d v(-std::sin(rad), std::cos(rad));

    // Sampling point on candidate axis: P(r) = effectivePivot + u * r
    cv::Point2d P_r = effectivePivot + u * r;
    cv::Point2d axisPoint = P_r;

    sec.leftPoint = axisPoint;
    sec.rightPoint = axisPoint;
    sec.centerPoint = axisPoint;

    int imgW = gray.cols;
    int imgH = gray.rows;

    if (axisPoint.x < 5.0 || axisPoint.x >= imgW - 5.0 ||
        axisPoint.y < 5.0 || axisPoint.y >= imgH - 5.0) {
        return sec;
    }

    double halfW = m_config.maxHalfWidth;
    double tStep = 0.5; // Fine subpixel sampling step along transverse normal
    int numSamples = static_cast<int>(std::round(halfW * 2.0 / tStep)) | 1;

    struct SamplePoint {
        double t;
        float mag;
        float dirScore;
        float edgeScore;
        float grayVal;
    };

    std::vector<SamplePoint> profile(numSamples);
    double tStart = -halfW;

    for (int i = 0; i < numSamples; ++i) {
        double t = tStart + i * tStep;
        // Transverse scanning: Q(r, t) = P(r) + v * t
        cv::Point2d Q_r_t = P_r + v * t;
        cv::Point2d q = Q_r_t;

        float mag = sampleBilinear(features.gradMag, static_cast<float>(q.x), static_cast<float>(q.y));
        float ux = sampleBilinear(features.gradUnitX, static_cast<float>(q.x), static_cast<float>(q.y));
        float uy = sampleBilinear(features.gradUnitY, static_cast<float>(q.x), static_cast<float>(q.y));
        float gVal = sampleBilinear(gray, static_cast<float>(q.x), static_cast<float>(q.y));

        // Direction matching = |dot(normalizedGradient, v)|
        float dirScore = std::abs(static_cast<float>(ux * v.x + uy * v.y));

        // edgeScore = gradientMagnitude * directionScore
        float edgeScore = mag * dirScore;

        profile[i] = { t, mag, dirScore, edgeScore, gVal };
    }

    // Section IV: Identify local edge peaks along transverse line
    struct EdgePeak {
        double t;
        float edgeScore;
        float dirScore;
        int idx;
    };
    std::vector<EdgePeak> peaks;

    float minGradThresh = static_cast<float>(m_config.gradientThreshold);

    for (int i = 1; i < numSamples - 1; ++i) {
        float val = profile[i].edgeScore;
        if (val >= minGradThresh) {
            if (val >= profile[i - 1].edgeScore && val >= profile[i + 1].edgeScore) {
                peaks.push_back({ profile[i].t, val, profile[i].dirScore, i });
            }
        }
    }

    // Section V & VI: EDGE TRACKING vs RE-DETECTION
    bool isTracking = (prevLeftT > -900.0 && prevRightT > -900.0 && previousWidth > 0.0);

    double bestPairScore = -1.0;
    EdgePeak bestL{0.0, 0.0f, 0.0f, -1}, bestR{0.0, 0.0f, 0.0f, -1};

    double win = m_config.trackingWindow;
    double maxJump = m_config.maxTrackingJumpPx;

    for (size_t i = 0; i < peaks.size(); ++i) {
        for (size_t j = i + 1; j < peaks.size(); ++j) {
            double tL = peaks[i].t;
            double tR = peaks[j].t;

            if (isTracking) {
                // Section VI: Localized search around prior edge positions
                if (std::abs(tL - prevLeftT) > win || std::abs(tR - prevRightT) > win) {
                    continue; // Skip peaks outside localized tracking window
                }
                // Section VII: Reject sudden jumps
                if (std::abs(tL - prevLeftT) > maxJump || std::abs(tR - prevRightT) > maxJump) {
                    continue;
                }
            } else {
                // Initial seeding near pointerBaseRadius: must be roughly centered around axis
                double initialCenter = (tL + tR) * 0.5;
                if (std::abs(initialCenter) > 6.0) continue;
            }

            double w = tR - tL;
            // Reasonable width bounds
            if (w < m_config.minPointerWidth || w > m_config.maxPointerWidth) continue;

            double cT = (tL + tR) * 0.5;
            if (std::abs(cT) > m_config.maxCenterOffset) continue;

            // 1. Axis center consistency: centerT must be near axis (0.0)
            double sAxisCenter = 1.0 / (1.0 + 0.20 * cT * cT);

            // 2. Center step deviation from prior section
            double cStep = isTracking ? std::abs(cT - previousCenterT) : 0.0;
            if (isTracking && cStep > m_config.maxCenterChange) continue;
            double sCenterStep = 1.0 / (1.0 + 0.25 * cStep * cStep);

            // 3. Width continuity
            double sWidth = 1.0;
            if (isTracking) {
                double dw = std::abs(w - previousWidth);
                if (dw > m_config.maxWidthChange * 1.3) continue;
                sWidth = std::max(0.0, 1.0 - dw / m_config.maxWidthChange);
            }

            // 4. Pair motion consistency: |dL - dR|
            double sMotion = 1.0;
            if (isTracking) {
                double dL = tL - prevLeftT;
                double dR = tR - prevRightT;
                double dMotion = std::abs(dL - dR);
                sMotion = std::max(0.0, 1.0 - dMotion / m_config.maxWidthChange);
            }

            // 5. Edge orientation alignment
            double sOrient = (peaks[i].dirScore + peaks[j].dirScore) * 0.5;

            // 6. Normalized edge strength (auxiliary only)
            double avgPeakEdge = (peaks[i].edgeScore + peaks[j].edgeScore) * 0.5;
            double normEdgeStrength = std::min(1.0, avgPeakEdge / 60.0);

            // 7. Interior contrast check
            int iL = peaks[i].idx;
            int iR = peaks[j].idx;
            float sumIn = 0.0f;
            int cntIn = 0;
            for (int k = iL; k <= iR; ++k) {
                sumIn += profile[k].grayVal;
                cntIn++;
            }
            float avgIn = cntIn > 0 ? (sumIn / cntIn) : 128.0f;
            float sContrast = std::max(0.0f, std::min(1.0f, (255.0f - avgIn) / 200.0f));

            // Bilateral Edge Pair Coherence score for this cross section
            double pairScore = 0.25 * sAxisCenter +
                               0.20 * sCenterStep +
                               0.20 * sMotion +
                               0.15 * sWidth +
                               0.10 * sOrient +
                               0.05 * normEdgeStrength +
                               0.05 * sContrast;

            if (pairScore > bestPairScore) {
                bestPairScore = pairScore;
                bestL = peaks[i];
                bestR = peaks[j];
            }
        }
    }

    if (bestPairScore > 0.25 && bestL.idx >= 0 && bestR.idx >= 0) {
        sec.leftValid = true;
        sec.rightValid = true;
        sec.edgePairCoherent = true;
        sec.leftT = bestL.t;
        sec.rightT = bestR.t;
        sec.width = bestR.t - bestL.t;
        sec.centerT = (bestL.t + bestR.t) * 0.5;

        sec.centerStable = (std::abs(sec.centerT) <= m_config.maxCenterOffset);
        sec.trackingStable = (!isTracking || (std::abs(sec.leftT - prevLeftT) <= maxJump && std::abs(sec.rightT - prevRightT) <= maxJump));

        sec.valid = (sec.leftValid && sec.rightValid && sec.edgePairCoherent && sec.centerStable && sec.trackingStable);

        sec.leftStrength = bestL.edgeScore;
        sec.rightStrength = bestR.edgeScore;
        sec.leftDirectionScore = bestL.dirScore;
        sec.rightDirectionScore = bestR.dirScore;

        sec.leftScore = bestL.edgeScore;
        sec.rightScore = bestR.edgeScore;
        sec.pairScore = bestPairScore;

        sec.leftPoint = axisPoint + v * sec.leftT;
        sec.rightPoint = axisPoint + v * sec.rightT;
        sec.centerPoint = axisPoint + v * sec.centerT;
    } else {
        sec.valid = false;
        sec.edgePairCoherent = false;
        sec.centerStable = false;
        sec.trackingStable = false;
    }

    return sec;
}

// -------------------------------------------------------------------------
// Evaluate Radial Direction Profile along a ray (phi in [0, 360))
// -------------------------------------------------------------------------
RadialDirectionProfile PointerDetector::evaluateRadialDirectionProfile(
    const cv::Mat& gray,
    const GradientFeatureMap& features,
    const cv::Point2d& effectivePivot,
    double thetaDeg) {

    RadialDirectionProfile prof;
    double emitTheta = normalizeAngle(thetaDeg);
    prof.angle = emitTheta;

    double rStart = m_config.pointerBaseRadius;
    double rEnd = m_config.maxSearchRadius;
    double stepR = (m_config.radialStepPx > 0.0) ? m_config.radialStepPx : m_config.radialStep;
    if (stepR < 1.0) stepR = 2.0;

    // Section I: Form and check Candidate Region FIRST!
    // If the left/right tracks are not long, straight, parallel, and starting near pointerBaseRadius,
    // IMMEDIATELY REJECT! Do NOT perform radial cross-section synthesis for invalid geometry!
    PointerCandidateRegion reg = evaluateCandidateRegion(gray, features, effectivePivot, emitTheta);
    prof.candidateRegion = reg;
    prof.leftLength = reg.leftEdge.length;
    prof.rightLength = reg.rightEdge.length;
    prof.commonLength = reg.commonLength;
    prof.leftStraightness = reg.leftStraightness;
    prof.rightStraightness = reg.rightStraightness;
    prof.parallelism = reg.parallelism;
    prof.baseProximity = reg.baseProximity;
    prof.leftSupportDensity = reg.leftSupportDensity;
    prof.rightSupportDensity = reg.rightSupportDensity;
    prof.edgeSupportDensity = reg.edgeSupportDensity;
    prof.geometryScore = reg.geometryScore;
    prof.geometryValid = reg.geometryValid;
    prof.meanInsideGray = reg.meanInsideGray;
    prof.entityContrast = reg.entityContrast;

    if (!prof.geometryValid) {
        prof.bodyScore = 0.0;
        prof.valid = false;
        prof.directionScore = 0.0;
        return prof;
    }

    // Build fast lookup for left & right points in candidateRegion
    std::map<int, EdgePoint> leftPtMap, rightPtMap;
    for (const auto& p : reg.leftEdge.points) {
        int rKey = static_cast<int>(std::round(p.radius * 2.0));
        leftPtMap[rKey] = p;
    }
    for (const auto& p : reg.rightEdge.points) {
        int rKey = static_cast<int>(std::round(p.radius * 2.0));
        rightPtMap[rKey] = p;
    }

    double rad = emitTheta * M_PI / 180.0;
    cv::Point2d u(std::cos(rad), std::sin(rad));
    cv::Point2d v(-std::sin(rad), std::cos(rad));

    std::vector<RadialSection> allSections;

    for (double r = rStart; r <= rEnd; r += stepR) {
        RadialSection sec;
        sec.radius = r;
        sec.valid = false;
        sec.leftValid = false;
        sec.rightValid = false;
        sec.edgePairCoherent = false;
        sec.centerStable = false;
        sec.trackingStable = false;

        cv::Point2d axisPt = effectivePivot + u * r;
        sec.leftPoint = axisPt;
        sec.rightPoint = axisPt;
        sec.centerPoint = axisPt;

        int rKey = static_cast<int>(std::round(r * 2.0));
        bool hasL = (leftPtMap.find(rKey) != leftPtMap.end());
        bool hasR = (rightPtMap.find(rKey) != rightPtMap.end());

        if (hasL && hasR && r >= reg.commonStartRadius && r <= reg.commonEndRadius) {
            const auto& pL = leftPtMap[rKey];
            const auto& pR = rightPtMap[rKey];

            double w = pR.t - pL.t;
            if (w >= m_config.minPointerWidth && w <= m_config.maxPointerWidth) {
                sec.leftValid = true;
                sec.rightValid = true;
                sec.leftT = pL.t;
                sec.rightT = pR.t;
                sec.width = w;
                sec.centerT = (pL.t + pR.t) * 0.5;

                sec.leftStrength = pL.strength;
                sec.rightStrength = pR.strength;
                sec.leftDirectionScore = pL.orientation;
                sec.rightDirectionScore = pR.orientation;

                sec.leftPoint = cv::Point2d(pL.x, pL.y);
                sec.rightPoint = cv::Point2d(pR.x, pR.y);
                sec.centerPoint = axisPt + v * sec.centerT;

                sec.centerStable = (std::abs(sec.centerT) <= m_config.maxCenterOffset);
                sec.trackingStable = true;
                sec.edgePairCoherent = true;
                sec.valid = (sec.centerStable && sec.trackingStable && sec.edgePairCoherent);
            }
        }
        allSections.push_back(sec);
    }

    prof.sections = allSections;
    int totalSections = static_cast<int>(allSections.size());
    if (totalSections == 0) return prof;

    int validCount = 0;
    int invalidCount = 0;
    int leftValidCount = 0;
    int rightValidCount = 0;

    double sumWidth = 0.0;
    double sumEdgeStrength = 0.0;
    double sumWidthCont = 0.0;
    double sumCenterCont = 0.0;
    double sumLeftCont = 0.0;
    double sumRightCont = 0.0;
    double sumPairMotion = 0.0;
    double sumAxisCenter = 0.0;
    double sumOrientation = 0.0;
    int contPairs = 0;
    int jumpCount = 0;

    int longestStreak = 0;
    int curStreak = 0;
    int maxGap = 0;
    int curGap = 0;

    double minValidR = 9999.0;
    double maxValidR = 0.0;

    std::vector<double> widths;
    std::vector<double> centerTVals;
    std::vector<RadialSection> validSecs;

    for (int i = 0; i < totalSections; ++i) {
        const auto& sec = allSections[i];
        if (sec.leftValid) leftValidCount++;
        if (sec.rightValid) rightValidCount++;

        if (sec.valid) {
            validCount++;
            validSecs.push_back(sec);
            sumWidth += sec.width;
            widths.push_back(sec.width);
            sumEdgeStrength += (sec.leftStrength + sec.rightStrength) * 0.5;
            centerTVals.push_back(sec.centerT);

            // Axis center consistency: centerT should stay close to axis (0.0)
            double sAxis = 1.0 / (1.0 + 0.20 * sec.centerT * sec.centerT);
            sumAxisCenter += sAxis;

            // Orientation consistency: gradients perpendicular to axis
            double sOrient = (sec.leftDirectionScore + sec.rightDirectionScore) * 0.5;
            sumOrientation += sOrient;

            if (sec.radius < minValidR) minValidR = sec.radius;
            if (sec.radius > maxValidR) maxValidR = sec.radius;

            curStreak++;
            if (curStreak > longestStreak) longestStreak = curStreak;

            if (curGap > maxGap) maxGap = curGap;
            curGap = 0;

            if (validSecs.size() > 1) {
                const auto& prev = validSecs[validSecs.size() - 2];
                double dL = std::abs(sec.leftT - prev.leftT);
                double dR = std::abs(sec.rightT - prev.rightT);

                if (dL > m_config.maxTrackingJumpPx * 0.75 || dR > m_config.maxTrackingJumpPx * 0.75) {
                    jumpCount++;
                }

                // Left & Right independent continuity
                sumLeftCont += std::max(0.0, 1.0 - dL / m_config.maxTrackingJumpPx);
                sumRightCont += std::max(0.0, 1.0 - dR / m_config.maxTrackingJumpPx);

                // Pair motion consistency: |dL - dR|
                double dMotion = std::abs((sec.leftT - prev.leftT) - (sec.rightT - prev.rightT));
                sumPairMotion += std::max(0.0, 1.0 - dMotion / m_config.maxWidthChange);

                // Width continuity
                double dw = std::abs(sec.width - prev.width);
                sumWidthCont += std::max(0.0, 1.0 - dw / m_config.maxWidthChange);

                // Center step continuity
                double dc = std::abs(sec.centerT - prev.centerT);
                sumCenterCont += std::max(0.0, 1.0 - dc / m_config.maxCenterChange);

                contPairs++;
            }
        } else {
            invalidCount++;
            curStreak = 0;
            curGap++;
        }
    }
    if (curGap > maxGap) maxGap = curGap;

    prof.validSectionCount = validCount;
    prof.invalidSectionCount = invalidCount;
    prof.maxGap = maxGap;

    prof.startRadius = (validCount > 0) ? minValidR : rStart;
    prof.endRadius = (validCount > 0) ? maxValidR : rStart;
    prof.reach = (validCount > 0) ? maxValidR : 0.0;
    prof.validLength = (validCount > 0) ? (maxValidR - minValidR) : 0.0;

    if (validCount > 0) {
        prof.meanWidth = sumWidth / validCount;
        std::sort(widths.begin(), widths.end());
        prof.medianWidth = widths[widths.size() / 2];
        prof.axisCenterConsistency = sumAxisCenter / validCount;
        prof.orientationConsistency = sumOrientation / validCount;
    } else {
        prof.meanWidth = 0.0;
        prof.medianWidth = 0.0;
        prof.axisCenterConsistency = 0.0;
        prof.orientationConsistency = 0.0;
    }

    // Slenderness = validLength / max(medianWidth, epsilon)
    prof.slenderness = prof.validLength / std::max(prof.medianWidth, 1e-3);

    // Persistence
    prof.bilateralPersistence = static_cast<double>(validCount) / std::max(1, totalSections);
    prof.leftPersistence = static_cast<double>(leftValidCount) / std::max(1, totalSections);
    prof.rightPersistence = static_cast<double>(rightValidCount) / std::max(1, totalSections);
    prof.edgePersistence = prof.bilateralPersistence;

    // Continuity & Jump metrics
    prof.leftContinuity = (contPairs > 0) ? (sumLeftCont / contPairs) : 0.0;
    prof.rightContinuity = (contPairs > 0) ? (sumRightCont / contPairs) : 0.0;
    prof.pairMotionConsistency = (contPairs > 0) ? (sumPairMotion / contPairs) : 0.0;
    prof.widthContinuity = (contPairs > 0) ? (sumWidthCont / contPairs) : 0.0;
    prof.trackingJumpPenalty = (contPairs > 0) ? (static_cast<double>(jumpCount) / contPairs) : 0.0;

    // Section II & XIV: Center Stability Strong Constraint
    double varCenter = 0.0;
    double minCenter = 999.0, maxCenter = -999.0;
    if (validCount > 1) {
        double avgCenter = 0.0;
        for (double ct : centerTVals) {
            avgCenter += ct;
            if (ct < minCenter) minCenter = ct;
            if (ct > maxCenter) maxCenter = ct;
        }
        avgCenter /= validCount;
        for (double ct : centerTVals) varCenter += (ct - avgCenter) * (ct - avgCenter);
        varCenter /= validCount;
    } else {
        minCenter = 0.0; maxCenter = 0.0;
    }
    double driftCenter = std::abs(maxCenter - minCenter);
    double stepCenterStab = (contPairs > 0) ? (sumCenterCont / contPairs) : 0.0;
    double globalCenterStab = 1.0 / (1.0 + 0.30 * varCenter + 0.15 * driftCenter * driftCenter);
    prof.centerStability = 0.40 * stepCenterStab + 0.60 * globalCenterStab;

    if (!centerTVals.empty()) {
        std::sort(centerTVals.begin(), centerTVals.end());
        prof.tangentialOffset = centerTVals[centerTVals.size() / 2];
    }
    prof.meanEdgeStrength = (validCount > 0) ? (sumEdgeStrength / validCount) : 0.0;
    prof.edgeStrength = prof.meanEdgeStrength;

    // Section VIII: Comprehensive Edge Pair Coherence
    prof.edgePairCoherence = 0.25 * prof.leftContinuity +
                             0.25 * prof.rightContinuity +
                             0.20 * prof.pairMotionConsistency +
                             0.15 * prof.axisCenterConsistency +
                             0.15 * prof.orientationConsistency -
                             0.50 * prof.trackingJumpPenalty;
    prof.edgePairCoherence = std::max(0.0, std::min(1.0, prof.edgePairCoherence));

    // Width trend (dW/dr)
    double widthTrendScore = 0.0;
    if (validCount >= 3) {
        int countDecreasing = 0;
        double tol = 0.5;
        for (size_t i = 0; i < validSecs.size() - 1; ++i) {
            if (validSecs[i + 1].width <= validSecs[i].width + tol) {
                countDecreasing++;
            }
        }
        double monoRatio = static_cast<double>(countDecreasing) / (validSecs.size() - 1);

        double sumR = 0.0, sumW_cov = 0.0, sumR2 = 0.0, sumRW = 0.0;
        for (const auto& s : validSecs) {
            sumR += s.radius;
            sumW_cov += s.width;
            sumR2 += s.radius * s.radius;
            sumRW += s.radius * s.width;
        }
        double denom = validCount * sumR2 - sumR * sumR;
        double slope = (std::abs(denom) > 1e-4) ? ((validCount * sumRW - sumR * sumW_cov) / denom) : 0.0;
        prof.widthSlope = slope;

        double slopeScore = 0.0;
        if (slope < 0.0) {
            slopeScore = std::min(1.0, -slope * 25.0);
        }
        if (slope >= -0.04 && slope <= 0.02) {
            slopeScore = std::max(slopeScore, 0.85); // Parallel slender pointer
        } else if (slope > 0.05) {
            slopeScore = std::max(0.0, 1.0 - (slope - 0.05) * 15.0);
        }
        widthTrendScore = 0.50 * monoRatio + 0.50 * slopeScore;
        prof.taperScore = widthTrendScore;
        double lastW = validSecs.back().width;
        prof.terminalScore = std::max(0.0, std::min(1.0, (14.0 - lastW) / (14.0 - 3.5)));
    } else {
        prof.widthSlope = 0.0;
        widthTrendScore = 0.0;
        prof.taperScore = 0.0;
        prof.terminalScore = 0.0;
    }

    // Normalized components
    double maxReachPossible = rEnd;
    double reachScore = std::min(1.0, prof.reach / (maxReachPossible * 0.85));

    double maxLenPossible = rEnd - rStart;
    double validLengthScore = std::min(1.0, prof.validLength / (maxLenPossible * 0.70));

    double slendernessScore = prof.slenderness / (prof.slenderness + 12.0);

    prof.gapPenalty = std::min(1.0, static_cast<double>(prof.maxGap) / std::max(1, m_config.maxOcclusionGap));
    double edgeStrengthScore = std::min(1.0, prof.meanEdgeStrength / 60.0);
    double offsetPenalty = 1.0 / (1.0 + 0.04 * prof.tangentialOffset * prof.tangentialOffset);
    double contrastScore = std::min(1.0, std::max(0.0, prof.entityContrast / 50.0));
    double longSpanBonus = std::min(4.0, (prof.commonLength / 50.0));

    // Section XI & XVI: Final BodyScore heavily prioritizes Geometry Gate and Candidate Region
    double geomWeight = (m_config.geomWeight > 0.0) ? m_config.geomWeight : 4.0;
    prof.bodyScore = (
        geomWeight                    * prof.geometryScore +
        longSpanBonus                 +
        m_config.wEdgePairCoherence   * prof.edgePairCoherence +
        m_config.wCenterStability     * prof.centerStability +
        m_config.wAxisCenter          * prof.axisCenterConsistency +
        m_config.wOrientation         * prof.orientationConsistency +
        m_config.wReach               * reachScore +
        m_config.wValidLength         * validLengthScore +
        m_config.wSlenderness         * slendernessScore +
        m_config.wPersistence         * prof.bilateralPersistence +
        m_config.wWidthTrend          * widthTrendScore +
        m_config.wWidthContinuity     * prof.widthContinuity +
        m_config.wEdgeStrength        * edgeStrengthScore +
        m_config.wEntityContrast      * contrastScore -
        m_config.wGapPenalty          * prof.gapPenalty -
        m_config.wTrackingJumpPenalty * prof.trackingJumpPenalty
    ) * offsetPenalty;

    // Section XVI: First Layer Geometry Gate check - ABSOLUTE HARD REJECTION
    // If the candidate region failed geometric validation (not long, not parallel, not straight, or not starting at base),
    // IMMEDIATELY ZERO THE BODY SCORE! Never allow false curved tracks into candidate pool!
    if (!prof.geometryValid) {
        prof.bodyScore = 0.0;
        prof.valid = false;
        prof.directionScore = 0.0;
        return prof;
    }

    // Incoherence filter: if center drifted or pair coherence is low, heavily suppress
    if (prof.centerStability < 0.55 || prof.axisCenterConsistency < 0.50 || prof.edgePairCoherence < 0.50) {
        prof.bodyScore *= 0.30;
    }

    // Hard gate on needle body width: must strictly satisfy [minPointerWidth, maxPointerWidth]
    if (prof.validSectionCount >= 3) {
        if (prof.medianWidth < m_config.minPointerWidth || prof.medianWidth > m_config.maxPointerWidth) {
            prof.bodyScore = 0.0;
            prof.valid = false;
            prof.directionScore = 0.0;
            return prof;
        }
    }

    if (prof.validSectionCount < 4 || prof.validLength < 25.0) {
        prof.bodyScore = 0.0;
        prof.valid = false;
    } else {
        prof.valid = true;
    }
    prof.bodyScore = std::max(0.0, prof.bodyScore);
    prof.directionScore = prof.bodyScore;

    return prof;
}

DirectionProfile PointerDetector::evaluateDirectionProfile(
    const cv::Mat& gray,
    const GradientFeatureMap& features,
    const cv::Point2d& effectivePivot,
    double thetaDeg) {
    return evaluateRadialDirectionProfile(gray, features, effectivePivot, thetaDeg);
}

// -------------------------------------------------------------------------
// Evaluate Wide Counterweight Needle Tail Profile
// -------------------------------------------------------------------------
TailDirectionProfile PointerDetector::evaluateTailProfile(
    const cv::Mat& gray,
    const GradientFeatureMap& features,
    const cv::Point2d& effectivePivot,
    double thetaDeg) {

    TailDirectionProfile prof;
    double emitTheta = normalizeAngle(thetaDeg);
    prof.angle = emitTheta;

    if (!m_config.enableTailDetection || gray.empty()) {
        return prof;
    }

    double rad = emitTheta * M_PI / 180.0;
    cv::Point2d u(std::cos(rad), std::sin(rad));
    cv::Point2d v(-std::sin(rad), std::cos(rad)); // Transverse normal

    // Exclude center exclusion radius (pointerBaseRadius) from tail search as well
    double rStart = std::max(5.0, m_config.pointerBaseRadius);
    double rEnd = std::max(rStart + 20.0, std::min(m_config.tailMaxRadius, m_config.maxSearchRadius * 0.85));
    double stepR = (m_config.tailRadialStepPx > 0.0) ? m_config.tailRadialStepPx : 2.0;

    double maxHalfW = std::max(15.0, m_config.tailCorridorHalfWidth);
    double minW = std::max(3.0, m_config.tailMinWidth);
    double maxW = std::max(minW + 5.0, m_config.tailMaxWidth);
    double stepT = 0.5;

    std::vector<RadialSection> validSections;
    double sumEdgeStrength = 0.0;
    double sumCenterOffset = 0.0;
    double sumTailInside = 0.0;
    double sumTailOutside = 0.0;
    std::vector<double> tailInsides;
    std::vector<double> widths;

    int consecutiveMisses = 0;
    int maxConsecutiveGap = (m_config.maxConsecutiveGapSamples > 0) ? m_config.maxConsecutiveGapSamples : 5;
    double maxStartGap = (m_config.maxStartBaseGapPx > 0.0) ? m_config.maxStartBaseGapPx : 16.0;

    float minPeakScore = static_cast<float>(std::min(30.0, std::max(18.0, m_config.rawGradientThreshold * 0.55)));

    for (double r = rStart; r <= rEnd; r += stepR) {
        // Hub start proximity check: tail must start close to pointerBaseRadius
        if (validSections.empty() && (r - rStart) > maxStartGap) {
            break;
        }

        cv::Point2d rayCenter = effectivePivot + u * r;

        struct LocalPeak {
            double t;
            double mag;
            double dirScore;
            double score;
            double intensity;
            float stepContrast;
        };

        std::vector<LocalPeak> leftPeaks;
        std::vector<LocalPeak> rightPeaks;

        int numT = static_cast<int>(std::round((maxHalfW * 2.0) / stepT));
        std::vector<LocalPeak> sliceSamples;
        sliceSamples.reserve(numT + 1);

        for (int i = 0; i <= numT; ++i) {
            double t = -maxHalfW + i * stepT;
            cv::Point2d pt = rayCenter + v * t;

            if (pt.x < 3.0 || pt.x >= gray.cols - 4.0 || pt.y < 3.0 || pt.y >= gray.rows - 4.0) {
                sliceSamples.push_back({ t, 0.0, 0.0, 0.0, 0.0, 0.0f });
                continue;
            }

            float mag = sampleBilinear(features.gradMag, static_cast<float>(pt.x), static_cast<float>(pt.y));
            float ux = sampleBilinear(features.gradUnitX, static_cast<float>(pt.x), static_cast<float>(pt.y));
            float uy = sampleBilinear(features.gradUnitY, static_cast<float>(pt.x), static_cast<float>(pt.y));
            float intensity = sampleBilinear(gray, static_cast<float>(pt.x), static_cast<float>(pt.y));

            float dirScore = std::abs(static_cast<float>(ux * v.x + uy * v.y));
            float instantScore = mag * dirScore;

            // 跨度 4~5 像素宏观阶跃差分 (Step Contrast)
            // 当过渡带平缓（如 4~5 像素爬坡）时，瞬时梯度会被稀释，但跨越 ±2.5px 的总阶跃依然显著
            cv::Point2d ptOut = (t < 0.0) ? (pt - v * 2.5) : (pt + v * 2.5);
            cv::Point2d ptIn  = (t < 0.0) ? (pt + v * 2.5) : (pt - v * 2.5);
            float iOut = sampleBilinear(gray, static_cast<float>(ptOut.x), static_cast<float>(ptOut.y));
            float iIn  = sampleBilinear(gray, static_cast<float>(ptIn.x), static_cast<float>(ptIn.y));
            float stepContrast = m_config.requireDarkPointer ? (iOut - iIn) : std::abs(iOut - iIn);
            stepContrast = std::max(0.0f, stepContrast);

            // 综合边缘强度：融合瞬时微观梯度与宏观跨步长阶跃
            float score = std::max(instantScore, stepContrast * std::max(0.40f, dirScore) * 1.25f);

            sliceSamples.push_back({ t, mag, dirScore, score, intensity, stepContrast });
        }

        double tailMinNormalAlign = (m_config.tailMinGradientNormalAlignment > 0.0) ? m_config.tailMinGradientNormalAlignment : 0.50;

        // 识别边缘峰值：支持陡峭微观边缘，也支持 4~5 像素宽阶跃（stepContrast >= 18.0）的平缓过渡边缘
        for (int i = 1; i < static_cast<int>(sliceSamples.size()) - 1; ++i) {
            const auto& cur = sliceSamples[i];
            bool peakCondition = (cur.score >= sliceSamples[i - 1].score && cur.score >= sliceSamples[i + 1].score);
            bool alignCondition = (cur.dirScore >= tailMinNormalAlign || (cur.stepContrast >= 20.0f && cur.dirScore >= 0.35f));
            bool strengthCondition = (cur.score >= minPeakScore || cur.stepContrast >= 20.0f);

            if (alignCondition && strengthCondition && peakCondition) {
                if (cur.t < -minW * 0.30 && cur.t >= -maxHalfW + 1.0) {
                    leftPeaks.push_back(cur);
                } else if (cur.t > minW * 0.30 && cur.t <= maxHalfW - 1.0) {
                    rightPeaks.push_back(cur);
                }
            }
        }

        double bestPairScore = -1.0;
        LocalPeak bestL{0.0, 0.0, 0.0, -1.0, 0.0, 0.0f}, bestR{0.0, 0.0, 0.0, -1.0, 0.0, 0.0f};
        double bestInside = 0.0, bestOutside = 0.0;

        double maxAllowedCenterT = std::max(m_config.maxCenterOffset * 1.2, maxHalfW * 0.45);

        auto backgroundGray = [&](bool left) {
            std::vector<double> values;
            for (const auto& sample : sliceSamples) {
                if ((left && sample.t < -minW) || (!left && sample.t > minW))
                    values.push_back(sample.intensity);
            }
            if (values.empty()) return 255.0;
            std::sort(values.begin(), values.end());
            return values[static_cast<size_t>((values.size() - 1) * 0.75)];
        };
        const double backgroundL = backgroundGray(true);
        const double backgroundR = backgroundGray(false);

        // Locate the outer transition into the local dial background. An
        // internal gradient maximum alone cannot define the silhouette when
        // the counterweight has a reflective rim.
        if (m_config.requireDarkPointer) {
            leftPeaks.clear();
            rightPeaks.clear();
            const int middle = numT / 2;
            const double coreGray = sliceSamples[middle].intensity;
            for (int sign : {-1, 1}) {
                const double background = sign < 0 ? backgroundL : backgroundR;
                const double threshold = (background + coreGray) * 0.5;
                for (int i = middle + sign; i > 1 && i < numT - 1; i += sign) {
                    if (sliceSamples[i].intensity < threshold) continue;
                    // Cross the reflective rim and its shadow, then require
                    // the actual dial background within a bounded distance.
                    int edge = i;
                    bool reachedBackground = false;
                    for (int j = i; std::abs(j - i) <= 40 && j > 1 && j < numT - 1; j += sign) {
                        if (sliceSamples[j].intensity >= background - 12.0) {
                            int supported = 0;
                            for (int k = 1; k <= 12; ++k) {
                                int index = j + sign * k;
                                if (index >= 0 && index <= numT && sliceSamples[index].intensity >= background - 12.0) ++supported;
                            }
                            if (supported < 9) continue;
                            edge = j;
                            reachedBackground = true;
                            break;
                        }
                    }
                    if (!reachedBackground) break;
                    auto candidate = sliceSamples[edge];
                    candidate.score = std::max(candidate.score, 18.0);
                    if (std::abs(candidate.t) > minW * 0.30) {
                        if (sign < 0) leftPeaks.push_back(candidate);
                        else rightPeaks.push_back(candidate);
                    }
                    break;
                }
            }
        }

        for (const auto& l : leftPeaks) {
            for (const auto& rk : rightPeaks) {
                double w = rk.t - l.t;
                if (w < minW || w > maxW) continue;

                double centerT = (l.t + rk.t) * 0.5;
                if (std::abs(centerT) > maxAllowedCenterT) continue;

                // Counterweights can be asymmetric about the body axis.
                double sCenter = 1.0 / (1.0 + 0.002 * centerT * centerT);
                double sSym = std::min(1.0, 1.0 - std::abs(rk.t + l.t) / (w + 1e-3));
                double edgeScore = (l.score + rk.score) * 0.5;

                int idxL = static_cast<int>(std::round((l.t + maxHalfW) / stepT));
                int idxR = static_cast<int>(std::round((rk.t + maxHalfW) / stepT));
                idxL = std::max(0, std::min(static_cast<int>(sliceSamples.size()) - 1, idxL));
                idxR = std::max(0, std::min(static_cast<int>(sliceSamples.size()) - 1, idxR));

                std::vector<double> insideInts;
                insideInts.reserve(std::max(0, idxR - idxL));
                for (int k = idxL + 1; k < idxR; ++k) {
                    insideInts.push_back(sliceSamples[k].intensity);
                }

                double robustInside = 0.0;
                if (!insideInts.empty()) {
                    std::sort(insideInts.begin(), insideInts.end());
                    // 采用第 35% 分位数作为深色实体的代表灰度，抗内部微弱反光
                    size_t pIdx = std::min(insideInts.size() - 1, static_cast<size_t>(insideInts.size() * 0.35));
                    robustInside = insideInts[pIdx];
                } else {
                    robustInside = (l.intensity + rk.intensity) * 0.5;
                }

                // Verify each edge independently against a sustained background
                // band, rather than calling the edge pixel itself "outside".
                auto bandGray = [&](double t, double sign, double first, double last) {
                    std::vector<double> values;
                    for (double d = first; d <= last; d += stepT) {
                        double sampleT = t + sign * d;
                        if (std::abs(sampleT) > maxHalfW) continue;
                        cv::Point2d p = rayCenter + v * sampleT;
                        if (p.x < 1 || p.y < 1 || p.x >= gray.cols - 2 || p.y >= gray.rows - 2) continue;
                        values.push_back(sampleBilinear(gray, static_cast<float>(p.x), static_cast<float>(p.y)));
                    }
                    if (values.size() < 5) return -1.0;
                    std::sort(values.begin(), values.end());
                    return values[values.size() / 2];
                };
                double outsideL = bandGray(l.t, -1, 3.0, 9.0);
                double outsideR = bandGray(rk.t, 1, 3.0, 9.0);
                double insideL = bandGray(l.t, 1, 2.0, std::min(7.0, w * 0.4));
                double insideR = bandGray(rk.t, -1, 2.0, std::min(7.0, w * 0.4));
                if (outsideL < 0 || outsideR < 0 || insideL < 0 || insideR < 0) continue;
                double minContrast = std::max(10.0, m_config.minPointerDarkContrast);
                if (m_config.requireDarkPointer &&
                    (outsideL - insideL < minContrast || outsideR - insideR < minContrast ||
                     outsideL - robustInside < minContrast * 2 || outsideR - robustInside < minContrast * 2)) continue;
                double avgOutside = (outsideL + outsideR) * 0.5;
                double contrast = m_config.requireDarkPointer ? (avgOutside - robustInside) : std::abs(avgOutside - robustInside);
                const double axisGray = sliceSamples[numT / 2].intensity;
                if (m_config.requireDarkPointer && axisGray > robustInside + contrast * 0.20) continue;

                if (m_config.requireDarkPointer && (contrast < 4.0 || robustInside > m_config.maxPointerInsideGray)) {
                    continue; // Skip slice: needle tail must be a dark entity
                }

                // 核心防线 1：深色填充率检验 (Dark Pixel Fill Ratio)
                // 针尾配重是深色实心块，区间内绝大多数像素必须显著低于表盘白底
                // 彻底杜绝因宽步长搜索而把外部大面积表盘白底包入针尾
                int darkCount = 0;
                int totalIn = idxR - idxL - 1;
                double darkThreshold = (avgOutside + robustInside) * 0.5;
                for (int k = idxL + 1; k < idxR; ++k) {
                    if (sliceSamples[k].intensity <= darkThreshold) {
                        darkCount++;
                    }
                }
                double darkRatio = totalIn > 0 ? (static_cast<double>(darkCount) / totalIn) : 0.0;
                if (darkRatio < 0.65) {
                    continue; // 内部超过 35% 为亮色背景，拒绝外扩假边缘！
                }
                // A text stroke beyond the silhouette must not be joined to
                // the tail across a strip of dial background.
                int backgroundRun = 0;
                bool crossesBackground = false;
                const double backgroundThreshold = std::min(backgroundL, backgroundR) - 12.0;
                for (int k = idxL + 12; k <= idxR - 12; ++k) {
                    backgroundRun = sliceSamples[k].intensity >= backgroundThreshold ? backgroundRun + 1 : 0;
                    if (backgroundRun * stepT >= 3.0) { crossesBackground = true; break; }
                }
                if (m_config.requireDarkPointer && crossesBackground) continue;

                // Penalize jumps, without forcing a widening tail into a
                // narrow, symmetric body shape. Missing sections reset tracking.
                double continuity = 1.0;
                // Do not restart on an isolated text/rim edge after leaving
                // the counterweight. Tail occlusion gaps are short; the body
                // detector's much longer text-bridging budget is unsuitable.
                if (!validSections.empty() && consecutiveMisses > 4 &&
                    w < validSections.back().width * 0.85) continue;
                if (!validSections.empty() && consecutiveMisses == 0) {
                    const auto& previous = validSections.back();
                    double jump = std::max(std::abs(l.t - previous.leftT), std::abs(rk.t - previous.rightT));
                    continuity = 1.0 / (1.0 + 0.03 * jump * jump);
                }
                double boundaryContrast = std::min(outsideL - insideL, outsideR - insideR);

                double pairScore = edgeScore * (0.4 + 0.6 * sCenter) * (0.5 + 0.5 * sSym)
                                   * (1.0 + std::min(1.0, std::max(0.0, contrast) / 25.0))
                                   * (0.4 + 0.6 * darkRatio)
                                   * continuity * (0.5 + std::min(2.0, std::max(0.0, boundaryContrast) / 25.0));

                if (pairScore > bestPairScore) {
                    bestPairScore = pairScore;
                    bestL = l;
                    bestR = rk;
                    bestInside = robustInside;
                    bestOutside = avgOutside;
                }
            }
        }

        if (bestPairScore > 0.0) {
            consecutiveMisses = 0;
            RadialSection sec;
            sec.radius = r;
            sec.valid = true;
            sec.leftValid = true;
            sec.rightValid = true;
            sec.leftT = bestL.t;
            sec.rightT = bestR.t;
            sec.width = bestR.t - bestL.t;
            sec.centerT = (bestL.t + bestR.t) * 0.5;
            sec.leftStrength = bestL.mag;
            sec.rightStrength = bestR.mag;
            sec.leftDirectionScore = bestL.dirScore;
            sec.rightDirectionScore = bestR.dirScore;
            sec.leftPoint = rayCenter + v * bestL.t;
            sec.rightPoint = rayCenter + v * bestR.t;
            sec.centerPoint = rayCenter + v * sec.centerT;
            validSections.push_back(sec);

            sumEdgeStrength += (bestL.mag + bestR.mag) * 0.5;
            sumCenterOffset += std::abs(sec.centerT);
            sumTailInside += bestInside;
            sumTailOutside += bestOutside;
            tailInsides.push_back(bestInside);
            widths.push_back(sec.width);
        } else {
            consecutiveMisses++;
            if (!validSections.empty() && consecutiveMisses > maxConsecutiveGap) {
                // Tail search terminated: consecutive missing slices exceeded tolerance limit
                break;
            }
        }
    }

    prof.sections = validSections;
    prof.validSectionCount = static_cast<int>(validSections.size());

    if (prof.validSectionCount >= 3) {
        prof.reach = validSections.back().radius;
        double sumW = 0.0;
        for (double w : widths) sumW += w;
        prof.meanWidth = sumW / widths.size();

        std::vector<double> sortedW = widths;
        std::sort(sortedW.begin(), sortedW.end());
        prof.medianWidth = sortedW[sortedW.size() / 2];

        double avgCenterOffset = sumCenterOffset / prof.validSectionCount;
        prof.symmetryScore = 1.0 / (1.0 + 0.002 * avgCenterOffset * avgCenterOffset);

        double avgEdgeStr = sumEdgeStrength / prof.validSectionCount;
        prof.contrastScore = std::min(1.5, avgEdgeStr / 40.0);

        if (!tailInsides.empty()) {
            std::vector<double> sortedTIns = tailInsides;
            std::sort(sortedTIns.begin(), sortedTIns.end());
            // 取第 35% 分位数作为针尾深色主体代表值，排除局部包含白底的影响
            size_t tIdx = std::min(sortedTIns.size() - 1, static_cast<size_t>(sortedTIns.size() * 0.35));
            prof.meanInsideGray = sortedTIns[tIdx];
        } else {
            prof.meanInsideGray = sumTailInside / prof.validSectionCount;
        }
        prof.meanOutsideGray = sumTailOutside / prof.validSectionCount;
        prof.meanContrast = prof.meanOutsideGray - prof.meanInsideGray;

        // 1. Calculate linear regression slope dW/dr along radius from center outward
        int N = static_cast<int>(validSections.size());
        double sumR = 0.0, sumW_cov = 0.0, sumR2 = 0.0, sumRW = 0.0;
        int countIncreasing = 0;
        double tol = 0.5; // pixel tolerance for discrete sampling noise
        for (size_t i = 0; i < validSections.size(); ++i) {
            double r_i = validSections[i].radius;
            double w_i = validSections[i].width;
            sumR += r_i;
            sumW_cov += w_i;
            sumR2 += r_i * r_i;
            sumRW += r_i * w_i;
            if (i + 1 < validSections.size()) {
                if (validSections[i + 1].width >= validSections[i].width - tol) {
                    countIncreasing++;
                }
            }
        }
        double denom = N * sumR2 - sumR * sumR;
        double slope = (std::abs(denom) > 1e-4) ? ((N * sumRW - sumR * sumW_cov) / denom) : 0.0;
        prof.widthSlope = slope;

        double monoRatio = (N > 1) ? (static_cast<double>(countIncreasing) / (N - 1)) : 0.0;

        // Widening score:
        // A normal needle tail widens outward from the center hub (dW/dr > 0).
        // If slope is negative (narrowing outward), it behaves like a pointer tip, not a tail!
        double slopeScore = 0.0;
        if (slope >= 0.05) {
            slopeScore = std::min(1.0, 0.40 + slope * 3.0); // Distinct outward widening
        } else if (slope >= -0.02) {
            slopeScore = 0.40; // Relatively flat / slightly widening
        } else {
            // Negative slope: getting narrower outward -> heavily penalize
            slopeScore = std::max(0.0, 0.20 + slope * 5.0);
        }
        prof.wideningScore = 0.40 * monoRatio + 0.60 * slopeScore;

        // Compare inner-region average width vs outer-region and peak width
        double innerW = validSections.front().width;
        double outerW = validSections.back().width;
        if (validSections.size() >= 4) {
            innerW = (validSections[0].width + validSections[1].width) * 0.5;
            outerW = (validSections[validSections.size() - 1].width + validSections[validSections.size() - 2].width) * 0.5;
        }

        double maxW_found = 0.0;
        for (const auto& s : validSections) {
            if (s.width > maxW_found) maxW_found = s.width;
        }

        // Geometric criterion: normal needle tail expands outward from the hub in its main body
        // (either overall positive slope, outer width >= inner width, or peak width clearly exceeds base width)
        bool isOutwardWidening = ((outerW >= innerW - 2.0) || (maxW_found >= innerW * 1.12)) && (slope >= -0.06);

        double tailLen = prof.reach - validSections.front().radius;
        if (tailLen >= 8.0 && prof.meanWidth >= minW && isOutwardWidening) {
            if (m_config.requireDarkPointer && (prof.meanInsideGray > m_config.maxPointerInsideGray || prof.meanContrast < 4.0)) {
                prof.tailScore = 0.0;
                prof.valid = false;
            } else {
                double widthBonus = std::min(1.5, prof.medianWidth / 18.0);
                double spanNorm = std::min(1.0, static_cast<double>(prof.validSectionCount) / 25.0);
                prof.tailScore = (spanNorm * 1.5) * prof.symmetryScore * prof.contrastScore * widthBonus * (0.4 + 0.8 * prof.wideningScore);
                prof.valid = (prof.tailScore > 0.35 && prof.wideningScore >= 0.30);
            }
        } else {
            prof.tailScore = 0.0;
            prof.valid = false;
        }
    }

    return prof;
}

// -------------------------------------------------------------------------
// Evaluate Non-Directional Candidate Axis in [0, 180°)
// -------------------------------------------------------------------------
PointerAxisCandidate PointerDetector::evaluateCandidateAxis(
    const cv::Mat& gray,
    const GradientFeatureMap& features,
    const cv::Point2d& effectivePivot,
    double thetaDeg) {

    PointerAxisCandidate cand;
    double emitTheta = normalizeAngle(thetaDeg);
    cand.theta = std::fmod(emitTheta, 180.0);

    RadialDirectionProfile profA = evaluateRadialDirectionProfile(gray, features, effectivePivot, cand.theta);
    RadialDirectionProfile profB = evaluateRadialDirectionProfile(gray, features, effectivePivot, cand.theta + 180.0);

    TailDirectionProfile tailA = evaluateTailProfile(gray, features, effectivePivot, cand.theta);
    TailDirectionProfile tailB = evaluateTailProfile(gray, features, effectivePivot, cand.theta + 180.0);

    double scoreAB = profA.bodyScore + (tailB.valid ? m_config.wTailScore * tailB.tailScore : 0.0);
    double scoreBA = profB.bodyScore + (tailA.valid ? m_config.wTailScore * tailA.tailScore : 0.0);

    cand.totalScore = std::max(scoreAB, scoreBA);
    cand.bodyScore = cand.totalScore;

    bool aIsPrimary = (scoreAB >= scoreBA);
    const auto& primary = aIsPrimary ? profA : profB;

    cand.sections = primary.sections;
    cand.reach = primary.reach;
    cand.coverageLength = primary.validLength;
    cand.meanWidth = primary.meanWidth;
    cand.medianWidth = primary.medianWidth;
    cand.slenderness = primary.slenderness;
    cand.edgeStrength = primary.meanEdgeStrength;
    cand.edgePersistence = primary.bilateralPersistence;
    cand.bilateralPersistence = primary.bilateralPersistence;
    cand.widthSlope = primary.widthSlope;
    cand.widthContinuity = primary.widthContinuity;
    cand.centerStability = primary.centerStability;
    cand.leftEdgeStability = primary.centerStability;
    cand.rightEdgeStability = primary.centerStability;

    // Edge Pair Coherence metrics
    cand.leftContinuity = primary.leftContinuity;
    cand.rightContinuity = primary.rightContinuity;
    cand.pairMotionConsistency = primary.pairMotionConsistency;
    cand.axisCenterConsistency = primary.axisCenterConsistency;
    cand.orientationConsistency = primary.orientationConsistency;
    cand.trackingJumpPenalty = primary.trackingJumpPenalty;
    cand.edgePairCoherence = primary.edgePairCoherence;

    cand.validSectionCount = primary.validSectionCount;
    cand.maxGapLength = primary.maxGap;
    cand.startRadius = primary.startRadius;
    cand.endRadius = primary.endRadius;
    cand.tangentialOffset = primary.tangentialOffset;

    // Backward compatibility aliases
    cand.edgeScore = primary.meanEdgeStrength;
    cand.widthContinuityScore = primary.widthContinuity;
    cand.centerContinuityScore = primary.centerStability;
    cand.leftContinuityScore = primary.centerStability;
    cand.rightContinuityScore = primary.centerStability;
    cand.coverageScore = primary.validLength / std::max(1.0, m_config.maxSearchRadius - m_config.pointerBaseRadius);
    cand.occlusionScore = std::max(0.0, 1.0 - static_cast<double>(primary.maxGap) / (m_config.maxOcclusionGap * 2.0));

    return cand;
}

// -------------------------------------------------------------------------
// Evaluate TipLikelihood for Needle Tip Identification
// -------------------------------------------------------------------------
double PointerDetector::evaluateTipLikelihood(
    const std::vector<PointerCrossSection>& sections,
    double& outTaperScore,
    double& outConvergenceScore,
    double& outTerminalScore,
    double& outContinuityScore) {

    std::vector<PointerCrossSection> validSecs;
    for (const auto& s : sections) {
        if (s.valid) validSecs.push_back(s);
    }

    int n = static_cast<int>(validSecs.size());
    if (n < 4) {
        outTaperScore = 0.0;
        outConvergenceScore = 0.0;
        outTerminalScore = 0.0;
        outContinuityScore = 0.0;
        return 0.0;
    }

    double covLen = validSecs.back().radius - validSecs.front().radius;
    double sumW = 0.0;
    for (const auto& s : validSecs) sumW += s.width;
    double avgW = sumW / n;
    double slender = covLen / std::max(1.0, avgW);

    // 1. taperScore: Monotonic width contraction trend towards the outer tip W(r)
    int countDecreasing = 0;
    double tol = 0.6;
    for (int i = 0; i < n - 1; ++i) {
        if (validSecs[i + 1].width <= validSecs[i].width + tol) {
            countDecreasing++;
        }
    }
    double monoRatio = static_cast<double>(countDecreasing) / (n - 1);

    double sumR = 0.0, sumW_cov = 0.0, sumR2 = 0.0, sumRW = 0.0;
    for (const auto& s : validSecs) {
        sumR += s.radius;
        sumW_cov += s.width;
        sumR2 += s.radius * s.radius;
        sumRW += s.radius * s.width;
    }
    double denom = n * sumR2 - sumR * sumR;
    double slope = (std::abs(denom) > 1e-4) ? ((n * sumRW - sumR * sumW_cov) / denom) : 0.0;
    double negSlopeScore = (slope < 0.0) ? std::min(1.0, -slope * 28.0) : 0.0;
    outTaperScore = std::min(1.0, 0.55 * monoRatio + 0.45 * negSlopeScore);

    int baseK = std::min(4, std::max(2, n / 4));
    int tipK = std::min(4, std::max(2, n / 4));

    double baseWidthSum = 0.0;
    for (int i = 0; i < baseK; ++i) baseWidthSum += validSecs[i].width;
    double baseW = baseWidthSum / baseK;

    double tipWidthSum = 0.0;
    for (int i = n - tipK; i < n; ++i) tipWidthSum += validSecs[i].width;
    double tipW = tipWidthSum / tipK;

    double contractionRatio = tipW / std::max(1.0, baseW);
    if (contractionRatio <= 0.35) {
        outConvergenceScore = 1.0;
    } else if (contractionRatio <= 0.85) {
        outConvergenceScore = (0.85 - contractionRatio) / (0.85 - 0.35);
    } else {
        outConvergenceScore = 0.0;
    }

    double lastW = validSecs.back().width;
    double acuteTipScore = std::max(0.0, std::min(1.0, (14.0 - lastW) / (14.0 - 4.0)));
    outTerminalScore = acuteTipScore;

    outContinuityScore = std::min(1.0, static_cast<double>(n) / sections.size());

    double slenderFactor = slender / (slender + 10.0);
    double lengthFactor = std::min(1.0, covLen / 120.0);

    double likelihood = 0.30 * outTaperScore +
                        0.25 * outConvergenceScore +
                        0.20 * outTerminalScore +
                        0.15 * slenderFactor +
                        0.10 * lengthFactor;

    return likelihood;
}

// -------------------------------------------------------------------------
// Main Detection Pipeline
// -------------------------------------------------------------------------
PointerDetectionResult PointerDetector::detect(const cv::Mat& gray, const cv::Point2d& dialCenter, cv::Mat* debugVis) {
    PointerDetectionResult res;
    auto lastStage = std::chrono::steady_clock::now();
    auto mark = [&](const char* name) {
        const auto now = std::chrono::steady_clock::now();
        res.stageTimings[name] = std::chrono::duration<double, std::milli>(now-lastStage).count();
        lastStage = now;
    };
    res.success = false;

    if (gray.empty()) {
        res.failureReason = "输入图像为空 (Input grayscale image is empty)";
        res.detailedDebugText = "❌ [PointerDetector] 失败原因: 输入灰度图像为空 (cv::Mat::empty)";
        return res;
    }

    cv::Mat grayImg;
    if (gray.channels() == 3) {
        cv::cvtColor(gray, grayImg, cv::COLOR_BGR2GRAY);
    } else {
        grayImg = gray.clone();
    }

    // Section XXI: Effective pivot = dialCenter + pivotOffset
    cv::Point2d effectivePivot = dialCenter + m_pivotOffset;
    res.effectivePivot = effectivePivot;

    // Global single-pass Scharr gradient calculation
    GradientFeatureMap features = computeGradientFeatures(grayImg);

    mark("gradient_preprocessing_ms");
    // Phase 1 Non-directional axis search theta in [0, 180°)
    double angleStep = std::max(0.5, m_config.angleStep > 0.0 ? m_config.angleStep : 1.0);
    int numSteps = static_cast<int>(std::round(180.0 / angleStep));

    std::vector<PointerAxisCandidate> allCandidates(numSteps);

    // Parallel multi-threaded coarse search across angles
    cv::parallel_for_(cv::Range(0, numSteps), [&](const cv::Range& range) {
        for (int i = range.start; i < range.end; ++i) {
            double theta = i * angleStep;
            RadialDirectionProfile profA = evaluateRadialDirectionProfile(grayImg, features, effectivePivot, theta);
            RadialDirectionProfile profB = evaluateRadialDirectionProfile(grayImg, features, effectivePivot, theta + 180.0);

            TailDirectionProfile tailA = evaluateTailProfile(grayImg, features, effectivePivot, theta);
            TailDirectionProfile tailB = evaluateTailProfile(grayImg, features, effectivePivot, theta + 180.0);

            double scoreAB = 0.0;
            if (profA.validSectionCount >= 3) {
                scoreAB = profA.bodyScore + (tailB.valid ? m_config.wTailScore * tailB.tailScore : 0.0);
            }
            double scoreBA = 0.0;
            if (profB.validSectionCount >= 3) {
                scoreBA = profB.bodyScore + (tailA.valid ? m_config.wTailScore * tailA.tailScore : 0.0);
            }
            if (profA.validSectionCount < 3 && profB.validSectionCount < 3) {
                scoreAB = profA.bodyScore;
                scoreBA = profB.bodyScore;
            }

            double axisScore = std::max(scoreAB, scoreBA);

            bool aIsPrimary = (scoreAB >= scoreBA);
            const auto& primary = aIsPrimary ? profA : profB;

            PointerAxisCandidate cand;
            cand.theta = theta;
            cand.sections = primary.sections;
            cand.reach = primary.reach;
            cand.coverageLength = primary.validLength;
            cand.meanWidth = primary.meanWidth;
            cand.medianWidth = primary.medianWidth;
            cand.slenderness = primary.slenderness;
            cand.edgeStrength = primary.meanEdgeStrength;
            cand.edgePersistence = primary.bilateralPersistence;
            cand.bilateralPersistence = primary.bilateralPersistence;
            cand.widthSlope = primary.widthSlope;
            cand.widthContinuity = primary.widthContinuity;
            cand.centerStability = primary.centerStability;
            cand.leftEdgeStability = primary.centerStability;
            cand.rightEdgeStability = primary.centerStability;
            cand.bodyScore = axisScore;
            cand.totalScore = axisScore;

            cand.validSectionCount = primary.validSectionCount;
            cand.maxGapLength = primary.maxGap;
            cand.startRadius = primary.startRadius;
            cand.endRadius = primary.endRadius;
            cand.tangentialOffset = primary.tangentialOffset;

            cand.leftLength = primary.leftLength;
            cand.rightLength = primary.rightLength;
            cand.commonLength = primary.commonLength;
            cand.leftStraightness = primary.leftStraightness;
            cand.rightStraightness = primary.rightStraightness;
            cand.parallelism = primary.parallelism;
            cand.baseProximity = primary.baseProximity;
            cand.leftSupportDensity = primary.leftSupportDensity;
            cand.rightSupportDensity = primary.rightSupportDensity;
            cand.edgeSupportDensity = primary.edgeSupportDensity;
            cand.geometryScore = primary.geometryScore;
            cand.geometryValid = primary.geometryValid;

            cand.leftContinuity = primary.leftContinuity;
            cand.rightContinuity = primary.rightContinuity;
            cand.pairMotionConsistency = primary.pairMotionConsistency;
            cand.axisCenterConsistency = primary.axisCenterConsistency;
            cand.orientationConsistency = primary.orientationConsistency;
            cand.trackingJumpPenalty = primary.trackingJumpPenalty;
            cand.edgePairCoherence = primary.edgePairCoherence;

            allCandidates[i] = cand;
        }
    });

    double bestCoarseTheta = 0.0;
    double maxCoarseScore = -1.0;
    PointerAxisCandidate bestCoarseCandidate;

    for (int i = 0; i < numSteps; ++i) {
        if (allCandidates[i].totalScore > maxCoarseScore) {
            maxCoarseScore = allCandidates[i].totalScore;
            bestCoarseTheta = allCandidates[i].theta;
            bestCoarseCandidate = allCandidates[i];
        }
    }

    mark("coarse_search_ms");
    // Top-10 candidate retention and formatted debug table
    std::vector<PointerAxisCandidate> sortedCandidates = allCandidates;
    std::sort(sortedCandidates.begin(), sortedCandidates.end(), [](const PointerAxisCandidate& a, const PointerAxisCandidate& b) {
        return a.totalScore > b.totalScore;
    });
    int topN = std::min(10, static_cast<int>(sortedCandidates.size()));
    res.topCandidates.assign(sortedCandidates.begin(), sortedCandidates.begin() + topN);

    std::cout << "\n================================================================================================================================================================================================" << std::endl;
    std::cout << "[PointerDetector] Top 10 Candidate Axes Evaluation (Long Parallel Edge Pair -> Candidate Region -> Width Validation):" << std::endl;
    std::cout << "Rank | Candidate |  Angle  | LeftLen | RightLen | ComLen | L-Straight | R-Straight | Parallel | L-Supp | R-Supp | BaseProx | MedW  |  W-Slope  | W-Cont | GeomScore | BodyScore" << std::endl;
    std::cout << "-----+-----------+---------+---------+----------+--------+------------+------------+----------+--------+--------+----------+-------+-----------+--------+-----------+----------" << std::endl;

    for (int k = 0; k < topN; ++k) {
        const auto& c = sortedCandidates[k];
        std::cout << " #" << std::left << std::setw(3) << (k + 1) << " | "
                  << "Cand #" << std::left << std::setw(4) << (k + 1) << " | "
                  << std::right << std::fixed << std::setprecision(2) << std::setw(6) << c.theta << "° | "
                  << std::setw(5) << std::setprecision(0) << c.leftLength << "px | "
                  << std::setw(6) << std::setprecision(0) << c.rightLength << "px | "
                  << std::setw(4) << std::setprecision(0) << c.commonLength << "px | "
                  << std::setw(10) << std::setprecision(2) << c.leftStraightness << " | "
                  << std::setw(10) << std::setprecision(2) << c.rightStraightness << " | "
                  << std::setw(8) << std::setprecision(2) << c.parallelism << " | "
                  << std::setw(6) << std::setprecision(2) << c.leftSupportDensity << " | "
                  << std::setw(6) << std::setprecision(2) << c.rightSupportDensity << " | "
                  << std::setw(8) << std::setprecision(2) << c.baseProximity << " | "
                  << std::setw(4) << std::setprecision(1) << c.medianWidth << "px | "
                  << std::setw(9) << std::setprecision(4) << c.widthSlope << " | "
                  << std::setw(6) << std::setprecision(2) << c.widthContinuity << " | "
                  << std::setw(9) << std::setprecision(2) << c.geometryScore << " | "
                  << std::setw(9) << std::setprecision(2) << c.totalScore
                  << std::endl;
    }
    std::cout << "================================================================================================================================================================================================\n" << std::endl;

    mark("candidate_diagnostics_ms");
    // Fine angular refinement around bestCoarseTheta
    double fineRange = 1.5;
    double fineStep = 0.05;
    double bestFineTheta = bestCoarseTheta;
    double maxFineScore = maxCoarseScore;
    PointerAxisCandidate bestFineCandidate = bestCoarseCandidate;

    int numFineSteps = static_cast<int>(std::round((fineRange * 2.0) / fineStep));
    std::vector<PointerAxisCandidate> fineCandidates(numFineSteps + 1);

    // Parallel multi-threaded fine search around best candidate
    cv::parallel_for_(cv::Range(0, numFineSteps + 1), [&](const cv::Range& range) {
        for (int i = range.start; i < range.end; ++i) {
            double theta = (bestCoarseTheta - fineRange) + i * fineStep;
            if (theta < 0.0) theta += 180.0;
            if (theta >= 180.0) theta -= 180.0;

            RadialDirectionProfile profA = evaluateRadialDirectionProfile(grayImg, features, effectivePivot, theta);
            RadialDirectionProfile profB = evaluateRadialDirectionProfile(grayImg, features, effectivePivot, theta + 180.0);

            TailDirectionProfile tailA = evaluateTailProfile(grayImg, features, effectivePivot, theta);
            TailDirectionProfile tailB = evaluateTailProfile(grayImg, features, effectivePivot, theta + 180.0);

            double scoreAB = 0.0;
            if (profA.validSectionCount >= 3) {
                scoreAB = profA.bodyScore + (tailB.valid ? m_config.wTailScore * tailB.tailScore : 0.0);
            }
            double scoreBA = 0.0;
            if (profB.validSectionCount >= 3) {
                scoreBA = profB.bodyScore + (tailA.valid ? m_config.wTailScore * tailA.tailScore : 0.0);
            }
            if (profA.validSectionCount < 3 && profB.validSectionCount < 3) {
                scoreAB = profA.bodyScore;
                scoreBA = profB.bodyScore;
            }
            double combScore = std::max(scoreAB, scoreBA);

            bool aIsPrimary = (scoreAB >= scoreBA);
            const auto& primary = aIsPrimary ? profA : profB;

            PointerAxisCandidate cand;
            cand.theta = theta;
            cand.sections = primary.sections;
            cand.reach = primary.reach;
            cand.coverageLength = primary.validLength;
            cand.meanWidth = primary.meanWidth;
            cand.medianWidth = primary.medianWidth;
            cand.slenderness = primary.slenderness;
            cand.edgeStrength = primary.meanEdgeStrength;
            cand.edgePersistence = primary.bilateralPersistence;
            cand.bilateralPersistence = primary.bilateralPersistence;
            cand.widthSlope = primary.widthSlope;
            cand.widthContinuity = primary.widthContinuity;
            cand.centerStability = primary.centerStability;
            cand.leftEdgeStability = primary.centerStability;
            cand.rightEdgeStability = primary.centerStability;
            cand.bodyScore = combScore;
            cand.totalScore = combScore;
            cand.validSectionCount = primary.validSectionCount;
            cand.startRadius = primary.startRadius;
            cand.endRadius = primary.endRadius;

            cand.leftLength = primary.leftLength;
            cand.rightLength = primary.rightLength;
            cand.commonLength = primary.commonLength;
            cand.leftStraightness = primary.leftStraightness;
            cand.rightStraightness = primary.rightStraightness;
            cand.parallelism = primary.parallelism;
            cand.baseProximity = primary.baseProximity;
            cand.leftSupportDensity = primary.leftSupportDensity;
            cand.rightSupportDensity = primary.rightSupportDensity;
            cand.edgeSupportDensity = primary.edgeSupportDensity;
            cand.geometryScore = primary.geometryScore;
            cand.geometryValid = primary.geometryValid;

            cand.leftContinuity = primary.leftContinuity;
            cand.rightContinuity = primary.rightContinuity;
            cand.pairMotionConsistency = primary.pairMotionConsistency;
            cand.axisCenterConsistency = primary.axisCenterConsistency;
            cand.orientationConsistency = primary.orientationConsistency;
            cand.trackingJumpPenalty = primary.trackingJumpPenalty;
            cand.edgePairCoherence = primary.edgePairCoherence;

            fineCandidates[i] = cand;
        }
    });

    for (int i = 0; i <= numFineSteps; ++i) {
        if (fineCandidates[i].totalScore > maxFineScore) {
            maxFineScore = fineCandidates[i].totalScore;
            bestFineTheta = fineCandidates[i].theta;
            bestFineCandidate = fineCandidates[i];
        }
    }

    res.axisAngle = bestFineTheta;

    mark("fine_search_ms");
    // Phase 2 Bidirectional Direction Analysis (+axis vs -axis)
    RadialDirectionProfile profA = evaluateRadialDirectionProfile(grayImg, features, effectivePivot, bestFineTheta);
    RadialDirectionProfile profB = evaluateRadialDirectionProfile(grayImg, features, effectivePivot, bestFineTheta + 180.0);

    TailDirectionProfile tailA = evaluateTailProfile(grayImg, features, effectivePivot, bestFineTheta);
    TailDirectionProfile tailB = evaluateTailProfile(grayImg, features, effectivePivot, bestFineTheta + 180.0);

    res.directionA = profA;
    res.directionB = profB;
    res.tailA = tailA;
    res.tailB = tailB;
    res.reachForward = profA.reach;
    res.reachBackward = profB.reach;

    double scoreAB = 0.0;
    if (profA.validSectionCount >= 3) {
        scoreAB = profA.bodyScore + (tailB.valid ? m_config.wTailScore * tailB.tailScore : 0.0);
    }
    double scoreBA = 0.0;
    if (profB.validSectionCount >= 3) {
        scoreBA = profB.bodyScore + (tailA.valid ? m_config.wTailScore * tailA.tailScore : 0.0);
    }
    if (profA.validSectionCount < 3 && profB.validSectionCount < 3) {
        scoreAB = profA.bodyScore;
        scoreBA = profB.bodyScore;
    }

    // Non-symmetric geometry polarity confirmation:
    // If A is Body and B is Tail: profA.reach should be substantially longer than tailB.reach
    double polarityBonusA = 0.0;
    double polarityBonusB = 0.0;
    if (profA.validSectionCount >= 3 && tailB.valid && profA.reach > tailB.reach * 1.3) {
        polarityBonusA += m_config.wTailPolarityBoost;
        if (tailB.medianWidth > profA.medianWidth * 1.05) {
            polarityBonusA += 1.5; // Wide tail vs slender tip confirmation
        }
    }
    if (profB.validSectionCount >= 3 && tailA.valid && profB.reach > tailA.reach * 1.3) {
        polarityBonusB += m_config.wTailPolarityBoost;
        if (tailA.medianWidth > profB.medianWidth * 1.05) {
            polarityBonusB += 1.5; // Wide tail vs slender tip confirmation
        }
    }

    res.tipLikelihoodA = scoreAB + polarityBonusA;
    res.tipLikelihoodB = scoreBA + polarityBonusB;
    res.tipLikelihoodMargin = std::abs(res.tipLikelihoodA - res.tipLikelihoodB);

    if (res.tipLikelihoodA >= res.tipLikelihoodB) {
        res.headEnd = 'A';
        res.tailEnd = 'B';
        res.pointerAngle = normalizeAngle(bestFineTheta);
        res.sections = profA.sections;
        res.startRadius = profA.startRadius;
        res.endRadius = profA.endRadius;
        res.tailProfile = tailB;
        res.tailDetected = tailB.valid;
    } else {
        res.headEnd = 'B';
        res.tailEnd = 'A';
        res.pointerAngle = normalizeAngle(bestFineTheta + 180.0);
        res.sections = profB.sections;
        res.startRadius = profB.startRadius;
        res.endRadius = profB.endRadius;
        res.tailProfile = tailA;
        res.tailDetected = tailA.valid;
    }

    if (res.tailDetected) {
        res.polarityConfidence = (res.tipLikelihoodMargin >= 1.5) ? "HIGH" : "MEDIUM";
    } else if (res.tipLikelihoodMargin < 1.2) {
        res.polarityConfidence = "LOW";
    } else if (res.tipLikelihoodMargin < 3.0) {
        res.polarityConfidence = "MEDIUM";
    } else {
        res.polarityConfidence = "HIGH";
    }

    // Print EXACT required debug output
    std::cout << "\n==================================================" << std::endl;
    std::cout << "[PointerDetector] Phase 2 Bidirectional Direction Analysis:" << std::endl;
    std::cout << "Axis Angle: " << std::fixed << std::setprecision(2) << res.axisAngle << " deg\n" << std::endl;

    std::cout << "Direction A (phi = " << std::setprecision(2) << bestFineTheta << " deg):" << std::endl;
    std::cout << "    Reach:              " << std::setprecision(1) << profA.reach << " px" << std::endl;
    std::cout << "    ValidLength:        " << std::setprecision(1) << profA.validLength << " px" << std::endl;
    std::cout << "    ValidSectionCount:  " << profA.validSectionCount << std::endl;
    std::cout << "    InvalidSectionCount:" << profA.invalidSectionCount << std::endl;
    std::cout << "    MaxGap:             " << profA.maxGap << std::endl;
    std::cout << "    MeanWidth:          " << std::setprecision(1) << profA.meanWidth << " px" << std::endl;
    std::cout << "    MedianWidth:        " << std::setprecision(1) << profA.medianWidth << " px" << std::endl;
    std::cout << "    Slenderness:        " << std::setprecision(1) << profA.slenderness << std::endl;
    std::cout << "    BilateralPersistence:" << std::setprecision(3) << profA.bilateralPersistence << std::endl;
    std::cout << "    LeftContinuity:     " << std::setprecision(3) << profA.leftContinuity << std::endl;
    std::cout << "    RightContinuity:    " << std::setprecision(3) << profA.rightContinuity << std::endl;
    std::cout << "    PairMotionConsist:  " << std::setprecision(3) << profA.pairMotionConsistency << std::endl;
    std::cout << "    CenterStability:    " << std::setprecision(3) << profA.centerStability << std::endl;
    std::cout << "    AxisCenterConsist:  " << std::setprecision(3) << profA.axisCenterConsistency << std::endl;
    std::cout << "    OrientationConsist: " << std::setprecision(3) << profA.orientationConsistency << std::endl;
    std::cout << "    TrackingJumpPenalty:" << std::setprecision(3) << profA.trackingJumpPenalty << std::endl;
    std::cout << "    EdgePairCoherence:  " << std::setprecision(3) << profA.edgePairCoherence << std::endl;
    std::cout << "    WidthSlope:         " << std::setprecision(4) << profA.widthSlope << std::endl;
    std::cout << "    WidthContinuity:    " << std::setprecision(3) << profA.widthContinuity << std::endl;
    std::cout << "    MeanEdgeStrength:   " << std::setprecision(1) << profA.meanEdgeStrength << std::endl;
    std::cout << "    MeanInsideGray:     " << std::setprecision(1) << profA.meanInsideGray << std::endl;
    std::cout << "    EntityContrast:     " << std::setprecision(1) << profA.entityContrast << std::endl;
    std::cout << "    BodyScore:          " << std::setprecision(3) << profA.bodyScore << "\n" << std::endl;

    std::cout << "Direction B (phi = " << std::setprecision(2) << normalizeAngle(bestFineTheta + 180.0) << " deg):" << std::endl;
    std::cout << "    Reach:              " << std::setprecision(1) << profB.reach << " px" << std::endl;
    std::cout << "    ValidLength:        " << std::setprecision(1) << profB.validLength << " px" << std::endl;
    std::cout << "    ValidSectionCount:  " << profB.validSectionCount << std::endl;
    std::cout << "    InvalidSectionCount:" << profB.invalidSectionCount << std::endl;
    std::cout << "    MaxGap:             " << profB.maxGap << std::endl;
    std::cout << "    MeanWidth:          " << std::setprecision(1) << profB.meanWidth << " px" << std::endl;
    std::cout << "    MedianWidth:        " << std::setprecision(1) << profB.medianWidth << " px" << std::endl;
    std::cout << "    Slenderness:        " << std::setprecision(1) << profB.slenderness << std::endl;
    std::cout << "    BilateralPersistence:" << std::setprecision(3) << profB.bilateralPersistence << std::endl;
    std::cout << "    LeftContinuity:     " << std::setprecision(3) << profB.leftContinuity << std::endl;
    std::cout << "    RightContinuity:    " << std::setprecision(3) << profB.rightContinuity << std::endl;
    std::cout << "    PairMotionConsist:  " << std::setprecision(3) << profB.pairMotionConsistency << std::endl;
    std::cout << "    CenterStability:    " << std::setprecision(3) << profB.centerStability << std::endl;
    std::cout << "    AxisCenterConsist:  " << std::setprecision(3) << profB.axisCenterConsistency << std::endl;
    std::cout << "    OrientationConsist: " << std::setprecision(3) << profB.orientationConsistency << std::endl;
    std::cout << "    TrackingJumpPenalty:" << std::setprecision(3) << profB.trackingJumpPenalty << std::endl;
    std::cout << "    EdgePairCoherence:  " << std::setprecision(3) << profB.edgePairCoherence << std::endl;
    std::cout << "    WidthSlope:         " << std::setprecision(4) << profB.widthSlope << std::endl;
    std::cout << "    WidthContinuity:    " << std::setprecision(3) << profB.widthContinuity << std::endl;
    std::cout << "    MeanEdgeStrength:   " << std::setprecision(1) << profB.meanEdgeStrength << std::endl;
    std::cout << "    MeanInsideGray:     " << std::setprecision(1) << profB.meanInsideGray << std::endl;
    std::cout << "    EntityContrast:     " << std::setprecision(1) << profB.entityContrast << std::endl;
    std::cout << "    BodyScore:          " << std::setprecision(3) << profB.bodyScore << "\n" << std::endl;

    std::cout << "Selected Direction:     Direction " << res.headEnd << std::endl;
    std::cout << "Selected Pointer Angle: " << std::setprecision(2) << res.pointerAngle << " deg" << std::endl;
    std::cout << "Reach: [Forward: " << std::setprecision(1) << res.reachForward
              << " px, Backward: " << res.reachBackward << " px]" << std::endl;
    std::cout << "==================================================\n" << std::endl;

    // Compute predicted tip and tail positions
    double ptrRad = res.pointerAngle * M_PI / 180.0;
    cv::Point2d ptrDir(std::cos(ptrRad), std::sin(ptrRad));

    res.tipRadius = static_cast<float>(res.endRadius > 0.0 ? res.endRadius : m_config.maxSearchRadius * 0.8);
    res.tipPoint = effectivePivot + ptrDir * res.tipRadius;
    if (res.tailDetected && res.tailProfile.reach > 5.0) {
        res.tailPoint = effectivePivot - ptrDir * res.tailProfile.reach;
    } else {
        res.tailPoint = effectivePivot - ptrDir * (m_config.pointerBaseRadius * 0.8);
    }

    const auto& winningProf = (res.headEnd == 'A') ? profA : profB;
    double cDiv = (m_config.confidenceDivisor > 0.0) ? m_config.confidenceDivisor : 12.0;
    res.confidence = std::min(1.0, (winningProf.bodyScore / cDiv));
    res.success = (winningProf.validSectionCount >= 3 && winningProf.bodyScore > 2.0);

    // Compile comprehensive diagnostic debug text and failure reason
    std::stringstream dbgStream;
    dbgStream << "=== [PointerDetector 算法检测日志] ===\n";
    dbgStream << "• 主轴方向: " << std::fixed << std::setprecision(2) << res.axisAngle << "°\n";
    dbgStream << "• 指针判定朝向: " << res.pointerAngle << "° (针尖端: " << res.headEnd << ", 针尾端: " << res.tailEnd << ")\n";
    dbgStream << "• 针身截面: 有效截面数=" << winningProf.validSectionCount << " (要求>=3), 覆盖长度=" << (res.endRadius - res.startRadius) << "px, 中值宽度=" << winningProf.medianWidth << "px\n";
    dbgStream << "• 几何特征: 细长比=" << winningProf.slenderness << ", 边缘互相关度=" << winningProf.edgePairCoherence << ", 轴心偏移稳定性=" << winningProf.centerStability << "\n";
    dbgStream << "• 综合身部得分(BodyScore): " << winningProf.bodyScore << " (门限: > 2.0)\n";
    if (res.tailDetected) {
        char tailDbg[160];
        snprintf(tailDbg, sizeof(tailDbg), "• 配重针尾(Tail): 已识别 (延伸=%.1fpx, 宽度=%.1fpx, 展宽斜率=%.4f, 展宽得分=%.2f, 得分=%.2f)\n",
                 res.tailProfile.reach, res.tailProfile.medianWidth, res.tailProfile.widthSlope, res.tailProfile.wideningScore, res.tailProfile.tailScore);
        dbgStream << tailDbg;
    } else {
        dbgStream << "• 配重针尾(Tail): 未识别到向外展宽的独立针尾\n";
    }
    dbgStream << "• 双向极性裕度: " << res.tipLikelihoodMargin << " [" << res.polarityConfidence << "]\n";

    if (winningProf.validSectionCount < 3) {
        res.failureReason = "指针径向剖面有效截面数不足 (当前仅找到 " + std::to_string(winningProf.validSectionCount) + " 个有效截面，算法至少需 >= 3 个)";
        dbgStream << "❌ 判定失败: " << res.failureReason << "\n";
    } else if (winningProf.bodyScore <= 2.0) {
        char sBuf[64];
        snprintf(sBuf, sizeof(sBuf), "%.2f", winningProf.bodyScore);
        res.failureReason = "指针结构综合置信度得分过低 (当前 bodyScore=" + std::string(sBuf) + " <= 门限 2.0，边缘反差不足或被高光遮挡)";
        dbgStream << "❌ 判定失败: " << res.failureReason << "\n";
    } else {
        res.failureReason.clear();
        dbgStream << "✅ 判定状态: 指针检测成功\n";
    }
    res.detailedDebugText = dbgStream.str();

    // Backward compatibility aliases
    res.angle = static_cast<float>(res.pointerAngle);
    res.angleDeg = static_cast<float>(res.pointerAngle);
    res.valid = res.success;
    res.score = static_cast<float>(winningProf.bodyScore);
    res.evidenceCoverage = static_cast<float>(winningProf.validLength / std::max(1.0, m_config.maxSearchRadius - m_config.pointerBaseRadius));
    res.finalScore = res.score;

    // Log debug summary
    std::cout << "[PointerDetector] "
              << "candidate theta = " << std::fixed << std::setprecision(2) << res.axisAngle << ", "
              << "validSections = " << winningProf.validSectionCount << ", "
              << "coverage = " << std::setprecision(1) << (res.endRadius - res.startRadius) << "px, "
              << "widthContinuity = " << std::setprecision(2) << winningProf.widthContinuity << ", "
              << "centerStability = " << winningProf.centerStability << ", "
              << "slenderness = " << winningProf.slenderness << ", "
              << "bodyScore = " << winningProf.bodyScore << ", "
              << "margin = " << res.tipLikelihoodMargin << " [" << res.polarityConfidence << "]" << std::endl;

    std::cout << "[PointerDetector] Best axis: theta = " << res.axisAngle << "°, "
              << "Pointer angle: " << res.pointerAngle << "°, "
              << "Pivot: (" << effectivePivot.x << ", " << effectivePivot.y << "), "
              << "Confidence: " << (res.confidence * 100.0) << "%" << std::endl;

    // Full visual debug diagnostics rendering
    mark("direction_scoring_ms");
    if (debugVis != nullptr) {
        renderDebugVisualization(*debugVis, effectivePivot, res);
        res.debugImage = debugVis->clone();
    }

    mark("debug_rendering_ms");
    return res;
}

PointerDetectionResult PointerDetector::detect(const cv::Mat& grayOrBin, cv::Mat* debugVis) {
    cv::Point2d center(m_model.center.x, m_model.center.y);
    return detect(grayOrBin, center, debugVis);
}

// -------------------------------------------------------------------------
// Comprehensive Visualization Overlay (Left/Right Edges, Centerline, HUD)
// -------------------------------------------------------------------------
void PointerDetector::renderDebugVisualization(
    cv::Mat& canvas,
    const cv::Point2d& effectivePivot,
    PointerDetectionResult& res) {

    int imgW = canvas.cols;
    int imgH = canvas.rows;

    if (canvas.empty() || canvas.channels() != 3) {
        if (!canvas.empty() && canvas.channels() == 1) {
            cv::cvtColor(canvas, canvas, cv::COLOR_GRAY2BGR);
        } else {
            int initW = 800, initH = 800;
            if (m_config.maxSearchRadius > 50.0) {
                initW = static_cast<int>(std::round(m_config.maxSearchRadius * 2.2));
                initH = initW;
            }
            canvas = cv::Mat::zeros(cv::Size(initW, initH), CV_8UC3);
        }
    }

    imgW = canvas.cols;
    imgH = canvas.rows;

    // 1. Draw pointerBaseRadius hub exclusion circle (Cyan) & outer search circle (Gray) - Thin Line (1 px)
    cv::circle(canvas, effectivePivot, static_cast<int>(m_config.pointerBaseRadius), cv::Scalar(255, 255, 0), 1, cv::LINE_AA);
    cv::circle(canvas, effectivePivot, static_cast<int>(m_config.maxSearchRadius), cv::Scalar(100, 100, 100), 1, cv::LINE_AA);

    // 2. Draw Top-N Candidate Axes (Thin dotted / dashed lines - 1 px)
    for (size_t i = 0; i < res.topCandidates.size(); ++i) {
        double candRad = res.topCandidates[i].theta * M_PI / 180.0;
        cv::Point2d dir(std::cos(candRad), std::sin(candRad));
        cv::Point2d p1 = effectivePivot - dir * m_config.maxSearchRadius;
        cv::Point2d p2 = effectivePivot + dir * m_config.maxSearchRadius;
        cv::line(canvas, p1, p2, cv::Scalar(80, 80, 140), 1, cv::LINE_AA);
    }

    // 3. Draw All Radial Cross Sections, Left Edges (Cyan), Right Edges (Magenta), and Centerline (Yellow) - Thin Lines (1 px)
    std::vector<cv::Point> leftPts, rightPts, centerPts;

    int secIdx = 0;
    for (const auto& sec : res.sections) {
        if (sec.valid) {
            // Draw cross section line segment connecting Left and Right edges (Green, 1 px)
            cv::line(canvas, sec.leftPoint, sec.rightPoint, cv::Scalar(0, 255, 0), 1, cv::LINE_AA);

            // Left edge point (Cyan)
            cv::circle(canvas, sec.leftPoint, 2, cv::Scalar(255, 255, 0), -1);
            leftPts.push_back(sec.leftPoint);

            // Right edge point (Magenta)
            cv::circle(canvas, sec.rightPoint, 2, cv::Scalar(255, 0, 255), -1);
            rightPts.push_back(sec.rightPoint);

            // Center point (Yellow)
            cv::circle(canvas, sec.centerPoint, 2, cv::Scalar(0, 255, 255), -1);
            centerPts.push_back(sec.centerPoint);

            // Render radius and width on selected sections along needle - Thin Text
            if (secIdx % 5 == 0) {
                char secBuf[64];
                snprintf(secBuf, sizeof(secBuf), "r:%.0f w:%.1f", sec.radius, sec.width);
                cv::putText(canvas, secBuf, cv::Point(static_cast<int>(sec.rightPoint.x + 4), static_cast<int>(sec.rightPoint.y + 2)),
                            cv::FONT_HERSHEY_SIMPLEX, 0.28, cv::Scalar(0, 255, 180), 1, cv::LINE_AA);
            }
            secIdx++;
        }
    }

    const auto& winProf = (res.headEnd == 'A') ? res.directionA : res.directionB;

    // Section XX: Draw full continuous EdgeTrack trajectories with THIN lines (1 px)
    const auto& reg = winProf.candidateRegion;
    if (reg.leftEdge.valid && reg.leftEdge.points.size() >= 2) {
        for (size_t i = 0; i < reg.leftEdge.points.size() - 1; ++i) {
            cv::Point p1(static_cast<int>(std::round(reg.leftEdge.points[i].x)),
                         static_cast<int>(std::round(reg.leftEdge.points[i].y)));
            cv::Point p2(static_cast<int>(std::round(reg.leftEdge.points[i + 1].x)),
                         static_cast<int>(std::round(reg.leftEdge.points[i + 1].y)));
            cv::line(canvas, p1, p2, cv::Scalar(255, 255, 0), 1, cv::LINE_AA); // Thin Cyan Left EdgeTrack (1 px)
        }
        // Principle 12: Draw Raw Support points (Green = Verified, Red = Unsupported)
        for (const auto& pt : reg.leftEdge.points) {
            cv::Point p(static_cast<int>(std::round(pt.x)), static_cast<int>(std::round(pt.y)));
            if (pt.hasRawSupport) {
                cv::circle(canvas, p, 2, cv::Scalar(0, 255, 0), -1, cv::LINE_AA); // Green: raw verified support
            } else {
                cv::circle(canvas, p, 2, cv::Scalar(0, 0, 255), -1, cv::LINE_AA); // Red: no raw support
            }
        }
    } else if (leftPts.size() >= 2) {
        for (size_t i = 0; i < leftPts.size() - 1; ++i) {
            cv::line(canvas, leftPts[i], leftPts[i + 1], cv::Scalar(255, 230, 0), 1, cv::LINE_AA);
        }
    }

    if (reg.rightEdge.valid && reg.rightEdge.points.size() >= 2) {
        for (size_t i = 0; i < reg.rightEdge.points.size() - 1; ++i) {
            cv::Point p1(static_cast<int>(std::round(reg.rightEdge.points[i].x)),
                         static_cast<int>(std::round(reg.rightEdge.points[i].y)));
            cv::Point p2(static_cast<int>(std::round(reg.rightEdge.points[i + 1].x)),
                         static_cast<int>(std::round(reg.rightEdge.points[i + 1].y)));
            cv::line(canvas, p1, p2, cv::Scalar(230, 0, 255), 1, cv::LINE_AA); // Thin Magenta Right EdgeTrack (1 px)
        }
        // Principle 12: Draw Raw Support points (Green = Verified, Red = Unsupported)
        for (const auto& pt : reg.rightEdge.points) {
            cv::Point p(static_cast<int>(std::round(pt.x)), static_cast<int>(std::round(pt.y)));
            if (pt.hasRawSupport) {
                cv::circle(canvas, p, 2, cv::Scalar(0, 255, 0), -1, cv::LINE_AA); // Green: raw verified support
            } else {
                cv::circle(canvas, p, 2, cv::Scalar(0, 0, 255), -1, cv::LINE_AA); // Red: no raw support
            }
        }
    } else if (rightPts.size() >= 2) {
        for (size_t i = 0; i < rightPts.size() - 1; ++i) {
            cv::line(canvas, rightPts[i], rightPts[i + 1], cv::Scalar(230, 0, 255), 1, cv::LINE_AA);
        }
    }

    // Connect Centerline Ridge Curve (Yellow, 1 px)
    if (centerPts.size() >= 2) {
        for (size_t i = 0; i < centerPts.size() - 1; ++i) {
            cv::line(canvas, centerPts[i], centerPts[i + 1], cv::Scalar(0, 255, 255), 1, cv::LINE_AA);
        }
    }

    // Draw Tail Cross Sections (Orange / Coral, 1 px)
    if (res.tailDetected) {
        for (const auto& sec : res.tailProfile.sections) {
            if (sec.valid) {
                cv::line(canvas, sec.leftPoint, sec.rightPoint, cv::Scalar(0, 140, 255), 1, cv::LINE_AA);
                cv::circle(canvas, sec.leftPoint, 1, cv::Scalar(0, 200, 255), -1, cv::LINE_AA);
                cv::circle(canvas, sec.rightPoint, 1, cv::Scalar(0, 200, 255), -1, cv::LINE_AA);
            }
        }
        cv::circle(canvas, res.tailPoint, 3, cv::Scalar(0, 140, 255), -1, cv::LINE_AA);
    }

    // 4. Draw Final Directed Pointer Vector - Thin Crisp Lines (1 px)
    cv::line(canvas, res.tailPoint, res.tipPoint, cv::Scalar(0, 0, 255), 1, cv::LINE_AA);
    cv::circle(canvas, res.tipPoint, 4, cv::Scalar(0, 255, 0), -1, cv::LINE_AA);
    cv::drawMarker(canvas, res.tipPoint, cv::Scalar(0, 0, 255), cv::MARKER_CROSS, 8, 1);

    // Effective pivot point (Red)
    cv::circle(canvas, effectivePivot, 3, cv::Scalar(0, 0, 255), -1, cv::LINE_AA);
    cv::circle(canvas, effectivePivot, 5, cv::Scalar(0, 255, 255), 1, cv::LINE_AA);

    // 5. Section XX: Output HUD to a Dedicated Separate Window Image (DO NOT draw over canvas to avoid obstructing image)
    int hudW = 560;
    int hudH = 445;
    cv::Mat hudMat = cv::Mat::zeros(cv::Size(hudW, hudH), CV_8UC3);
    hudMat.setTo(cv::Scalar(18, 20, 28)); // Dark slate background
    cv::rectangle(hudMat, cv::Rect(4, 4, hudW - 8, hudH - 8), cv::Scalar(0, 229, 255), 1);

    std::stringstream hudStream;
    char buf[256];
    int lineY = 28;
    int lineStep = 22;

    snprintf(buf, sizeof(buf), "[LONG PARALLEL EDGE PAIR HUD]");
    cv::putText(hudMat, buf, cv::Point(16, lineY), cv::FONT_HERSHEY_SIMPLEX, 0.48, cv::Scalar(0, 229, 255), 2, cv::LINE_AA);
    hudStream << buf << "\n";
    lineY += lineStep;

    snprintf(buf, sizeof(buf), "Candidate Angle:     %.2f deg", res.axisAngle);
    cv::putText(hudMat, buf, cv::Point(16, lineY), cv::FONT_HERSHEY_SIMPLEX, 0.42, cv::Scalar(255, 255, 255), 1, cv::LINE_AA);
    hudStream << buf << "\n";
    lineY += lineStep;

    snprintf(buf, sizeof(buf), "Left Length:         %.0f px | Right Length: %.0f px", winProf.leftLength, winProf.rightLength);
    cv::putText(hudMat, buf, cv::Point(16, lineY), cv::FONT_HERSHEY_SIMPLEX, 0.40, cv::Scalar(255, 200, 100), 1, cv::LINE_AA);
    hudStream << buf << "\n";
    lineY += lineStep;

    snprintf(buf, sizeof(buf), "Common Overlap:      %.0f px (Span: [%.0f, %.0f])",
             winProf.commonLength, reg.commonStartRadius, reg.commonEndRadius);
    cv::putText(hudMat, buf, cv::Point(16, lineY), cv::FONT_HERSHEY_SIMPLEX, 0.40, cv::Scalar(220, 220, 220), 1, cv::LINE_AA);
    hudStream << buf << "\n";
    lineY += lineStep;

    snprintf(buf, sizeof(buf), "Parallelism:         %.2f | Base Proximity: %.2f",
             winProf.parallelism, winProf.baseProximity);
    cv::putText(hudMat, buf, cv::Point(16, lineY), cv::FONT_HERSHEY_SIMPLEX, 0.40, cv::Scalar(0, 255, 200), 1, cv::LINE_AA);
    hudStream << buf << "\n";
    lineY += lineStep;

    snprintf(buf, sizeof(buf), "Left Straightness:   %.2f | Right Straight: %.2f",
             winProf.leftStraightness, winProf.rightStraightness);
    cv::putText(hudMat, buf, cv::Point(16, lineY), cv::FONT_HERSHEY_SIMPLEX, 0.40, cv::Scalar(220, 220, 220), 1, cv::LINE_AA);
    hudStream << buf << "\n";
    lineY += lineStep;

    snprintf(buf, sizeof(buf), "Raw Support Density: L=%.2f (%d pts) | R=%.2f (%d pts)",
             reg.leftSupportDensity, reg.leftEdge.verifiedSupportCount,
             reg.rightSupportDensity, reg.rightEdge.verifiedSupportCount);
    cv::putText(hudMat, buf, cv::Point(16, lineY), cv::FONT_HERSHEY_SIMPLEX, 0.40, cv::Scalar(0, 255, 180), 1, cv::LINE_AA);
    hudStream << buf << "\n";
    lineY += lineStep;

    snprintf(buf, sizeof(buf), "Median Width:        %.1f px (Mean: %.1f px)", winProf.medianWidth, winProf.meanWidth);
    cv::putText(hudMat, buf, cv::Point(16, lineY), cv::FONT_HERSHEY_SIMPLEX, 0.40, cv::Scalar(220, 220, 220), 1, cv::LINE_AA);
    hudStream << buf << "\n";
    lineY += lineStep;

    snprintf(buf, sizeof(buf), "Width Slope (dW/dr): %.4f | Width Cont: %.2f", winProf.widthSlope, winProf.widthContinuity);
    cv::putText(hudMat, buf, cv::Point(16, lineY), cv::FONT_HERSHEY_SIMPLEX, 0.40, cv::Scalar(200, 200, 200), 1, cv::LINE_AA);
    hudStream << buf << "\n";
    lineY += lineStep;

    snprintf(buf, sizeof(buf), "Geometry Score:      %.3f [%s]",
             winProf.geometryScore, winProf.geometryValid ? "VALID" : "GATE_REJECTED");
    cv::Scalar geomColor = winProf.geometryValid ? cv::Scalar(0, 255, 180) : cv::Scalar(0, 100, 255);
    cv::putText(hudMat, buf, cv::Point(16, lineY), cv::FONT_HERSHEY_SIMPLEX, 0.42, geomColor, 1, cv::LINE_AA);
    hudStream << buf << "\n";
    lineY += lineStep;

    snprintf(buf, sizeof(buf), "Body Score:          %.3f (DirA=%.2f, DirB=%.2f)",
             winProf.bodyScore, res.directionA.bodyScore, res.directionB.bodyScore);
    cv::putText(hudMat, buf, cv::Point(16, lineY), cv::FONT_HERSHEY_SIMPLEX, 0.44, cv::Scalar(0, 255, 255), 2, cv::LINE_AA);
    hudStream << buf << "\n";
    lineY += lineStep;

    snprintf(buf, sizeof(buf), "Pointer Angle:       %.2f deg (Head: %c)", res.pointerAngle, res.headEnd);
    cv::putText(hudMat, buf, cv::Point(16, lineY), cv::FONT_HERSHEY_SIMPLEX, 0.44, cv::Scalar(0, 255, 0), 2, cv::LINE_AA);
    hudStream << buf << "\n";
    lineY += lineStep;

    snprintf(buf, sizeof(buf), "Tail Detection:      %s (Reach: %.0f px, Width: %.1f px, Score: %.2f)",
             res.tailDetected ? "DETECTED" : "NONE",
             res.tailDetected ? res.tailProfile.reach : 0.0,
             res.tailDetected ? res.tailProfile.meanWidth : 0.0,
             res.tailDetected ? res.tailProfile.tailScore : 0.0);
    cv::putText(hudMat, buf, cv::Point(16, lineY), cv::FONT_HERSHEY_SIMPLEX, 0.40,
                res.tailDetected ? cv::Scalar(0, 165, 255) : cv::Scalar(180, 180, 180), 1, cv::LINE_AA);
    hudStream << buf << "\n";
    lineY += lineStep;

    snprintf(buf, sizeof(buf), "Tracks: Cyan=L | Magenta=R | Orange=Tail | GreenDot=RawSupp");
    cv::putText(hudMat, buf, cv::Point(16, lineY), cv::FONT_HERSHEY_SIMPLEX, 0.36, cv::Scalar(0, 229, 255), 1, cv::LINE_AA);
    hudStream << buf << "\n";

    res.hudImage = hudMat;
    res.hudText = hudStream.str();
}
