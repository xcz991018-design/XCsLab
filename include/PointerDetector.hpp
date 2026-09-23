#ifndef POINTER_DETECTOR_HPP
#define POINTER_DETECTOR_HPP

#include <opencv2/opencv.hpp>
#include <vector>
#include <string>

// -------------------------------------------------------------------------
// Legacy compatibility structures (preserved for existing dialogs and profiles)
// -------------------------------------------------------------------------
enum class RegionType {
    Tip = 0,
    Body = 1,
    Tail = 2
};

struct TemplatePoint {
    cv::Point2f localPt;
    cv::Point2f normal;
    float weight{1.0f};
    RegionType regionType{RegionType::Body};
};

struct PointerShapeModel {
    cv::Point2f center{400.0f, 400.0f};
    float radius{360.0f};

    float pointerMinAngle{135.0f};
    float pointerMaxAngle{45.0f};
    bool clockwise{true};

    float centerExclusionRadius{25.0f};

    std::vector<cv::Point2f> customTipPoints;
    std::vector<cv::Point2f> customBodyPoints;
    std::vector<cv::Point2f> customTailPoints;

    float tipMinRadius{40.0f};
    float tipMaxRadius{340.0f};
    float tipBaseWidth{12.0f};

    float bodyMinRadius{30.0f};
    float bodyMaxRadius{320.0f};
    float bodyWidth{12.0f};

    float tailMinRadius{15.0f};
    float tailMaxRadius{90.0f};
    float tailWidth{26.0f};

    float pointerMinWidth{2.0f};
    float pointerMaxWidth{35.0f};
    float pointerMinLength{80.0f};

    float radialStep{2.5f};
    float dtSigma{3.5f};
    float maxHitDistance{8.0f};
    float minValidCoverage{0.15f};

    float coarseAngleStep{0.5f};
    float fineAngleStep{0.05f};
    float fineAngleRange{2.0f};

    float wTip{2.5f};
    float wBody{1.5f};
    float wTail{2.0f};
    float wAxis{2.0f};
    float wCoverage{1.0f};
};

struct GaugeConfig {
    cv::Point2f center{400.0f, 400.0f};
    double radius{360.0};
    double pointerMinAngle{135.0};
    double pointerMaxAngle{45.0};
    bool clockwise{true};

    double pointerBaseRadius{45.0};
    double pointerMinWidth{2.0};
    double pointerMaxWidth{35.0};
    double pointerMinLength{80.0};

    double coarseAngleStep{0.5};
    double fineAngleStep{0.05};
    double fineAngleRange{2.0};

    double minValidSectionRatio{0.65};
    double maxAllowedWidthStep{6.0};
    double maxCenterlineOffset{8.0};
    double maxOcclusionGapPx{20.0};

    double widthTrendWeight{1.5};
    double lengthWeight{2.0};
    double continuityWeight{1.5};
    double centerlineWeight{2.0};
    double widthWeight{1.0};

    PointerShapeModel toShapeModel() const {
        PointerShapeModel m;
        m.center = center;
        m.radius = static_cast<float>(radius);
        m.pointerMinAngle = static_cast<float>(pointerMinAngle);
        m.pointerMaxAngle = static_cast<float>(pointerMaxAngle);
        m.clockwise = clockwise;
        m.centerExclusionRadius = static_cast<float>(std::max(15.0, pointerBaseRadius * 0.5));
        m.pointerMinWidth = static_cast<float>(pointerMinWidth);
        m.pointerMaxWidth = static_cast<float>(pointerMaxWidth);
        m.pointerMinLength = static_cast<float>(pointerMinLength);
        m.radialStep = 2.5f;
        m.coarseAngleStep = static_cast<float>(coarseAngleStep);
        m.fineAngleStep = static_cast<float>(fineAngleStep);
        m.fineAngleRange = static_cast<float>(fineAngleRange);
        return m;
    }
};

// =========================================================================
// Core Data Structures for Long Parallel Edge Pair & Candidate Region
// =========================================================================

/**
 * @brief Section V: A single tracked edge point along a radial ray.
 */
struct EdgePoint {
    double radius{0.0};             // Radial distance r from pivot
    double t{0.0};                  // Lateral distance from axis along normal v

    double x{0.0};                  // Image coordinate x
    double y{0.0};                  // Image coordinate y

    double strength{0.0};           // Gradient magnitude / edge score
    double orientation{0.0};        // Local gradient normal / orientation score
    double rawAngle{0.0};           // Local gradient angle

