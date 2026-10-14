#ifndef GAUGE_PROFILE_HPP
#define GAUGE_PROFILE_HPP

#include <opencv2/opencv.hpp>
#include <string>
#include <vector>
#include <QJsonObject>
#include <QJsonArray>

/**
 * @brief Calibration Profile for an Individual Gauge.
 * 
 * Supports physical measurement range, angles, manual ROI,
 * perspective correction, and pointer tip/tail orientation inversion.
 */
/**
 * @brief Geographic Location (GIS) Parameters.
 */
struct GisInfo {
    double longitude{118.7965};
    double latitude{32.0584};
    double altitude{15.5};
};

/**
 * @brief Physical Installation Location Info.
 */
struct LocationInfo {
    std::string substation{"南京一期变电站"};
    std::string area{"10kV 高压室"};
    std::string device_name{"#3 变压器压力监控位"};
    std::string device_tag{"PT-2026-05A"};
};

/**
 * @brief Meter Head Hardware Specifications.
 */
struct GaugeSpecInfo {
    std::string gauge_type{"弹簧管耐震压力表"};
    std::string manufacturer{"红旗仪表有限公司"};
    std::string model_name{"Y-100BF"};
    std::string accuracy_class{"1.6级"};
    double range_min{0.0};
    double range_max{1.6};
    std::string unit{"MPa"};
};

/**
 * @brief Calibration Profile & Asset Descriptor for an Individual Gauge.
 * 
 * Supports physical measurement range, angles, manual ROI,
 * perspective correction, GIS coordinates, installation location, and meter head specs.
 */
struct GaugeProfile {
    QJsonObject qualityConfig;
    std::string gauge_id{"GAUGE_DEFAULT"};
    std::string parent_model_path{""};
    std::string unit{"MPa"};
    
    double min_val{0.0};
    double max_val{1.0};
    
    // Direction in which the manually calibrated scale values increase.
    bool clockwise{true};
    
    // Pointer Needle Tip vs Tail Orientation Inversion
    bool pointer_invert_direction{true};

    // PointerDetector Detection Parameters
    double pointerBaseRadius{50.0};       // Radius offset to skip center hub cap / pin cap (px)
    double pointerMinWidth{2.0};          // Minimum valid section width (px)
    double pointerMaxWidth{35.0};         // Maximum valid section width (px)
    double pointerMinLength{80.0};        // Minimum required pointer length (px)
    double coarseAngleStep{0.4};          // Stage 1 coarse search angle step (deg)
    double fineAngleStep{0.03};           // Stage 2 fine search angle step (deg)
    double fineAngleRange{2.5};           // Stage 2 fine search range (+/- deg)
    double minValidSectionRatio{0.65};    // Minimum ratio of valid cross-sections
    double maxAllowedWidthStep{6.0};      // Maximum allowed width change step (px)
    double maxCenterlineOffset{8.0};      // Maximum allowed centerline offset (px)

    // Anti-Interference / Occlusion Bridging Parameters (Tolerate text & scale marks)
    int maxConsecutiveGapSamples{16};         // Maximum consecutive gap samples before termination (allows crossing ~32px text)
    double minEdgeSupportDensity{0.45};       // Minimum verified edge point ratio along track (allows text occlusion)
    double rawGradientThreshold{45.0};        // Minimum raw Scharr gradient magnitude for valid edge
    double minGradientNormalAlignment{0.72};  // Perpendicular alignment |dot(grad, normal)|
    double minLocalStepContrast{12.0};        // Minimum contrast across needle edge
    double maxStartBaseGapPx{16.0};           // Hub search gap tolerance
    double maxParallelAngleDiffDeg{15.0};     // Angle divergence tolerance for tapered needles
    double minCommonLengthPx{35.0};           // Minimum required pointer length overlap
    double maxLineFitResidualPx{2.2};         // Maximum orthogonal residual to straight line
    int maxOcclusionGap{12};                  // Maximum radial section bridge gap across text
    double corridorHalfWidthPx{22.0};         // Lateral search corridor half-width

