#include "PointerDetector.hpp"
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
}

PointerDetector::PointerDetector(const GaugeConfig& config)
    : m_model(config.toShapeModel()) {
    m_config.pointerBaseRadius = std::max(15.0, config.pointerBaseRadius);
    m_config.maxSearchRadius = std::max(100.0, config.radius > 50.0 ? config.radius * 0.92 : 360.0);
    m_config.minPointerWidth = std::max(1.5, config.pointerMinWidth);
    m_config.maxPointerWidth = std::min(45.0, config.pointerMaxWidth);
    m_config.radialStepPx = 2.0;
    m_config.radialStep = 2.0;
    m_config.angleStep = (config.coarseAngleStep > 0.0) ? config.coarseAngleStep : 1.0;
}

void PointerDetector::setConfig(const PointerDetectorConfig& config) {
    m_config = config;
}

void PointerDetector::setConfig(const GaugeConfig& config) {
    m_model = config.toShapeModel();
    m_config.pointerBaseRadius = std::max(15.0, config.pointerBaseRadius);
    m_config.maxSearchRadius = std::max(100.0, config.radius > 50.0 ? config.radius * 0.92 : 360.0);
    m_config.minPointerWidth = std::max(1.5, config.pointerMinWidth);
    m_config.maxPointerWidth = std::min(45.0, config.pointerMaxWidth);
    m_config.radialStepPx = 2.0;
    m_config.radialStep = 2.0;
    m_config.angleStep = (config.coarseAngleStep > 0.0) ? config.coarseAngleStep : 1.0;
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
// Legacy compatibility implementation for PointerTemplateBuilderDialog
// -------------------------------------------------------------------------
std::vector<TemplatePoint> PointerDetector::buildUnifiedTemplate(const PointerShapeModel& model) {
    std::vector<TemplatePoint> pts;
    float step = std::max(2.0f, model.radialStep);

    auto sampleCustomContour = [&](const std::vector<cv::Point2f>& contour, RegionType region, float weight, bool closed) {
        if (contour.size() < 2) return;
        size_t nSegs = closed ? contour.size() : (contour.size() - 1);
        for (size_t i = 0; i < nSegs; ++i) {
            cv::Point2f p1 = contour[i];
            cv::Point2f p2 = contour[(i + 1) % contour.size()];
            cv::Point2f vec = p2 - p1;
            float len = static_cast<float>(cv::norm(vec));
            if (len < 1e-3f) continue;

            cv::Point2f dir = vec * (1.0f / len);
            cv::Point2f norm(-dir.y, dir.x);

            int nSamples = std::max(1, static_cast<int>(std::round(len / step)));
            for (int k = 0; k <= nSamples; ++k) {
                float t = static_cast<float>(k) / nSamples;
                cv::Point2f samplePt = p1 + t * vec;
                if (cv::norm(samplePt) < model.centerExclusionRadius) continue;
                pts.push_back({ samplePt, norm, weight, region });
            }
        }
    };

    if (!model.customTipPoints.empty() && model.customTipPoints.size() >= 2) {
        sampleCustomContour(model.customTipPoints, RegionType::Tip, 1.4f, true);
    } else {
        float tipRStart = std::max(model.bodyMaxRadius, model.tipMinRadius);
        float tipREnd = model.tipMaxRadius;
        if (tipREnd > tipRStart) {
            float baseHalfW = model.tipBaseWidth * 0.5f;
            float totalTipLen = tipREnd - tipRStart;
            for (float r = tipRStart; r < tipREnd; r += step) {
                float frac = (r - tipRStart) / totalTipLen;
                float currentHalfW = baseHalfW * (1.0f - frac);
                float dwdr = -baseHalfW / totalTipLen;
                cv::Vec2f nLeftVec(-dwdr, 1.0f);
                cv::Vec2f nLeftNorm = cv::normalize(nLeftVec);
                cv::Vec2f nRightVec(-dwdr, -1.0f);
                cv::Vec2f nRightNorm = cv::normalize(nRightVec);
                pts.push_back({ cv::Point2f(r, currentHalfW), cv::Point2f(nLeftNorm[0], nLeftNorm[1]), 1.2f, RegionType::Tip });
                pts.push_back({ cv::Point2f(r, -currentHalfW), cv::Point2f(nRightNorm[0], nRightNorm[1]), 1.2f, RegionType::Tip });
            }
            pts.push_back({ cv::Point2f(tipREnd, 0.0f), cv::Point2f(1.0f, 0.0f), 1.5f, RegionType::Tip });
        }
    }

    if (!model.customBodyPoints.empty() && model.customBodyPoints.size() >= 2) {
        sampleCustomContour(model.customBodyPoints, RegionType::Body, 1.0f, false);
    } else {
        float bodyRStart = std::max(model.centerExclusionRadius + 2.0f, model.bodyMinRadius);
        float bodyREnd = std::min(model.tipMinRadius, model.bodyMaxRadius);
        if (bodyREnd > bodyRStart) {
            float halfW = model.bodyWidth * 0.5f;
            for (float r = bodyRStart; r <= bodyREnd; r += step) {
                pts.push_back({ cv::Point2f(r, halfW), cv::Point2f(0.0f, 1.0f), 1.0f, RegionType::Body });
                pts.push_back({ cv::Point2f(r, -halfW), cv::Point2f(0.0f, -1.0f), 1.0f, RegionType::Body });
            }
        }
    }

    if (!model.customTailPoints.empty() && model.customTailPoints.size() >= 2) {
        sampleCustomContour(model.customTailPoints, RegionType::Tail, 1.2f, true);
    } else {
        float tailRStart = std::max(model.centerExclusionRadius + 2.0f, model.tailMinRadius);
        float tailREnd = model.tailMaxRadius;
        if (tailREnd > tailRStart) {
            float tailHalfW = model.tailWidth * 0.5f;
            for (float r = tailRStart; r <= tailREnd; r += step) {
                pts.push_back({ cv::Point2f(-r, tailHalfW), cv::Point2f(0.0f, 1.0f), 1.1f, RegionType::Tail });
                pts.push_back({ cv::Point2f(-r, -tailHalfW), cv::Point2f(0.0f, -1.0f), 1.1f, RegionType::Tail });
            }
        }
    }
    return pts;
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
// Step 2: Compute Single-Pass Global Scharr Gradient Magnitude and Orientation
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
    int maxConsecutiveGap = (m_config.maxConsecutiveGapSamples > 0) ? m_config.maxConsecutiveGapSamples : 2;

    double rawGradThresh = (m_config.rawGradientThreshold > 0.0) ? m_config.rawGradientThreshold : 55.0;
    double minNormalAlign = (m_config.minGradientNormalAlignment > 0.0) ? m_config.minGradientNormalAlignment : 0.85;
    double minStepContrast = (m_config.minLocalStepContrast > 0.0) ? m_config.minLocalStepContrast : 16.0;
    double maxStartGap = (m_config.maxStartBaseGapPx > 0.0) ? m_config.maxStartBaseGapPx : 8.0;

    std::vector<EdgePoint> rawPoints;
    double sumStrength = 0.0;
    std::vector<double> strengths;
    int jumpCount = 0;
    int verifiedSupportCount = 0;

    for (double r = rStart; r <= rEnd; r += stepR) {
        // Principle 11: Base proximity hard check.
        // If we haven't found any valid edge within maxStartGap from pointerBaseRadius,
        // it means this corridor starts in empty dial background. Terminate immediately!
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

            float dirScore = std::abs(static_cast<float>(ux * v.x + uy * v.y));
            float edgeScore = mag * dirScore;

            profile[i] = { t, mag, dirScore, edgeScore, gVal };
        }

        // Principle 10: Pure white or flat background check!
        // If the peak-to-peak intensity contrast across the slice is less than minStepContrast,
        // it is impossible for a genuine pointer physical edge to exist here!
        if ((maxGray - minGray) < static_cast<float>(minStepContrast)) {
            consecutiveMisses++;
            if (consecutiveMisses > maxConsecutiveGap) {
                // Principle 3 & 4: Max Consecutive Gap exceeded -> TERMINATE TRACK!
                break;
            }
            continue;
        }

        // 2. Find local peaks with STRICT RAW GRADIENT HARD GATE & PROMINENCE
        struct LocalPeak {
            double t;
            float edgeScore;
            float dirScore;
            float mag;
        };
        std::vector<LocalPeak> peaks;

        for (int i = 1; i < numSamples - 1; ++i) {
            // Hard gates:
            // a. Gradient magnitude must exceed rawGradThresh (e.g. >= 55.0)
            // b. Direction must be tightly perpendicular to needle axis (|dot(grad, v)| >= 0.85)
            // c. Combined edgeScore must exceed minGradThresh
            if (profile[i].mag >= rawGradThresh && profile[i].dirScore >= minNormalAlign && profile[i].edgeScore >= minGradThresh) {
                // Strict local peak
                if (profile[i].edgeScore > profile[i - 1].edgeScore &&
                    profile[i].edgeScore >= profile[i + 1].edgeScore) {
                    float prominence = profile[i].edgeScore - std::min(profile[i - 1].edgeScore, profile[i + 1].edgeScore);
                    if (prominence >= 3.0f) {
                        peaks.push_back({ profile[i].t, profile[i].edgeScore, profile[i].dirScore, profile[i].mag });
                    }
                }
            }
        }

        // 3. Select best peak using tracking window
        LocalPeak bestPeak{ 0.0, -1.0f, 0.0f, 0.0f };
        double bestPeakScore = -1.0;

        for (const auto& p : peaks) {
            if (prevT > -900.0) {
                // Tracking mode: prioritize proximity to prevT
                double dt = std::abs(p.t - prevT);
                if (dt > trackingWindow * 1.5) continue; // Outside local tracking window
                if (dt > maxJump * 1.5) continue;       // Reject massive jump

                double sJump = std::max(0.0, 1.0 - dt / maxJump);
                double sDir = p.dirScore;
                double sMag = std::min(1.0, static_cast<double>(p.edgeScore) / 100.0);

                double score = 0.50 * sJump + 0.30 * sDir + 0.20 * sMag;
                if (score > bestPeakScore) {
                    bestPeakScore = score;
                    bestPeak = p;
                }
            } else {
                // Initial detection mode: choose strongest peak with good orientation
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

            // Principle 5: Raw edge support verification on exact image coordinates
            float rawMag = sampleBilinear(features.gradMag, static_cast<float>(pt.x), static_cast<float>(pt.y));
            float rawUx = sampleBilinear(features.gradUnitX, static_cast<float>(pt.x), static_cast<float>(pt.y));
            float rawUy = sampleBilinear(features.gradUnitY, static_cast<float>(pt.x), static_cast<float>(pt.y));
            float rawDir = std::abs(static_cast<float>(rawUx * v.x + rawUy * v.y));

            // Check local step contrast across the edge point (+/- 2px along normal v)
            float gIn = sampleBilinear(gray, static_cast<float>(pt.x - v.x * 2.0), static_cast<float>(pt.y - v.y * 2.0));
            float gOut = sampleBilinear(gray, static_cast<float>(pt.x + v.x * 2.0), static_cast<float>(pt.y + v.y * 2.0));
            float localStep = std::abs(gOut - gIn);

            pt.hasRawSupport = (rawMag >= rawGradThresh && rawDir >= minNormalAlign && localStep >= (minStepContrast * 0.7f));
            pt.valid = pt.hasRawSupport;

            if (pt.hasRawSupport) {
                verifiedSupportCount++;
            }

            rawPoints.push_back(pt);
            sumStrength += bestPeak.edgeScore;
            strengths.push_back(bestPeak.edgeScore);
        } else {
            consecutiveMisses++;
            // Principle 3 & 4: HARD GAP CONSTRAINT - Stop immediately if no genuine edge found for maxConsecutiveGap samples!
            // Strictly forbid bridging long gaps across white dial background!
            if (consecutiveMisses > maxConsecutiveGap) {
                break;
            }
        }
    }

    // Minimum points check: a real pointer edge must extend over a meaningful distance
    if (rawPoints.size() < 8) {
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
    if (track.length < 25.0) {
        return track;
    }

    track.meanStrength = sumStrength / rawPoints.size();
    std::sort(strengths.begin(), strengths.end());
    track.medianStrength = strengths[strengths.size() / 2];
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

    // HARD GATES:
    // 1. edgeSupportDensity >= minEdgeSupportDensity (0.75)
    // 2. maxConsecutiveGap <= maxConsecutiveGapSamples (2)
    // 3. continuity >= 0.70
    double minSupportThresh = (m_config.minEdgeSupportDensity > 0.0) ? m_config.minEdgeSupportDensity : 0.75;
    if (track.edgeSupportDensity < minSupportThresh ||
        track.maxConsecutiveGap > maxConsecutiveGap ||
        track.continuity < 0.70) {
        // Principle 2: REJECT TRACK! Do not allow sparse or synthetic points to pass!
        track.valid = false;
        return track;
    }

    // Principle 1 & 8: Line fitting ONLY to verify straightness of genuine physical track
    std::vector<cv::Point2f> cvPts;
    cvPts.reserve(rawPoints.size());
    for (const auto& p : rawPoints) {
        if (p.hasRawSupport) {
            cvPts.emplace_back(static_cast<float>(p.x), static_cast<float>(p.y));
        }
    }

    if (cvPts.size() < 6) {
        track.valid = false;
        return track;
    }

    cv::Vec4f lineParams;
    cv::fitLine(cvPts, lineParams, cv::DIST_L2, 0, 0.01, 0.01);
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
    double maxResThresh = (m_config.maxLineFitResidualPx > 0.0) ? m_config.maxLineFitResidualPx : 1.8;
    track.valid = (track.length >= 35.0 &&
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

    double maxParallelDiff = (m_config.maxParallelAngleDiffDeg > 0.0) ? m_config.maxParallelAngleDiffDeg : 5.0;
    region.parallelism = std::max(0.0, 1.0 - diffAng / maxParallelDiff);

    // 4. Section XII: Base Proximity Check
    double baseDistL = std::abs(region.leftEdge.baseDistance);
    double baseDistR = std::abs(region.rightEdge.baseDistance);
    double maxBaseDist = (m_config.maxBaseDistancePx > 0.0) ? m_config.maxBaseDistancePx : 20.0;
    double baseProxL = std::max(0.0, 1.0 - baseDistL / maxBaseDist);
    double baseProxR = std::max(0.0, 1.0 - baseDistR / maxBaseDist);
    region.baseProximity = (baseProxL + baseProxR) * 0.5;

    // 5. Section XVI: First Layer - Geometry Score Gate with Strict Straightness & Raw Support
    double minCommonLen = (m_config.minCommonLengthPx > 0.0) ? m_config.minCommonLengthPx : 50.0;
    double lenScore = std::min(1.0, commonLen / minCommonLen);

    region.geometryScore = 0.25 * lenScore +
                           0.25 * region.parallelism +
                           0.20 * region.straightness +
                           0.15 * region.baseProximity +
                           0.15 * region.edgeSupportDensity;

    double maxResThresh = (m_config.maxLineFitResidualPx > 0.0) ? m_config.maxLineFitResidualPx : 1.8;
    double minSupportThresh = (m_config.minEdgeSupportDensity > 0.0) ? m_config.minEdgeSupportDensity : 0.75;

    // HARD GEOMETRY GATE (Principle 11):
    // 1. Both edges must be approximately straight: lineFitResidual <= maxResThresh
    // 2. Both edges must be approximately parallel: diffAng <= maxParallelDiff
    // 3. Both edges must have sufficient common overlap: commonLen >= minCommonLen
    // 4. Both edges must start near pointerBaseRadius: baseProximity >= 0.50
    // 5. Both edges must have high Raw Edge Support Density: supportDensity >= minSupportThresh
    bool straightValid = (region.leftEdge.lineFitResidual <= maxResThresh &&
                          region.rightEdge.lineFitResidual <= maxResThresh &&
                          region.straightness >= 0.70);
    bool parallelValid = (diffAng <= maxParallelDiff);
    bool lengthValid = (commonLen >= minCommonLen);
    bool baseValid = (region.baseProximity >= 0.50);
    bool supportValid = (region.leftSupportDensity >= minSupportThresh &&
                         region.rightSupportDensity >= minSupportThresh);

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

    for (double r = commonStart; r <= commonEnd; r += stepR) {
        int rKey = static_cast<int>(std::round(r * 2.0));
        if (leftTMap.find(rKey) != leftTMap.end() && rightTMap.find(rKey) != rightTMap.end()) {
            double w = rightTMap[rKey] - leftTMap[rKey];
            if (w >= m_config.minPointerWidth && w <= m_config.maxPointerWidth) {
                widths.push_back(w);
                radii.push_back(r);
            }
        }
    }

    if (widths.size() < 4) {
        region.geometryValid = false;
        region.bodyScore = 0.0;
        return region;
    }

    double sumW = 0.0;
    for (double w : widths) sumW += w;
    region.meanWidth = sumW / widths.size();

    std::vector<double> sortedW = widths;
    std::sort(sortedW.begin(), sortedW.end());
    region.medianWidth = sortedW[sortedW.size() / 2];

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

    // Section XVI: Final Body Score
    region.bodyScore = 0.35 * region.geometryScore +
                       0.25 * slendernessScore +
                       0.15 * region.widthContinuity +
                       0.15 * widthTrend +
                       0.10 * (commonLen / 250.0);

    return region;
}

// -------------------------------------------------------------------------
// Step 3: Extract Cross Section Bilateral Edge Pair along v
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
// Step 6: Evaluate Radial Direction Profile along a ray (phi in [0, 360))
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

    // Section XI & XVI: Final BodyScore heavily prioritizes Geometry Gate and Candidate Region
    double geomWeight = 4.0;
    prof.bodyScore = (
        geomWeight                    * prof.geometryScore +
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
        m_config.wEdgeStrength        * edgeStrengthScore -
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
// Step 4 & 5: Evaluate Non-Directional Candidate Axis in [0, 180°)
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

    cand.totalScore = profA.bodyScore + profB.bodyScore;
    cand.bodyScore = cand.totalScore;

    bool aIsPrimary = (profA.bodyScore >= profB.bodyScore);
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
// Step 9: Evaluate TipLikelihood for Needle Tip Identification
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
    res.success = false;

    if (gray.empty()) return res;

    cv::Mat grayImg;
    if (gray.channels() == 3) {
        cv::cvtColor(gray, grayImg, cv::COLOR_BGR2GRAY);
    } else {
        grayImg = gray.clone();
    }

    // Section XXI: Effective pivot = dialCenter + pivotOffset
    cv::Point2d effectivePivot = dialCenter + m_pivotOffset;
    res.effectivePivot = effectivePivot;

    // Step 2: Global single-pass Scharr gradient calculation
    GradientFeatureMap features = computeGradientFeatures(grayImg);

    // Step 5: Phase 1 Non-directional axis search theta in [0, 180°)
    double angleStep = std::max(0.2, m_config.angleStep);
    int numSteps = static_cast<int>(std::round(180.0 / angleStep));

    std::vector<PointerAxisCandidate> allCandidates;
    allCandidates.reserve(numSteps);

    double bestCoarseTheta = 0.0;
    double maxCoarseScore = -1.0;
    PointerAxisCandidate bestCoarseCandidate;

    for (int i = 0; i < numSteps; ++i) {
        double theta = i * angleStep;
        RadialDirectionProfile profA = evaluateRadialDirectionProfile(grayImg, features, effectivePivot, theta);
        RadialDirectionProfile profB = evaluateRadialDirectionProfile(grayImg, features, effectivePivot, theta + 180.0);

        double axisScore = profA.bodyScore + profB.bodyScore;

        bool aIsPrimary = (profA.bodyScore >= profB.bodyScore);
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

        allCandidates.push_back(cand);

        if (axisScore > maxCoarseScore) {
            maxCoarseScore = axisScore;
            bestCoarseTheta = theta;
            bestCoarseCandidate = cand;
        }
    }

    // Step 8: Top-10 candidate retention and formatted debug table
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

    // Fine angular refinement around bestCoarseTheta
    double fineRange = 1.5;
    double fineStep = 0.05;
    double bestFineTheta = bestCoarseTheta;
    double maxFineScore = maxCoarseScore;
    PointerAxisCandidate bestFineCandidate = bestCoarseCandidate;

    int numFineSteps = static_cast<int>(std::round((fineRange * 2.0) / fineStep));
    for (int i = 0; i <= numFineSteps; ++i) {
        double theta = (bestCoarseTheta - fineRange) + i * fineStep;
        if (theta < 0.0) theta += 180.0;
        if (theta >= 180.0) theta -= 180.0;

        RadialDirectionProfile profA = evaluateRadialDirectionProfile(grayImg, features, effectivePivot, theta);
        RadialDirectionProfile profB = evaluateRadialDirectionProfile(grayImg, features, effectivePivot, theta + 180.0);
        double combScore = profA.bodyScore + profB.bodyScore;

        if (combScore > maxFineScore) {
            maxFineScore = combScore;
            bestFineTheta = theta;
            bool aIsPrimary = (profA.bodyScore >= profB.bodyScore);
            const auto& primary = aIsPrimary ? profA : profB;

            bestFineCandidate.theta = theta;
            bestFineCandidate.sections = primary.sections;
            bestFineCandidate.reach = primary.reach;
            bestFineCandidate.coverageLength = primary.validLength;
            bestFineCandidate.meanWidth = primary.meanWidth;
            bestFineCandidate.medianWidth = primary.medianWidth;
            bestFineCandidate.slenderness = primary.slenderness;
            bestFineCandidate.edgeStrength = primary.meanEdgeStrength;
            bestFineCandidate.edgePersistence = primary.bilateralPersistence;
            bestFineCandidate.bilateralPersistence = primary.bilateralPersistence;
            bestFineCandidate.widthSlope = primary.widthSlope;
            bestFineCandidate.widthContinuity = primary.widthContinuity;
            bestFineCandidate.centerStability = primary.centerStability;
            bestFineCandidate.leftEdgeStability = primary.centerStability;
            bestFineCandidate.rightEdgeStability = primary.centerStability;
            bestFineCandidate.bodyScore = combScore;
            bestFineCandidate.totalScore = combScore;
            bestFineCandidate.validSectionCount = primary.validSectionCount;
            bestFineCandidate.startRadius = primary.startRadius;
            bestFineCandidate.endRadius = primary.endRadius;

            bestFineCandidate.leftLength = primary.leftLength;
            bestFineCandidate.rightLength = primary.rightLength;
            bestFineCandidate.commonLength = primary.commonLength;
            bestFineCandidate.leftStraightness = primary.leftStraightness;
            bestFineCandidate.rightStraightness = primary.rightStraightness;
            bestFineCandidate.parallelism = primary.parallelism;
            bestFineCandidate.baseProximity = primary.baseProximity;
            bestFineCandidate.leftSupportDensity = primary.leftSupportDensity;
            bestFineCandidate.rightSupportDensity = primary.rightSupportDensity;
            bestFineCandidate.edgeSupportDensity = primary.edgeSupportDensity;
            bestFineCandidate.geometryScore = primary.geometryScore;
            bestFineCandidate.geometryValid = primary.geometryValid;

            bestFineCandidate.leftContinuity = primary.leftContinuity;
            bestFineCandidate.rightContinuity = primary.rightContinuity;
            bestFineCandidate.pairMotionConsistency = primary.pairMotionConsistency;
            bestFineCandidate.axisCenterConsistency = primary.axisCenterConsistency;
            bestFineCandidate.orientationConsistency = primary.orientationConsistency;
            bestFineCandidate.trackingJumpPenalty = primary.trackingJumpPenalty;
            bestFineCandidate.edgePairCoherence = primary.edgePairCoherence;
        }
    }

    res.axisAngle = bestFineTheta;

    // Step 9: Phase 2 Bidirectional Direction Analysis (+axis vs -axis)
    RadialDirectionProfile profA = evaluateRadialDirectionProfile(grayImg, features, effectivePivot, bestFineTheta);
    RadialDirectionProfile profB = evaluateRadialDirectionProfile(grayImg, features, effectivePivot, bestFineTheta + 180.0);

    res.directionA = profA;
    res.directionB = profB;
    res.reachForward = profA.reach;
    res.reachBackward = profB.reach;

    res.tipLikelihoodA = profA.bodyScore;
    res.tipLikelihoodB = profB.bodyScore;
    res.tipLikelihoodMargin = std::abs(profA.bodyScore - profB.bodyScore);

    if (profA.bodyScore >= profB.bodyScore) {
        res.headEnd = 'A';
        res.tailEnd = 'B';
        res.pointerAngle = normalizeAngle(bestFineTheta);
        res.sections = profA.sections;
        res.startRadius = profA.startRadius;
        res.endRadius = profA.endRadius;
    } else {
        res.headEnd = 'B';
        res.tailEnd = 'A';
        res.pointerAngle = normalizeAngle(bestFineTheta + 180.0);
        res.sections = profB.sections;
        res.startRadius = profB.startRadius;
        res.endRadius = profB.endRadius;
    }

    if (res.tipLikelihoodMargin < 1.2) {
        res.polarityConfidence = "LOW";
    } else if (res.tipLikelihoodMargin < 3.0) {
        res.polarityConfidence = "MEDIUM";
    } else {
        res.polarityConfidence = "HIGH";
    }

    // Step 10: Print EXACT required debug output
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
    std::cout << "    BodyScore:          " << std::setprecision(3) << profB.bodyScore << "\n" << std::endl;

    std::cout << "Selected Direction:     Direction " << res.headEnd << std::endl;
    std::cout << "Selected Pointer Angle: " << std::setprecision(2) << res.pointerAngle << " deg" << std::endl;
    std::cout << "Reach: [Forward: " << std::setprecision(1) << res.reachForward
              << " px, Backward: " << res.reachBackward << " px]" << std::endl;
    std::cout << "==================================================\n" << std::endl;

    // Step 10: Compute predicted tip and tail positions
    double ptrRad = res.pointerAngle * M_PI / 180.0;
    cv::Point2d ptrDir(std::cos(ptrRad), std::sin(ptrRad));

    res.tipRadius = static_cast<float>(res.endRadius > 0.0 ? res.endRadius : m_config.maxSearchRadius * 0.8);
    res.tipPoint = effectivePivot + ptrDir * res.tipRadius;
    res.tailPoint = effectivePivot - ptrDir * (m_config.pointerBaseRadius * 0.8);

    const auto& winningProf = (res.headEnd == 'A') ? profA : profB;
    res.confidence = std::min(1.0, (winningProf.bodyScore / 12.0));
    res.success = (winningProf.validSectionCount >= 3 && winningProf.bodyScore > 2.0);

    // Backward compatibility aliases
    res.angle = static_cast<float>(res.pointerAngle);
    res.angleDeg = static_cast<float>(res.pointerAngle);
    res.valid = res.success;
    res.score = static_cast<float>(winningProf.bodyScore);
    res.evidenceCoverage = static_cast<float>(winningProf.validLength / std::max(1.0, m_config.maxSearchRadius - m_config.pointerBaseRadius));
    res.finalScore = res.score;

    // Step 27: Log debug summary
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

    // Step 12: Full visual debug diagnostics rendering
    if (debugVis != nullptr) {
        renderDebugVisualization(*debugVis, effectivePivot, res);
        res.debugImage = debugVis->clone();
    }

    return res;
}

PointerDetectionResult PointerDetector::detect(const cv::Mat& grayOrBin, cv::Mat* debugVis) {
    cv::Point2d center(m_model.center.x, m_model.center.y);
    return detect(grayOrBin, center, debugVis);
}

// -------------------------------------------------------------------------
// Step 12: Comprehensive Visualization Overlay (Left/Right Edges, Centerline, HUD)
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

    // 4. Draw Final Directed Pointer Vector - Thin Crisp Lines (1 px)
    cv::line(canvas, res.tailPoint, res.tipPoint, cv::Scalar(0, 0, 255), 1, cv::LINE_AA);
    cv::circle(canvas, res.tipPoint, 4, cv::Scalar(0, 255, 0), -1, cv::LINE_AA);
    cv::drawMarker(canvas, res.tipPoint, cv::Scalar(0, 0, 255), cv::MARKER_CROSS, 8, 1);

    // Effective pivot point (Red)
    cv::circle(canvas, effectivePivot, 3, cv::Scalar(0, 0, 255), -1, cv::LINE_AA);
    cv::circle(canvas, effectivePivot, 5, cv::Scalar(0, 255, 255), 1, cv::LINE_AA);

    // 5. Section XX: Output HUD to a Dedicated Separate Window Image (DO NOT draw over canvas to avoid obstructing image)
    int hudW = 560;
    int hudH = 420;
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

    snprintf(buf, sizeof(buf), "Tracks: Cyan=L | Magenta=R | GreenDot=RawSupp | RedDot=NoSupp");
    cv::putText(hudMat, buf, cv::Point(16, lineY), cv::FONT_HERSHEY_SIMPLEX, 0.36, cv::Scalar(0, 229, 255), 1, cv::LINE_AA);
    hudStream << buf << "\n";

    res.hudImage = hudMat;
    res.hudText = hudStream.str();
}