    bool hasRawSupport{false};      // Verified by raw Scharr magnitude & direction
    bool valid{false};
};

/**
 * @brief Section V: Continuous tracked edge trajectory along one side of candidate corridor.
 */
struct EdgeTrack {
    std::vector<EdgePoint> points;

    double startRadius{0.0};
    double endRadius{0.0};
    double length{0.0};

    double meanStrength{0.0};
    double medianStrength{0.0};

    double lineAngle{0.0};          // Fitted line angle in degrees [0, 360)
    double straightness{0.0};       // Straightness score in [0, 1] (residual & orientation variance)
    double curvature{0.0};          // Average local curvature / lateral deviation
    double lineFitResidual{0.0};    // Mean orthogonal distance to fitted straight line (px)

    double continuity{0.0};         // Tracking continuity ratio in [0, 1]
    double edgeSupportDensity{0.0}; // Ratio of raw-supported samples to total samples (HARD GATE)
    double baseDistance{0.0};       // Distance from pointerBaseRadius to startRadius

    int validPointCount{0};
    int verifiedSupportCount{0};
    int maxConsecutiveGap{0};
    int jumpCount{0};
    double trackingJumpPenalty{0.0};

    bool valid{false};
};

/**
 * @brief Section V: Data structure for an individual radial cross section.
 */
struct RadialSection {
    double radius{0.0};             // Radial distance from pivot along candidate axis

    bool leftValid{false};
    bool rightValid{false};
    bool edgePairCoherent{false};   // Whether left & right edges are mutually coherent
    bool centerStable{false};       // Whether centerT is stable
    bool trackingStable{false};     // Whether tracking was maintained without jump
    bool valid{false};              // Strictly true only if leftValid && rightValid && coherent && stable

    double leftT{0.0};              // Left edge coordinate along transverse normal v (< 0)
    double rightT{0.0};             // Right edge coordinate along transverse normal v (> 0)

    double width{0.0};              // Cross section width = rightT - leftT
    double centerT{0.0};            // Center lateral offset = (leftT + rightT) / 2.0

    double leftStrength{0.0};
    double rightStrength{0.0};

    double leftDirectionScore{0.0};
    double rightDirectionScore{0.0};

    cv::Point2d leftPoint;          // Image space coordinate of left edge point
    cv::Point2d rightPoint;         // Image space coordinate of right edge point
    cv::Point2d centerPoint;        // Image space coordinate of section center point

    // Backward compatibility aliases
    double leftScore{0.0};
    double rightScore{0.0};
    double pairScore{0.0};
};

using PointerCrossSection = RadialSection;

/**
 * @brief Section XIII: Candidate Region formed by a long parallel edge pair.
 */
struct PointerCandidateRegion {
    double axisAngle{0.0};

    EdgeTrack leftEdge;
    EdgeTrack rightEdge;

    double commonStartRadius{0.0};
    double commonEndRadius{0.0};
    double commonLength{0.0};

    double meanWidth{0.0};
    double medianWidth{0.0};

    double widthSlope{0.0};
    double widthContinuity{0.0};

    double parallelism{0.0};        // Parallelism score [0, 1] (angle difference)
    double straightness{0.0};       // Combined straightness of left & right tracks
    double leftStraightness{0.0};
    double rightStraightness{0.0};

    double leftContinuity{0.0};
    double rightContinuity{0.0};

    double leftSupportDensity{0.0}; // Ratio of raw-supported points on left track (HARD GATE)
    double rightSupportDensity{0.0};// Ratio of raw-supported points on right track (HARD GATE)
    double edgeSupportDensity{0.0}; // Mean support density of left & right tracks

    double baseProximity{0.0};      // Proximity of startRadius to pointerBaseRadius

    double geometryScore{0.0};      // Phase 1 Geometric gate score
    double bodyScore{0.0};          // Phase 2 Width & body score

    bool geometryValid{false};      // Passes long parallel geometry gate
};

/**
 * @brief Overall evaluation metrics for a candidate non-directional axis theta in [0, 180°).
 */
struct PointerAxisCandidate {
    double theta{0.0};              // Candidate axis angle in [0, 180°)

    std::vector<RadialSection> sections; // All radial cross sections