    // Pointer Tail (Counterweight) Detection Parameters
    bool enable_tail_detection{true};         // Enable joint needle tail detection
    double tail_max_radius{130.0};            // Maximum radial search reach for tail (px)
    double tail_min_width{4.0};               // Minimum allowed tail width (px)
    double tail_max_width{65.0};              // Maximum allowed tail width (px, wider for counterweight)
    double tail_corridor_half_width{45.0};    // Search corridor half-width for tail (px)
    double tail_min_gradient_normal_alignment{0.50}; // Alignment threshold for widening tail edge (relaxed vs slender body)

    // Optical Polarity & Dark Entity Constraints
    bool require_dark_pointer{true};          // Enforce needle body and tail are dark entities on a lighter dial
    double min_pointer_dark_contrast{10.0};   // Minimum (meanOutsideGray - meanInsideGray)
    double max_pointer_inside_gray{165.0};    // Maximum average gray level inside pointer entity

    // Pointer Structure & Confidence Scoring Weights
    double w_geom_weight{4.0};                // Weight for first-layer candidate geometry score
    double w_edge_pair_coherence{3.5};        // Weight for mutual bilateral edge pair coherence
    double w_center_stability{3.0};           // Weight for centerline variance and stability
    double w_axis_center{2.5};                // Weight for transverse offset penalty from axis
    double w_orientation{2.0};                // Weight for gradient normal orientation alignment
    double w_reach{2.5};                      // Weight for radial pointer reach
    double w_valid_length{2.5};               // Weight for continuous valid length
    double w_slenderness{3.0};                // Weight for slenderness ratio (length / width)
    double w_persistence{3.0};                // Weight for bilateral edge persistence
    double w_width_trend{1.5};                // Weight for tapering/parallel width trend
    double w_width_continuity{1.5};           // Weight for section-to-section width smoothness
    double w_edge_strength{0.3};              // Weight for edge gradient magnitude (auxiliary)
    double w_gap_penalty{1.5};                // Penalty weight for maximum occlusion gap
    double w_tracking_jump_penalty{2.5};       // Penalty weight for lateral tracking jumps
    double w_tail_score{1.8};                 // Weight for counterweight tail score in joint evaluation
    double w_tail_polarity_boost{2.5};        // Boost weight for tail in needle head polarity resolution
    double confidence_divisor{12.0};          // Normalization divisor to convert bodyScore to confidence [0.0 ~ 1.0]

    // Asset Metadata: GIS, Location & Specifications
    GisInfo gis;
    LocationInfo location;
    GaugeSpecInfo spec;

    // Optional preset ROI & constrained tolerance search
    cv::Point2f center{-1.0f, -1.0f};
    float radius{-1.0f};
    bool auto_detect_roi{true};
    float center_search_tolerance{35.0f};

    // Camera Perspective Distortion Correction Parameters
    QJsonArray scalePoints; // Device calibration, never a template parameter.
    bool perspective_correction_enabled{true};
    std::vector<cv::Point2f> perspective_pts; // 4 points [12H Top, 3H Right, 6H Bottom, 9H Left]
    int calibrated_width{0};                   // Resolution width of original frame during 4-point calibration
    int calibrated_height{0};                  // Resolution height of original frame during 4-point calibration
    int rectified_size{800};                   // Square rectified front-view size
    int rectified_margin{40};                  // Margin padding for rectified dial rim

    // Optional diagnostic image output
    bool save_debug_images{false};            // Save core images and HUD to debug_images (disabled by default)

    bool loadFromFile(const std::string& configFilePath, const std::string& targetCameraId = "");
    bool loadFromJson(const QJsonObject& definition, const std::string& targetCameraId = "");
    QJsonObject toJson() const;
    bool saveToFile(const std::string& configFilePath, const std::string& targetCameraId = "") const;
    static GaugeProfile createDefault();
private:
    bool loadContent(const std::string& content, const std::string& targetCameraId,
                     const std::string& resolvedPath, bool allowFileInheritance);
};

#endif // GAUGE_PROFILE_HPP
