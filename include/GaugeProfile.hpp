#ifndef GAUGE_PROFILE_HPP
#define GAUGE_PROFILE_HPP

#include <opencv2/opencv.hpp>
#include <string>
#include <vector>

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
    std::string gauge_id{"GAUGE_DEFAULT"};
    std::string parent_model_path{""};
    std::string unit{"MPa"};
    
    double min_val{0.0};
    double max_val{1.0};
    
    // Angles in degrees (OpenCV Cartesian: 0° = 3 o'clock, 90° = 6 o'clock, 180° = 9 o'clock, 270° = 12 o'clock)
    double min_angle_deg{135.0}; // Angle at zero scale mark
    double max_angle_deg{45.0};  // Angle at max scale mark
    
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
    bool perspective_correction_enabled{true};
    std::vector<cv::Point2f> perspective_pts; // 4 points [12H Top, 3H Right, 6H Bottom, 9H Left]
    int calibrated_width{0};                   // Resolution width of original frame during 4-point calibration
    int calibrated_height{0};                  // Resolution height of original frame during 4-point calibration
    int rectified_size{800};                   // Square rectified front-view size
    int rectified_margin{40};                  // Margin padding for rectified dial rim

    // Image Preprocessing & Anti-Glare Parameters
    double clahe_clip_limit{2.5};             // CLAHE contrast limitation threshold
    int clahe_grid_size{8};                   // CLAHE grid tile size
    int gaussian_blur_size{7};                // Gaussian blur kernel size (odd px)
    double gaussian_blur_sigma{1.5};          // Gaussian blur sigma
    int blackhat_kernel_size{21};             // Black-Hat morphological kernel size (odd px)
    double blackhat_threshold{32.0};          // Black-Hat binary thresholding cutoff
    int local_bg_kernel_size{35};             // Adaptive background estimation kernel size (odd px)
    double adapt_diff_threshold{35.0};        // Adaptive background difference threshold
    double grad_mag_threshold{22.0};          // Sobel gradient magnitude threshold
    int morph_close_size{3};                  // Morphological closing kernel size (odd px)

    bool loadFromFile(const std::string& configFilePath, const std::string& targetCameraId = "");
    bool saveToFile(const std::string& configFilePath, const std::string& targetCameraId = "") const;
    static GaugeProfile createDefault();
};

#endif // GAUGE_PROFILE_HPP