    // Specific metrics requested for candidate ranking and debug analysis
    double reach{0.0};              // Maximum radial reach (px)
    double coverageLength{0.0};     // Valid continuous radial coverage length (px)
    double meanWidth{0.0};          // Average width of valid cross-sections (px)
    double medianWidth{0.0};        // Median width of valid cross-sections (px)
    double slenderness{0.0};        // Slenderness ratio = coverageLength / max(medianWidth, 1e-3)
    double edgeStrength{0.0};       // Average raw gradient magnitude of edges
    double edgePersistence{0.0};    // Bilateral edge presence rate along radial axis
    double bilateralPersistence{0.0};
    double widthSlope{0.0};         // Linear regression slope dW/dr
    double widthContinuity{0.0};    // Smoothness and continuity of width variation W(r)
    double centerStability{0.0};    // Centerline stability and smoothness across radius
    double leftEdgeStability{0.0};  // Left edge independent trajectory stability
    double rightEdgeStability{0.0}; // Right edge independent trajectory stability

    // Edge Pair & Candidate Region Geometric metrics
    double leftLength{0.0};         // Left edge track length (px)
    double rightLength{0.0};        // Right edge track length (px)
    double commonLength{0.0};       // Common overlap length (px)
    double leftStraightness{0.0};   // Left edge straightness
    double rightStraightness{0.0};  // Right edge straightness
    double parallelism{0.0};        // Left/Right parallelism score
    double leftContinuity{0.0};     // Left edge trajectory smoothness
    double rightContinuity{0.0};    // Right edge trajectory smoothness
    double leftSupportDensity{0.0}; // Raw edge support density on left track
    double rightSupportDensity{0.0};// Raw edge support density on right track
    double edgeSupportDensity{0.0}; // Combined raw edge support density
    double baseProximity{0.0};      // Start radius proximity to pointerBaseRadius
    double geometryScore{0.0};      // Geometric gate score
    bool geometryValid{false};

    // Edge Pair Coherence metrics
    double pairMotionConsistency{0.0};  // Synchronization of left and right motion
    double axisCenterConsistency{0.0};  // Consistency of centerT near 0.0 (the axis)
    double orientationConsistency{0.0}; // Edge gradient perpendicularity to axis
    double trackingJumpPenalty{0.0};    // Penalty for lateral edge jumping
    double edgePairCoherence{0.0};      // Comprehensive Edge Pair Coherence score

    double bodyScore{0.0};          // Comprehensive body score
    double totalScore{0.0};         // Total non-directional structural score

    // Backward compatibility aliases
    double edgeScore{0.0};
    double widthContinuityScore{0.0};
    double centerContinuityScore{0.0};
    double leftContinuityScore{0.0};
    double rightContinuityScore{0.0};
    double coverageScore{0.0};
    double occlusionScore{0.0};

    int validSectionCount{0};       // Total count of valid cross sections
    int maxGapLength{0};            // Maximum consecutive invalid section gap

    double startRadius{0.0};        // Starting radius of effective continuous pointer body
    double endRadius{0.0};          // Ending radius of effective continuous pointer body
    double tangentialOffset{0.0};   // Measured tangential offset d_perp from pivot
};

/**
 * @brief Centralized configuration parameters for PointerDetector.
 */
struct PointerDetectorConfig {
    double angleStep{1.0};          // Phase 1 coarse search step in [0, 180°) (degrees, default 1.0)
    double radialStepPx{2.0};       // Radial sampling step along needle axis (px, default 2.0 px)
    double radialStep{2.0};         // Alias for radialStepPx

    double pointerBaseRadius{45.0}; // Hub exclusion radius, sampling starts at r >= pointerBaseRadius
    double maxSearchRadius{350.0};  // Maximum radial reach to search (px)

    // Section III: Search corridor bounds
    double corridorHalfWidthPx{15.0}; // Limited transverse corridor half-width [-15, +15] px
    double maxHalfWidth{26.0};      // Transverse profile half-width t in [-maxHalfWidth, +maxHalfWidth]
    double minPointerWidth{1.5};    // Minimum valid needle width (px)
    double maxPointerWidth{26.0};   // Maximum valid needle width (px)

    double maxCenterOffset{12.0};   // Maximum allowed lateral shift |centerT|
    double maxWidthChange{4.5};     // Maximum allowed adjacent section width step (px)
    double maxCenterChange{4.0};    // Maximum allowed adjacent section center step (px)
    double maxEdgeChange{4.5};      // Maximum allowed adjacent individual edge step (px)

