#ifndef GAUGE_READER_ENGINE_HPP
#define GAUGE_READER_ENGINE_HPP

#include "GaugeProfile.hpp"
#include "PointerDetector.hpp"
#include <opencv2/opencv.hpp>
#include <string>
#include <vector>

/**
 * @brief Detailed intermediate processing step image metadata and image container.
 */
struct ProcessingStepImage {
    int stepNumber{0};
    std::string title;       // e.g. "步骤 5: 高斯平滑降噪 (Gaussian Blur)"
    std::string description; // e.g. "7x7高斯核滤波，消除高频高光噪声与锯齿"
    cv::Mat image;           // Intermediate Mat image
};

struct GaugeResult {
    bool success{false};
    double value{0.0};
    std::string unit{"MPa"};
    double pointer_angle_deg{0.0};
    
    cv::Point2f center;
    float radius{0.0f};
    cv::Point2f pointer_tip;
    
    // PointerDetector detailed detection result & trajectory metrics
    PointerResult pointerDetectorResult;

    // 4 Main Pipeline Stage Images (原图 - 矫正 - 图像预处理 - 识别)
    cv::Mat step1_original;       // Step 1: Raw input original frame
    cv::Mat step2_rectified;      // Step 2: Rectified front-view dial image
    cv::Mat step3_preprocessed;   // Step 3: Preprocessed (Grayscale, Gaussian Blur & Edge/Threshold) image
    cv::Mat step4_recognition;    // Step 4: Final recognition overlay result image

    // Detailed Stage Visualization Images (for internal analysis)
    cv::Mat originalImage;       // Stage 1
    cv::Mat rectifiedImage;      // Stage 2
    cv::Mat grayImage;           // Stage 3
    cv::Mat blurredImage;        // Stage 3
    cv::Mat hubRoiImage;         // Stage 4 helper
    cv::Mat needleBinaryImage;   // Stage 4 helper
    cv::Mat overlayImage;        // Stage 4

    // Complete Step-by-Step Processing Images for visual inspection & tuning
    std::vector<ProcessingStepImage> intermediateSteps;

    // Fail-Safe Secondary Check Metrics (防错位二次检测指标)
    bool centerInconsistent{false};
    double centerOffsetDist{0.0};
    std::string warningMessage;

    // Step 12 Separate Diagnostic Window Output
    cv::Mat diagnosticHudImage;
    std::string diagnosticText;

    std::string errorMessage;
    double confidence{0.0};
};

class GaugeReaderEngine {
public:
    GaugeReaderEngine() = default;
    ~GaugeReaderEngine() = default;

    /**
     * @brief Process an input gauge frame (4 steps: Original -> FOV Correction -> Preprocessing -> Recognition).
     */
    GaugeResult process(const cv::Mat& frame, GaugeProfile& profile);

    /**
     * @brief Rectify perspective distortion using 4 cardinal tangent points (12H Top, 3H Right, 6H Bottom, 9H Left).
     */
    cv::Mat rectifyPerspective(const cv::Mat& src, const std::vector<cv::Point2f>& quadPts, cv::Mat& invH, int outSize = 800, int margin = 40);

    /**
     * @brief Automatically detect dial rim 4 cardinal points (Top, Right, Bottom, Left) matching fov_corrector.py algorithm.
     */
    static std::vector<cv::Point2f> autoDetectCardinalPoints(const cv::Mat& image);

    /**
     * @brief Locate circular dial ROI in the image.
     */
    static bool locateDialCircle(const cv::Mat& gray, cv::Point2f& center, float& radius, cv::Mat* outHubVis = nullptr);

    /**
     * @brief Detect pointer angle within valid scale angular range.
     */
    double detectPointerAngleRobust(const cv::Mat& gray, const cv::Point2f& center, float radius, const GaugeProfile& profile, cv::Mat* outScanVis = nullptr);

private:
    void drawOverlay(cv::Mat& canvas, const GaugeProfile& profile, const GaugeResult& res, const cv::Mat& invH);
    static double normalizeAngle(double angleDeg);
};

#endif // GAUGE_READER_ENGINE_HPP
