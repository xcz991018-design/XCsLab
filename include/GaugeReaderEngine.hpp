#ifndef GAUGE_READER_ENGINE_HPP
#define GAUGE_READER_ENGINE_HPP

#include "GaugeProfile.hpp"
#include "PointerDetector.hpp"
#include <opencv2/opencv.hpp>
#include <string>
#include <vector>

enum class GaugeRunMode { Production, Evidence, Debug };

inline const char* gaugeRunModeName(GaugeRunMode mode) {
    switch (mode) {
    case GaugeRunMode::Production: return "production";
    case GaugeRunMode::Evidence: return "evidence";
    case GaugeRunMode::Debug: return "debug";
    }
    return "production";
}

struct GaugeResult {
    GaugeRunMode runMode{GaugeRunMode::Debug};
    bool success{false};
    double value{0.0};
    std::string unit{"MPa"};
    double pointer_angle_deg{0.0};
    
    cv::Point2f center;
    float radius{0.0f};
    cv::Point2f pointer_tip;
    
    // PointerDetector detailed detection result & trajectory metrics
    PointerResult pointerDetectorResult;

    // 4 Main Pipeline Stage Images (原图 - 矫正 - 灰度图 - 识别)
    cv::Mat step1_original;       // Step 1: Raw input original frame
    cv::Mat step2_rectified;      // Step 2: Rectified front-view dial image
    cv::Mat step3_gray;   // Step 3: Actual grayscale detector input (BGR for display)
    cv::Mat step4_recognition;    // Step 4: Final recognition overlay result image

    // Detailed Stage Visualization Images (for internal analysis)
    cv::Mat originalImage;       // Stage 1
    cv::Mat rectifiedImage;      // Stage 2
    cv::Mat grayImage;           // Stage 3
    cv::Mat overlayImage;        // Stage 4

    // Fail-Safe Secondary Check Metrics (防错位二次检测指标)
    bool centerInconsistent{false};
    double centerOffsetDist{0.0};
    std::string warningMessage;

    // Pointer detection diagnostic Window Output
    cv::Mat diagnosticHudImage;
    std::string diagnosticText;

    std::string errorMessage;
    std::string errorStep;               // Pipeline step where error occurred
    std::string detailedDebugInfo;       // Detailed debug diagnostic text
    double confidence{0.0};
    QJsonObject stageTimings; // Monotonic elapsed milliseconds, independent of run mode.
    QJsonObject recognitionQuality; // Advisory; never changes success/value/confidence.
};

class GaugeReaderEngine {
public:
    GaugeReaderEngine() = default;
    ~GaugeReaderEngine() = default;

    /**
     * @brief Process an input gauge frame (4 steps: Original -> FOV Correction -> Grayscale -> Recognition).
     */
    GaugeResult process(const cv::Mat& frame, GaugeProfile& profile,
                        GaugeRunMode mode = GaugeRunMode::Debug);

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
    static bool locateDialCircle(const cv::Mat& gray, cv::Point2f& center, float& radius);

    void drawOverlay(cv::Mat& canvas, const GaugeProfile& profile, const GaugeResult& res, const cv::Mat& invH, double minAngle, double maxAngle);
    static double normalizeAngle(double angleDeg);
};

#endif // GAUGE_READER_ENGINE_HPP