    // Edge Tracking parameters
    double trackingWindowPx{3.0};   // Local lateral tracking search window around prior edge (px)
    double trackingWindow{3.0};     // Alias for trackingWindowPx
    double maxTrackingJumpPx{3.5};  // Maximum allowed single-step edge jump (px)

    // Section IX, X, XI, XII: Geometric constraints
    double minCommonLengthPx{50.0};       // Minimum required common radial overlap length (px)
    double maxParallelAngleDiffDeg{5.0};  // Maximum allowed angle difference between left & right tracks (deg)
    double maxBaseDistancePx{20.0};       // Maximum allowed start distance from pointerBaseRadius (px)
    double maxLineFitResidualPx{1.8};     // Maximum orthogonal residual to straight line (px) - hard straightness gate

    // Section II, III, V, VI: Raw Edge Support Verification & Hard Gates
    double minEdgeSupportDensity{0.75};   // Minimum ratio of verified raw edge points along EdgeTrack (HARD GATE)
    int maxConsecutiveGapSamples{2};      // Maximum consecutive samples without raw edge support before track is TERMINATED
    double rawGradientThreshold{55.0};    // Minimum absolute raw Scharr gradient magnitude for genuine edge support
    double minGradientNormalAlignment{0.85}; // Minimum |dot(gradUnit, normal)| to count as edge perpendicular to axis
    double minLocalStepContrast{16.0};    // Minimum peak-to-peak intensity contrast in transverse slice (px)
    double maxStartBaseGapPx{8.0};        // Maximum radial distance from pointerBaseRadius to start finding edge

    int maxOcclusionGap{2};         // Maximum consecutive invalid sections to bridge across occlusions (capped at 2)
    double maxInvalidGapPx{6.0};    // Maximum invalid gap in pixels (6.0 px)

    double gradientThreshold{55.0}; // Minimum Scharr magnitude to count as candidate edge

    // Weights for Body Score & Edge Pair Coherence
    double wEdgePairCoherence{3.5}; // High weight for mutual edge coherence
    double wCenterStability{3.0};   // High weight to penalize lateral drift
    double wAxisCenter{2.5};        // High weight to penalize centerT offset from axis
    double wOrientation{2.0};       // Gradient orientation alignment
    double wReach{2.5};
    double wValidLength{2.5};
    double wSlenderness{3.0};
    double wPersistence{3.0};
    double wWidthTrend{1.5};
    double wWidthContinuity{1.5};
    double wEdgeStrength{0.3};      // Auxiliary only! Never dominates candidate ranking
    double wTrackingJumpPenalty{2.5};
    double wGapPenalty{1.5};

    // Backward compatibility aliases
    double slendernessWeight{3.5};
    double coverageLengthWeight{2.5};
    double edgePersistenceWeight{3.0};
    double axisStabilityWeight{1.5};
    double widthContinuityWeight{1.5};
    double edgeStrengthAuxWeight{0.5};
    double tipTaperWeight{0.5};
    double tipConvergenceWeight{0.5};
    double tipTerminalWeight{0.5};
    double tipContinuityWeight{0.5};
    double wJumpPenalty{1.5};
};

/**
 * @brief Global precomputed Scharr gradient feature maps.
 */
struct GradientFeatureMap {
    cv::Mat gradMag;                // CV_32F magnitude of Scharr gradient
    cv::Mat gradUnitX;              // CV_32F unit gradient X component: Gx / Mag
    cv::Mat gradUnitY;              // CV_32F unit gradient Y component: Gy / Mag
};

/**
 * @brief Section XVI: Radial Direction Profile along a ray (phi in [0, 360)).
 */
struct RadialDirectionProfile {
    double angle{0.0};

    std::vector<RadialSection> sections;

    double reach{0.0};              // Maximum radial reach from pivot (px)
    double validLength{0.0};        // Total radial coverage length of valid sections (px)

    double medianWidth{0.0};        // Median width across valid sections (px)
    double meanWidth{0.0};          // Mean width across valid sections (px)

    double slenderness{0.0};        // Slenderness ratio = validLength / max(medianWidth, 1e-3)

    double bilateralPersistence{0.0}; // Valid bilateral section count / total sections
    double leftPersistence{0.0};      // Left edge presence rate
    double rightPersistence{0.0};     // Right edge presence rate

    double widthSlope{0.0};         // Linear regression slope dW/dr
    double widthContinuity{0.0};    // Smoothness and continuity of width variation W(r)

    double centerStability{0.0};    // Centerline stability and smoothness across radius

    // Edge Pair Coherence metrics
    double leftContinuity{0.0};         // Left edge trajectory smoothness
    double rightContinuity{0.0};        // Right edge trajectory smoothness
    double pairMotionConsistency{0.0};  // Synchronization of left and right motion
    double axisCenterConsistency{0.0};  // Consistency of centerT near 0.0 (the axis)
    double orientationConsistency{0.0}; // Edge gradient perpendicularity to axis
    double trackingJumpPenalty{0.0};    // Penalty for lateral edge jumping
    double edgePairCoherence{0.0};      // Comprehensive Edge Pair Coherence score

    // Candidate Region & Geometric metrics
    double leftLength{0.0};
    double rightLength{0.0};
    double commonLength{0.0};
    double leftStraightness{0.0};
    double rightStraightness{0.0};
    double parallelism{0.0};
    double baseProximity{0.0};
    double leftSupportDensity{0.0};
    double rightSupportDensity{0.0};
    double edgeSupportDensity{0.0};
    double geometryScore{0.0};
    bool geometryValid{false};

    PointerCandidateRegion candidateRegion;

    double meanEdgeStrength{0.0};   // Auxiliary edge strength
    double densityScore{0.0};       // Section density score
    double bodyScore{0.0};          // Comprehensive body score

    bool valid{false};              // Whether this profile contains an effective continuous body

    // Tracking & auxiliary diagnostics
    int validSectionCount{0};
    int invalidSectionCount{0};
    int maxGap{0};
    double startRadius{0.0};
    double endRadius{0.0};
    double tangentialOffset{0.0};
    double edgeStrength{0.0};
    double widthJumpPenalty{0.0};
    double gapPenalty{0.0};
    double taperScore{0.0};
    double terminalScore{0.0};
    double directionScore{0.0};     // Alias for bodyScore
    double edgePersistence{0.0};    // Alias for bilateralPersistence
};

using DirectionProfile = RadialDirectionProfile;

/**
 * @brief Section I & XIII: Final Detection Result of PointerDetector.
 */
struct PointerDetectionResult {
    bool success{false};

    double axisAngle{0.0};          // Non-directional principal axis angle in [0, 180°)
    double pointerAngle{0.0};       // Directed needle tip angle in [0, 360°)

    double startRadius{0.0};        // Valid pointer body start radius (px)
    double endRadius{0.0};          // Valid pointer body end radius (px)

    cv::Point2d tipPoint;           // Predicted needle tip position in image
    cv::Point2d effectivePivot;     // Effective center of needle rotation

    double confidence{0.0};         // Overall detection confidence in [0, 1]

    char headEnd{'A'};              // 'A' or 'B'
    char tailEnd{'B'};              // 'A' or 'B'
    double tipLikelihoodA{0.0};
    double tipLikelihoodB{0.0};
    double tipLikelihoodMargin{0.0};
    std::string polarityConfidence{"LOW"}; // "HIGH", "MEDIUM", "LOW"

    // Bidirectional profiles for Direction A (+axis) and Direction B (-axis)
    DirectionProfile directionA;
    DirectionProfile directionB;
    double reachForward{0.0};       // Maximum reach along Direction A (px)
    double reachBackward{0.0};      // Maximum reach along Direction B (px)

    // Detailed diagnostic cross sections and candidate data
    std::vector<PointerCrossSection> sections;
    std::vector<PointerAxisCandidate> topCandidates;

    // Backward compatibility fields
    float angleDeg{0.0f};           // Alias for pointerAngle
    float angle{0.0f};              // Alias for pointerAngle
    bool valid{false};              // Alias for success
    float score{0.0f};              // Overall score
    float tipRadius{0.0f};          // Distance from effectivePivot to tipPoint
    cv::Point2f tailPoint;          // Counterweight tail end position

    float tipMatch{0.0f};
    float bodyMatch{0.0f};
    float tailMatch{0.0f};
    float evidenceCoverage{0.0f};
    float axisConsistency{0.0f};
    float finalScore{0.0f};

    cv::Mat debugImage;
    cv::Mat hudImage;               // Section XX: Separate HUD diagnostic window image
    std::string hudText;            // Formatted diagnostic metrics text
};

using PointerResult = PointerDetectionResult;

/**
 * @brief Industrial Mechanical Gauge Pointer Detector based on Radial Cross-Section Bilateral Edge Tracking.
 */
class PointerDetector {
public:
    PointerDetector();
    explicit PointerDetector(const PointerDetectorConfig& config);
    explicit PointerDetector(const PointerShapeModel& model);
    explicit PointerDetector(const GaugeConfig& config);
    ~PointerDetector() = default;

    void setConfig(const PointerDetectorConfig& config);
    void setConfig(const GaugeConfig& config);
    void setModel(const PointerShapeModel& model);

    PointerDetectorConfig getConfig() const;
    PointerShapeModel getModel() const;

    /**
     * @brief Detect pointer direction and angle using Scharr Gradient + Radial Cross-Section Bilateral Edge Tracking.
     * @param gray Input grayscale image (dial ROI or full rectified frame).
     * @param dialCenter Nominal dial center point.
     * @param debugVis Optional visualization canvas to render left/right edges, centerline, and HUD.
     * @return PointerDetectionResult containing axisAngle, pointerAngle, effectivePivot, sections, and HUD.
     */
    PointerDetectionResult detect(const cv::Mat& gray, const cv::Point2d& dialCenter, cv::Mat* debugVis = nullptr);

    /**
     * @brief Legacy detect entry taking single Mat and optional debug canvas.
     */
    PointerDetectionResult detect(const cv::Mat& grayOrBin, cv::Mat* debugVis = nullptr);

    /**
     * @brief Helper to normalize angle into [0, 360) range.
     */
    static double normalizeAngle(double angleDeg);

    /**
     * @brief Compatibility helper for dialog template visualization.
     */
    static std::vector<TemplatePoint> buildUnifiedTemplate(const PointerShapeModel& model);

private:
    // Step 2: Compute single-pass Scharr gradient magnitude and unit orientation
    GradientFeatureMap computeGradientFeatures(const cv::Mat& gray);

    // Section VI: Track a single continuous edge in the candidate corridor (isLeft: t < 0 vs !isLeft: t > 0)
    EdgeTrack trackEdgeInCorridor(
        const cv::Mat& gray,
        const GradientFeatureMap& features,
        const cv::Point2d& effectivePivot,
        double thetaDeg,
        bool isLeft);

    // Section XIII & XIV: Form Candidate Region from Left & Right EdgeTracks and validate geometry + width
    PointerCandidateRegion evaluateCandidateRegion(
        const cv::Mat& gray,
        const GradientFeatureMap& features,
        const cv::Point2d& effectivePivot,
        double thetaDeg);

    // Step 3: Extract cross section bilateral edge pair for candidate (theta, r) with Edge Tracking
    PointerCrossSection extractCrossSection(
        const cv::Mat& gray,
        const GradientFeatureMap& features,
        const cv::Point2d& effectivePivot,
        double thetaDeg,
        double r,
        double prevLeftT = -999.0,
        double prevRightT = -999.0,
        double previousWidth = -1.0,
        double previousCenterT = 0.0);

    // Step 4 & 5: Evaluate non-directional candidate axis theta in [0, 180°)
    PointerAxisCandidate evaluateCandidateAxis(
        const cv::Mat& gray,
        const GradientFeatureMap& features,
        const cv::Point2d& effectivePivot,
        double thetaDeg);

    // Step 6: Evaluate Radial Direction Profile along a ray (phi in [0, 360))
    RadialDirectionProfile evaluateRadialDirectionProfile(
        const cv::Mat& gray,
        const GradientFeatureMap& features,
        const cv::Point2d& effectivePivot,
        double thetaDeg);

    DirectionProfile evaluateDirectionProfile(
        const cv::Mat& gray,
        const GradientFeatureMap& features,
        const cv::Point2d& effectivePivot,
        double thetaDeg);

    // Step 9: Evaluate TipLikelihood for forward (+u) vs backward (-u) directions
    double evaluateTipLikelihood(
        const std::vector<PointerCrossSection>& sections,
        double& outTaperScore,
        double& outConvergenceScore,
        double& outTerminalScore,
        double& outContinuityScore);

    // Step 12: Render full debug visualization (left/right edges, centerline, HUD)
    void renderDebugVisualization(
        cv::Mat& canvas,
        const cv::Point2d& effectivePivot,
        PointerDetectionResult& res);

    PointerDetectorConfig m_config;
    PointerShapeModel m_model;
    cv::Point2d m_pivotOffset{0.0, 0.0};
};

#endif // POINTER_DETECTOR_HPP
