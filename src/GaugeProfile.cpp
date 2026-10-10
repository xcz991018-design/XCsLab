#include "GaugeProfile.hpp"
#include <fstream>
#include <iostream>
#include <sstream>
#include <filesystem>

namespace fs = std::filesystem;

GaugeProfile GaugeProfile::createDefault() {
    GaugeProfile prof;
    prof.gauge_id = "PRESSURE_GAUGE_01";
    prof.unit = "MPa";
    prof.min_val = 0.0;
    prof.max_val = 1.0;
    prof.min_angle_deg = 135.0; // Lower left (~ 7:30 o'clock)
    prof.max_angle_deg = 45.0;  // Lower right (~ 4:30 o'clock)
    prof.clockwise = true;
    prof.pointer_invert_direction = true;
    prof.auto_detect_roi = false;
    prof.center = cv::Point2f(569.5f, 533.5f);
    prof.radius = 483.0f;
    prof.center_search_tolerance = 35.0f;
    prof.perspective_correction_enabled = false;
    return prof;
}

bool GaugeProfile::loadFromFile(const std::string& configFilePath, const std::string& targetCameraId) {
    std::string resolvedPath = configFilePath;
    std::ifstream file(resolvedPath);

    if (!file.is_open()) {
        std::vector<std::string> candidates = {
            configFilePath,
            "../" + configFilePath,
            "../../" + configFilePath,
            "../../../" + configFilePath,
            "config/" + fs::path(configFilePath).filename().string(),
            "../config/" + fs::path(configFilePath).filename().string(),
            "../../config/" + fs::path(configFilePath).filename().string()
        };

        for (const auto& cand : candidates) {
            std::ifstream testFile(cand);
            if (testFile.is_open()) {
                resolvedPath = cand;
                file = std::move(testFile);
                std::cout << "[GaugeProfile] Resolved config file location: " << resolvedPath << std::endl;
                break;
            }
        }
    }

    if (!file.is_open()) {
        std::cerr << "[GaugeProfile] Could not open config file: " << configFilePath 
                  << ". Using default profile." << std::endl;
        *this = createDefault();
        return false;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string content = buffer.str();
    file.close();

    std::string targetBlock = "";

    // 1. If targetCameraId is specified, try to find the specific JSON block containing targetCameraId
    if (!targetCameraId.empty()) {
        size_t idPos = content.find("\"" + targetCameraId + "\"");
        if (idPos != std::string::npos) {
            size_t blockStart = content.rfind('{', idPos);
            if (blockStart != std::string::npos) {
                int depth = 0;
                size_t blockEnd = std::string::npos;
                for (size_t i = blockStart; i < content.size(); ++i) {
                    if (content[i] == '{') depth++;
                    else if (content[i] == '}') {
                        depth--;
                        if (depth == 0) {
                            blockEnd = i;
                            break;
                        }
                    }
                }
                if (blockEnd != std::string::npos) {
                    targetBlock = content.substr(blockStart, blockEnd - blockStart + 1);
                }
            }
        }
    }

    // 2. Fallback: If no specific block found, check if content has array of "cameras" or "profiles"
    if (targetBlock.empty()) {
        size_t arrPos = content.find("\"cameras\"");
        if (arrPos == std::string::npos) arrPos = content.find("\"profiles\"");
        if (arrPos != std::string::npos) {
            size_t blockStart = content.find('{', arrPos);
            if (blockStart != std::string::npos) {
                int depth = 0;
                size_t blockEnd = std::string::npos;
                for (size_t i = blockStart; i < content.size(); ++i) {
                    if (content[i] == '{') depth++;
                    else if (content[i] == '}') {
                        depth--;
                        if (depth == 0) {
                            blockEnd = i;
                            break;
                        }
                    }
                }
                if (blockEnd != std::string::npos) {
                    targetBlock = content.substr(blockStart, blockEnd - blockStart + 1);
                }
            }
        }
    }

    // 3. Fallback: Parse whole content as a single object block
    if (targetBlock.empty()) {
        targetBlock = content;
    }

    // Parse root-level global "use_model" if present
    std::string globalModel = "";
    size_t modelPos = content.find("\"use_model\"");
    if (modelPos != std::string::npos) {
        size_t colon = content.find(':', modelPos);
        if (colon != std::string::npos) {
            size_t q1 = content.find('"', colon);
            size_t q2 = content.find('"', q1 + 1);
            if (q1 != std::string::npos && q2 != std::string::npos) {
                globalModel = content.substr(q1 + 1, q2 - q1 - 1);
            }
        }
    }

    // Parse targetBlock key-values line by line
    std::stringstream ss(targetBlock);
    std::string line;
    std::vector<float> ptsBuf(8, -1.0f);
    bool has_pts = false;
    std::string cameraModel = "";

    auto parseBlockKeyValues = [&](const std::string& blockStr) {
        std::stringstream ss(blockStr);
        std::string line;
        while (std::getline(ss, line)) {
            auto pos = line.find(':');
            if (pos == std::string::npos) pos = line.find('=');
            if (pos == std::string::npos) continue;

            std::string key = line.substr(0, pos);
            std::string val = line.substr(pos + 1);

            auto trim = [](std::string& s) {
                size_t first = s.find_first_not_of(" \t\r\n\"',");
                if (first == std::string::npos) { s.clear(); return; }
                size_t last = s.find_last_not_of(" \t\r\n\"',");
                s = s.substr(first, (last - first + 1));
            };

            trim(key); trim(val);

            if (key == "gauge_profile" || key == "use_model" || key == "profile_model" || key == "parent_model_path" || key == "default_profile_id") cameraModel = val;
            else if (key == "gauge_id" || key == "camera_id" || key == "model_id") gauge_id = val;
            else if (key == "unit") { unit = val; spec.unit = val; }
            else if (key == "min_val") { min_val = std::stod(val); spec.range_min = min_val; }
            else if (key == "max_val") { max_val = std::stod(val); spec.range_max = max_val; }
            else if (key == "min_angle_deg") min_angle_deg = std::stod(val);
            else if (key == "max_angle_deg") max_angle_deg = std::stod(val);
            else if (key == "clockwise") clockwise = (val == "true" || val == "1");
            else if (key == "pointer_invert_direction") pointer_invert_direction = (val == "true" || val == "1");
            // PointerDetector Parameters
            else if (key == "pointerBaseRadius" || key == "pointer_base_radius") pointerBaseRadius = std::stod(val);
            else if (key == "pointerMinWidth" || key == "pointer_min_width" || key == "body_min_width") pointerMinWidth = std::stod(val);
            else if (key == "pointerMaxWidth" || key == "pointer_max_width" || key == "body_max_width") pointerMaxWidth = std::stod(val);
            else if (key == "pointerMinLength" || key == "pointer_min_length") pointerMinLength = std::stod(val);
            else if (key == "coarseAngleStep" || key == "coarse_angle_step") coarseAngleStep = std::stod(val);
            else if (key == "fineAngleStep" || key == "fine_angle_step") fineAngleStep = std::stod(val);
            else if (key == "fineAngleRange" || key == "fine_angle_range") fineAngleRange = std::stod(val);
            else if (key == "minValidSectionRatio" || key == "min_valid_section_ratio") minValidSectionRatio = std::stod(val);
            else if (key == "maxAllowedWidthStep" || key == "max_allowed_width_step") maxAllowedWidthStep = std::stod(val);
            else if (key == "maxCenterlineOffset" || key == "max_centerline_offset") maxCenterlineOffset = std::stod(val);
            // Anti-Interference / Occlusion Bridging Parameters (Tolerate text & scale marks)
            else if (key == "max_consecutive_gap_samples" || key == "maxConsecutiveGapSamples") maxConsecutiveGapSamples = std::stoi(val);
            else if (key == "min_edge_support_density" || key == "minEdgeSupportDensity") minEdgeSupportDensity = std::stod(val);
            else if (key == "raw_gradient_threshold" || key == "rawGradientThreshold") rawGradientThreshold = std::stod(val);
            else if (key == "min_gradient_normal_alignment" || key == "minGradientNormalAlignment") minGradientNormalAlignment = std::stod(val);
            else if (key == "min_local_step_contrast" || key == "minLocalStepContrast") minLocalStepContrast = std::stod(val);
            else if (key == "max_start_base_gap_px" || key == "maxStartBaseGapPx") maxStartBaseGapPx = std::stod(val);
            else if (key == "max_parallel_angle_diff_deg" || key == "maxParallelAngleDiffDeg") maxParallelAngleDiffDeg = std::stod(val);
            else if (key == "min_common_length_px" || key == "minCommonLengthPx") minCommonLengthPx = std::stod(val);
            else if (key == "max_line_fit_residual_px" || key == "maxLineFitResidualPx") maxLineFitResidualPx = std::stod(val);
            else if (key == "max_occlusion_gap" || key == "maxOcclusionGap") maxOcclusionGap = std::stoi(val);
            else if (key == "corridor_half_width_px" || key == "corridorHalfWidthPx") corridorHalfWidthPx = std::stod(val);
            // Pointer Tail (Counterweight) Detection Parameters
            else if (key == "enable_tail_detection" || key == "enableTailDetection") enable_tail_detection = (val == "true" || val == "1");
            else if (key == "tail_max_radius" || key == "tailMaxRadius") tail_max_radius = std::stod(val);
            else if (key == "tail_min_width" || key == "tailMinWidth") tail_min_width = std::stod(val);
            else if (key == "tail_max_width" || key == "tailMaxWidth") tail_max_width = std::stod(val);
            else if (key == "tail_corridor_half_width" || key == "tailCorridorHalfWidth") tail_corridor_half_width = std::stod(val);
            else if (key == "tail_min_gradient_normal_alignment" || key == "tailMinGradientNormalAlignment") tail_min_gradient_normal_alignment = std::stod(val);
            // Optical Polarity & Dark Entity Constraints
            else if (key == "require_dark_pointer" || key == "requireDarkPointer") require_dark_pointer = (val == "true" || val == "1");
            else if (key == "min_pointer_dark_contrast" || key == "minPointerDarkContrast") min_pointer_dark_contrast = std::stod(val);
            else if (key == "max_pointer_inside_gray" || key == "maxPointerInsideGray") max_pointer_inside_gray = std::stod(val);
            // Pointer Structure & Confidence Scoring Weights
            else if (key == "geom_weight" || key == "w_geom_weight" || key == "geomWeight") w_geom_weight = std::stod(val);
            else if (key == "edge_pair_coherence" || key == "w_edge_pair_coherence" || key == "edgePairCoherence" || key == "wEdgePairCoherence") w_edge_pair_coherence = std::stod(val);
            else if (key == "center_stability" || key == "w_center_stability" || key == "centerStability" || key == "wCenterStability") w_center_stability = std::stod(val);
            else if (key == "axis_center" || key == "w_axis_center" || key == "axisCenter" || key == "wAxisCenter") w_axis_center = std::stod(val);
            else if (key == "orientation" || key == "w_orientation" || key == "orientationConsistency" || key == "wOrientation") w_orientation = std::stod(val);
            else if (key == "reach" || key == "w_reach" || key == "reachWeight" || key == "wReach") w_reach = std::stod(val);
            else if (key == "valid_length" || key == "w_valid_length" || key == "validLengthWeight" || key == "wValidLength") w_valid_length = std::stod(val);
            else if (key == "slenderness" || key == "w_slenderness" || key == "slendernessWeight" || key == "wSlenderness") w_slenderness = std::stod(val);
            else if (key == "persistence" || key == "w_persistence" || key == "persistenceWeight" || key == "wPersistence") w_persistence = std::stod(val);
            else if (key == "width_trend" || key == "w_width_trend" || key == "widthTrendWeight" || key == "wWidthTrend") w_width_trend = std::stod(val);
            else if (key == "width_continuity" || key == "w_width_continuity" || key == "widthContinuityWeight" || key == "wWidthContinuity") w_width_continuity = std::stod(val);
            else if (key == "edge_strength" || key == "w_edge_strength" || key == "edgeStrengthWeight" || key == "wEdgeStrength") w_edge_strength = std::stod(val);
            else if (key == "gap_penalty" || key == "w_gap_penalty" || key == "gapPenalty" || key == "wGapPenalty") w_gap_penalty = std::stod(val);
            else if (key == "tracking_jump_penalty" || key == "w_tracking_jump_penalty" || key == "trackingJumpPenalty" || key == "wTrackingJumpPenalty") w_tracking_jump_penalty = std::stod(val);
            else if (key == "tail_score" || key == "w_tail_score" || key == "tailScoreWeight" || key == "wTailScore") w_tail_score = std::stod(val);
            else if (key == "tail_polarity_boost" || key == "w_tail_polarity_boost" || key == "tailPolarityBoost" || key == "wTailPolarityBoost") w_tail_polarity_boost = std::stod(val);
            else if (key == "confidence_divisor" || key == "confidenceDivisor") confidence_divisor = std::stod(val);
            // GIS Parameters
            else if (key == "longitude" || key == "gis_lng") gis.longitude = std::stod(val);
            else if (key == "latitude" || key == "gis_lat") gis.latitude = std::stod(val);
            else if (key == "altitude" || key == "gis_alt") gis.altitude = std::stod(val);
            // Location Info Parameters
            else if (key == "substation" || key == "plant_area") location.substation = val;
            else if (key == "area" || key == "room_area") location.area = val;
            else if (key == "device_name" || key == "installation_location") location.device_name = val;
            else if (key == "device_tag" || key == "tag") location.device_tag = val;
            // Gauge Head Hardware Specification Parameters
            else if (key == "gauge_type") spec.gauge_type = val;
            else if (key == "manufacturer") spec.manufacturer = val;
            else if (key == "model_name") spec.model_name = val;
            else if (key == "accuracy_class") spec.accuracy_class = val;
            else if (key == "center_x") center.x = std::stof(val);
            else if (key == "center_y") center.y = std::stof(val);
            else if (key == "radius") radius = std::stof(val);
            else if (key == "center_search_tolerance") center_search_tolerance = std::stof(val);
            // Image Preprocessing & Anti-Glare Parameters
            else if (key == "clahe_clip_limit") clahe_clip_limit = std::stod(val);
            else if (key == "clahe_grid_size") clahe_grid_size = std::stoi(val);
            else if (key == "gaussian_blur_size") gaussian_blur_size = std::stoi(val);
            else if (key == "gaussian_blur_sigma") gaussian_blur_sigma = std::stod(val);
            else if (key == "blackhat_kernel_size") blackhat_kernel_size = std::stoi(val);
            else if (key == "blackhat_threshold") blackhat_threshold = std::stod(val);
            else if (key == "local_bg_kernel_size") local_bg_kernel_size = std::stoi(val);
            else if (key == "adapt_diff_threshold") adapt_diff_threshold = std::stod(val);
            else if (key == "grad_mag_threshold") grad_mag_threshold = std::stod(val);
            else if (key == "morph_close_size") morph_close_size = std::stoi(val);
            else if (key == "enable_steps_4_to_9") enable_steps_4_to_9 = (val == "true" || val == "1");
            else if (key == "save_debug_images") save_debug_images = (val == "true" || val == "1");
            else if (key == "auto_detect_roi") auto_detect_roi = (val == "true" || val == "1");
            else if (key == "perspective_correction_enabled") perspective_correction_enabled = (val == "true" || val == "1");
            else if (key == "calibrated_width") calibrated_width = std::stoi(val);
            else if (key == "calibrated_height") calibrated_height = std::stoi(val);
            else if (key == "pt_0_x") { ptsBuf[0] = std::stof(val); has_pts = true; }
            else if (key == "pt_0_y") { ptsBuf[1] = std::stof(val); has_pts = true; }
            else if (key == "pt_1_x") { ptsBuf[2] = std::stof(val); has_pts = true; }
            else if (key == "pt_1_y") { ptsBuf[3] = std::stof(val); has_pts = true; }
            else if (key == "pt_2_x") { ptsBuf[4] = std::stof(val); has_pts = true; }
            else if (key == "pt_2_y") { ptsBuf[5] = std::stof(val); has_pts = true; }
            else if (key == "pt_3_x") { ptsBuf[6] = std::stof(val); has_pts = true; }
            else if (key == "pt_3_y") { ptsBuf[7] = std::stof(val); has_pts = true; }
        }
    };

    // 1. Parse root-level global settings (e.g. pointer_detector_global_config) first
    if (!content.empty() && !targetBlock.empty() && targetBlock != content) {
        parseBlockKeyValues(content);
    }
    // 2. Parse target camera block to override global settings if specified
    parseBlockKeyValues(targetBlock);

    if (has_pts && ptsBuf[0] >= 0.0f) {
        perspective_pts.clear();
        for (size_t i = 0; i < 8; i += 2) {
            perspective_pts.push_back(cv::Point2f(ptsBuf[i], ptsBuf[i + 1]));
        }
    }

    // Resolve parent model inheritance if specified
    std::string targetModel = !cameraModel.empty() ? cameraModel : globalModel;
    if (!targetModel.empty() && targetModel != targetCameraId) {
        std::string parentPath = targetModel;
        std::string parentSubId = "";

        auto hashPos = targetModel.find('#');
        if (hashPos != std::string::npos) {
            parentPath = targetModel.substr(0, hashPos);
            parentSubId = targetModel.substr(hashPos + 1);
        } else if (targetModel.find(".json") == std::string::npos) {
            parentPath = !globalModel.empty() ? globalModel : "gauge_profile.json";
            parentSubId = targetModel;
        }

        if (fs::path(parentPath).is_relative()) {
            fs::path baseDir = fs::path(resolvedPath).parent_path();
            parentPath = (baseDir / parentPath).string();
        }

        GaugeProfile parentProfile;
        if (parentProfile.loadFromFile(parentPath, parentSubId)) {
            std::cout << "[GaugeProfile] Inheriting parameters from model template: " << parentPath 
                      << (parentSubId.empty() ? "" : (" [" + parentSubId + "]")) << std::endl;
            this->gauge_id = parentProfile.gauge_id;
            this->unit = parentProfile.unit;
            this->min_val = parentProfile.min_val;
            this->max_val = parentProfile.max_val;
            this->min_angle_deg = parentProfile.min_angle_deg;
            this->max_angle_deg = parentProfile.max_angle_deg;
            this->clockwise = parentProfile.clockwise;
            this->pointer_invert_direction = parentProfile.pointer_invert_direction;
            this->pointerBaseRadius = parentProfile.pointerBaseRadius;
            this->pointerMinWidth = parentProfile.pointerMinWidth;
            this->pointerMaxWidth = parentProfile.pointerMaxWidth;
            this->pointerMinLength = parentProfile.pointerMinLength;
            this->coarseAngleStep = parentProfile.coarseAngleStep;
            this->fineAngleStep = parentProfile.fineAngleStep;
            this->fineAngleRange = parentProfile.fineAngleRange;
            this->minValidSectionRatio = parentProfile.minValidSectionRatio;
            this->maxAllowedWidthStep = parentProfile.maxAllowedWidthStep;
            this->maxCenterlineOffset = parentProfile.maxCenterlineOffset;
            this->maxConsecutiveGapSamples = parentProfile.maxConsecutiveGapSamples;
            this->minEdgeSupportDensity = parentProfile.minEdgeSupportDensity;
            this->rawGradientThreshold = parentProfile.rawGradientThreshold;
            this->minGradientNormalAlignment = parentProfile.minGradientNormalAlignment;
            this->minLocalStepContrast = parentProfile.minLocalStepContrast;
            this->maxStartBaseGapPx = parentProfile.maxStartBaseGapPx;
            this->maxParallelAngleDiffDeg = parentProfile.maxParallelAngleDiffDeg;
            this->minCommonLengthPx = parentProfile.minCommonLengthPx;
            this->maxLineFitResidualPx = parentProfile.maxLineFitResidualPx;
            this->maxOcclusionGap = parentProfile.maxOcclusionGap;
            this->corridorHalfWidthPx = parentProfile.corridorHalfWidthPx;
            this->enable_tail_detection = parentProfile.enable_tail_detection;
            this->tail_max_radius = parentProfile.tail_max_radius;
            this->tail_min_width = parentProfile.tail_min_width;
            this->tail_max_width = parentProfile.tail_max_width;
            this->tail_corridor_half_width = parentProfile.tail_corridor_half_width;
            this->tail_min_gradient_normal_alignment = parentProfile.tail_min_gradient_normal_alignment;
            this->require_dark_pointer = parentProfile.require_dark_pointer;
            this->min_pointer_dark_contrast = parentProfile.min_pointer_dark_contrast;
            this->max_pointer_inside_gray = parentProfile.max_pointer_inside_gray;
            this->clahe_clip_limit = parentProfile.clahe_clip_limit;
            this->clahe_grid_size = parentProfile.clahe_grid_size;
            this->gaussian_blur_size = parentProfile.gaussian_blur_size;
            this->gaussian_blur_sigma = parentProfile.gaussian_blur_sigma;
            this->blackhat_kernel_size = parentProfile.blackhat_kernel_size;
            this->blackhat_threshold = parentProfile.blackhat_threshold;
            this->local_bg_kernel_size = parentProfile.local_bg_kernel_size;
            this->adapt_diff_threshold = parentProfile.adapt_diff_threshold;
            this->grad_mag_threshold = parentProfile.grad_mag_threshold;
            this->morph_close_size = parentProfile.morph_close_size;
            this->w_geom_weight = parentProfile.w_geom_weight;
            this->w_edge_pair_coherence = parentProfile.w_edge_pair_coherence;
            this->w_center_stability = parentProfile.w_center_stability;
            this->w_axis_center = parentProfile.w_axis_center;
            this->w_orientation = parentProfile.w_orientation;
            this->w_reach = parentProfile.w_reach;
            this->w_valid_length = parentProfile.w_valid_length;
            this->w_slenderness = parentProfile.w_slenderness;
            this->w_persistence = parentProfile.w_persistence;
            this->w_width_trend = parentProfile.w_width_trend;
            this->w_width_continuity = parentProfile.w_width_continuity;
            this->w_edge_strength = parentProfile.w_edge_strength;
            this->w_gap_penalty = parentProfile.w_gap_penalty;
            this->w_tracking_jump_penalty = parentProfile.w_tracking_jump_penalty;
            this->w_tail_score = parentProfile.w_tail_score;
            this->w_tail_polarity_boost = parentProfile.w_tail_polarity_boost;
            this->confidence_divisor = parentProfile.confidence_divisor;
            this->parent_model_path = parentPath;
        }
    }

    std::cout << "[GaugeProfile] Successfully loaded profile [" << gauge_id 
              << "] (" << min_val << " ~ " << max_val << " " << unit << ") from " << resolvedPath << std::endl;
    return true;
}

bool GaugeProfile::saveToFile(const std::string& configFilePath, const std::string& targetCameraId) const {
    std::string resolvedPath = configFilePath;
    std::ifstream testFile(resolvedPath);
    if (!testFile.is_open()) {
        std::vector<std::string> candidates = {
            configFilePath,
            "../" + configFilePath,
            "../../" + configFilePath,
            "config/" + fs::path(configFilePath).filename().string(),
            "../config/" + fs::path(configFilePath).filename().string()
        };
        for (const auto& cand : candidates) {
            std::ifstream tf(cand);
            if (tf.is_open()) {
                resolvedPath = cand;
                break;
            }
        }
    }

    std::ifstream in(resolvedPath);
    if (!in.is_open()) return false;

    std::stringstream buffer;
    buffer << in.rdbuf();
    std::string content = buffer.str();
    in.close();

    std::string searchId = !targetCameraId.empty() ? targetCameraId : gauge_id;

    // Check if content is a multi-camera JSON registry containing "cameras"
    if (content.find("\"cameras\"") != std::string::npos && !searchId.empty()) {
        size_t idPos = content.find("\"" + searchId + "\"");
        if (idPos != std::string::npos) {
            size_t blockStart = content.rfind('{', idPos);
            size_t blockEnd = std::string::npos;
            if (blockStart != std::string::npos) {
                int depth = 0;
                for (size_t i = blockStart; i < content.size(); ++i) {
                    if (content[i] == '{') depth++;
                    else if (content[i] == '}') {
                        depth--;
                        if (depth == 0) { blockEnd = i; break; }
                    }
                }
            }

            if (blockStart != std::string::npos && blockEnd != std::string::npos) {
                std::string block = content.substr(blockStart, blockEnd - blockStart + 1);
                auto updateBlock = [&](std::string& blk) {
                    auto updateOrInsertKey = [&](const std::string& key, float val) {
                        size_t kpos = blk.find("\"" + key + "\"");
                        if (kpos != std::string::npos) {
                            size_t colon = blk.find(':', kpos);
                            size_t comma = blk.find_first_of(",\r\n}", colon);
                            if (colon != std::string::npos && comma != std::string::npos) {
                                char buf[64];
                                snprintf(buf, sizeof(buf), " %.4g", val);
                                blk.replace(colon + 1, comma - colon - 1, buf);
                            }
                        } else {
                            size_t closeBrace = blk.rfind('}');
                            if (closeBrace != std::string::npos) {
                                char buf[128];
                                snprintf(buf, sizeof(buf), ",\n  \"%s\": %.4g\n", key.c_str(), val);
                                blk.insert(closeBrace, buf);
                            }
                        }
                    };

                    auto updateOrInsertBoolKey = [&](const std::string& key, bool val) {
                        size_t kpos = blk.find("\"" + key + "\"");
                        if (kpos != std::string::npos) {
                            size_t colon = blk.find(':', kpos);
                            size_t comma = blk.find_first_of(",\r\n}", colon);
                            if (colon != std::string::npos && comma != std::string::npos) {
                                std::string strVal = val ? " true" : " false";
                                blk.replace(colon + 1, comma - colon - 1, strVal);
                            }
                        } else {
                            size_t closeBrace = blk.rfind('}');
                            if (closeBrace != std::string::npos) {
                                char buf[128];
                                snprintf(buf, sizeof(buf), ",\n  \"%s\": %s\n", key.c_str(), val ? "true" : "false");
                                blk.insert(closeBrace, buf);
                            }
                        }
                    };

                    if (perspective_pts.size() == 4) {
                        updateOrInsertKey("pt_0_x", perspective_pts[0].x);
                        updateOrInsertKey("pt_0_y", perspective_pts[0].y);
                        updateOrInsertKey("pt_1_x", perspective_pts[1].x);
                        updateOrInsertKey("pt_1_y", perspective_pts[1].y);
                        updateOrInsertKey("pt_2_x", perspective_pts[2].x);
                        updateOrInsertKey("pt_2_y", perspective_pts[2].y);
                        updateOrInsertKey("pt_3_x", perspective_pts[3].x);
                        updateOrInsertKey("pt_3_y", perspective_pts[3].y);
                    }

                    if (calibrated_width > 0 && calibrated_height > 0) {
                        updateOrInsertKey("calibrated_width", static_cast<float>(calibrated_width));
                        updateOrInsertKey("calibrated_height", static_cast<float>(calibrated_height));
                    }

                    if (center.x > 0 && center.y > 0) {
                        updateOrInsertKey("center_x", center.x);
                        updateOrInsertKey("center_y", center.y);
                    }
                    if (pointerBaseRadius > 0) {
                        updateOrInsertKey("pointerBaseRadius", static_cast<float>(pointerBaseRadius));
                    }
                    updateOrInsertBoolKey("auto_detect_roi", auto_detect_roi);
                    updateOrInsertBoolKey("perspective_correction_enabled", perspective_correction_enabled);
                };

                updateBlock(block);
                content.replace(blockStart, blockEnd - blockStart + 1, block);

                std::ofstream out(resolvedPath);
                if (!out.is_open()) return false;
                out << content;
                out.close();
                std::cout << "[GaugeProfile] Updated cardinal points & center for camera " << searchId << " in " << resolvedPath << std::endl;
                return true;
            }
        }
    } else if (content.find('{') != std::string::npos) {
        // Single-camera JSON: in-place update of calibration keys to preserve all other custom parameters
        auto updateOrInsertKey = [&](const std::string& key, float val) {
            size_t kpos = content.find("\"" + key + "\"");
            if (kpos != std::string::npos) {
                size_t colon = content.find(':', kpos);
                size_t comma = content.find_first_of(",\r\n}", colon);
                if (colon != std::string::npos && comma != std::string::npos) {
                    char buf[64];
                    snprintf(buf, sizeof(buf), " %.4g", val);
                    content.replace(colon + 1, comma - colon - 1, buf);
                }
            } else {
                size_t closeBrace = content.rfind('}');
                if (closeBrace != std::string::npos) {
                    char buf[128];
                    snprintf(buf, sizeof(buf), ",\n  \"%s\": %.4g\n", key.c_str(), val);
                    content.insert(closeBrace, buf);
                }
            }
        };

        auto updateOrInsertBoolKey = [&](const std::string& key, bool val) {
            size_t kpos = content.find("\"" + key + "\"");
            if (kpos != std::string::npos) {
                size_t colon = content.find(':', kpos);
                size_t comma = content.find_first_of(",\r\n}", colon);
                if (colon != std::string::npos && comma != std::string::npos) {
                    std::string strVal = val ? " true" : " false";
                    content.replace(colon + 1, comma - colon - 1, strVal);
                }
            } else {
                size_t closeBrace = content.rfind('}');
                if (closeBrace != std::string::npos) {
                    char buf[128];
                    snprintf(buf, sizeof(buf), ",\n  \"%s\": %s\n", key.c_str(), val ? "true" : "false");
                    content.insert(closeBrace, buf);
                }
            }
        };

        if (perspective_pts.size() == 4) {
            updateOrInsertKey("pt_0_x", perspective_pts[0].x);
            updateOrInsertKey("pt_0_y", perspective_pts[0].y);
            updateOrInsertKey("pt_1_x", perspective_pts[1].x);
            updateOrInsertKey("pt_1_y", perspective_pts[1].y);
            updateOrInsertKey("pt_2_x", perspective_pts[2].x);
            updateOrInsertKey("pt_2_y", perspective_pts[2].y);
            updateOrInsertKey("pt_3_x", perspective_pts[3].x);
            updateOrInsertKey("pt_3_y", perspective_pts[3].y);
        }

        if (calibrated_width > 0 && calibrated_height > 0) {
            updateOrInsertKey("calibrated_width", static_cast<float>(calibrated_width));
            updateOrInsertKey("calibrated_height", static_cast<float>(calibrated_height));
        }

        if (center.x > 0 && center.y > 0) {
            updateOrInsertKey("center_x", center.x);
            updateOrInsertKey("center_y", center.y);
        }
        if (pointerBaseRadius > 0) {
            updateOrInsertKey("pointerBaseRadius", static_cast<float>(pointerBaseRadius));
        }
        updateOrInsertBoolKey("auto_detect_roi", auto_detect_roi);
        updateOrInsertBoolKey("perspective_correction_enabled", perspective_correction_enabled);

        std::ofstream out(resolvedPath);
        if (!out.is_open()) return false;
        out << content;
        out.close();
        std::cout << "[GaugeProfile] Updated cardinal points & center in single-camera config " << resolvedPath << std::endl;
        return true;
    }

    // Standard single-file JSON output
    std::ofstream file(resolvedPath);
    if (!file.is_open()) return false;

    file << "{\n";
    file << "  \"gauge_id\": \"" << gauge_id << "\",\n";
    if (!parent_model_path.empty()) {
        file << "  \"use_model\": \"" << parent_model_path << "\",\n";
    }
    file << "  \"unit\": \"" << unit << "\",\n";
    file << "  \"min_val\": " << min_val << ",\n";
    file << "  \"max_val\": " << max_val << ",\n";
    file << "  \"min_angle_deg\": " << min_angle_deg << ",\n";
    file << "  \"max_angle_deg\": " << max_angle_deg << ",\n";
    file << "  \"clockwise\": " << (clockwise ? "true" : "false") << ",\n";
    file << "  \"pointer_invert_direction\": " << (pointer_invert_direction ? "true" : "false") << ",\n";
    file << "  \"auto_detect_roi\": " << (auto_detect_roi ? "true" : "false") << ",\n";
    file << "  \"center_x\": " << center.x << ",\n";
    file << "  \"center_y\": " << center.y << ",\n";
    file << "  \"radius\": " << radius << ",\n";
    file << "  \"pointerBaseRadius\": " << pointerBaseRadius << ",\n";
    file << "  \"pointer_min_width\": " << pointerMinWidth << ",\n";
    file << "  \"pointer_max_width\": " << pointerMaxWidth << ",\n";
    file << "  \"center_search_tolerance\": " << center_search_tolerance << ",\n";
    file << "  \"max_consecutive_gap_samples\": " << maxConsecutiveGapSamples << ",\n";
    file << "  \"min_edge_support_density\": " << minEdgeSupportDensity << ",\n";
    file << "  \"raw_gradient_threshold\": " << rawGradientThreshold << ",\n";
    file << "  \"min_gradient_normal_alignment\": " << minGradientNormalAlignment << ",\n";
    file << "  \"min_local_step_contrast\": " << minLocalStepContrast << ",\n";
    file << "  \"max_start_base_gap_px\": " << maxStartBaseGapPx << ",\n";
    file << "  \"max_parallel_angle_diff_deg\": " << maxParallelAngleDiffDeg << ",\n";
    file << "  \"min_common_length_px\": " << minCommonLengthPx << ",\n";
    file << "  \"max_line_fit_residual_px\": " << maxLineFitResidualPx << ",\n";
    file << "  \"max_occlusion_gap\": " << maxOcclusionGap << ",\n";
    file << "  \"corridor_half_width_px\": " << corridorHalfWidthPx << ",\n";
    file << "  \"enable_tail_detection\": " << (enable_tail_detection ? "true" : "false") << ",\n";
    file << "  \"tail_max_radius\": " << tail_max_radius << ",\n";
    file << "  \"tail_min_width\": " << tail_min_width << ",\n";
    file << "  \"tail_max_width\": " << tail_max_width << ",\n";
    file << "  \"tail_corridor_half_width\": " << tail_corridor_half_width << ",\n";
    file << "  \"tail_min_gradient_normal_alignment\": " << tail_min_gradient_normal_alignment << ",\n";
    file << "  \"require_dark_pointer\": " << (require_dark_pointer ? "true" : "false") << ",\n";
    file << "  \"min_pointer_dark_contrast\": " << min_pointer_dark_contrast << ",\n";
    file << "  \"max_pointer_inside_gray\": " << max_pointer_inside_gray << ",\n";
    file << "  \"score_weights\": {\n";
    file << "    \"geom_weight\": " << w_geom_weight << ",\n";
    file << "    \"edge_pair_coherence\": " << w_edge_pair_coherence << ",\n";
    file << "    \"center_stability\": " << w_center_stability << ",\n";
    file << "    \"axis_center\": " << w_axis_center << ",\n";
    file << "    \"orientation\": " << w_orientation << ",\n";
    file << "    \"reach\": " << w_reach << ",\n";
    file << "    \"valid_length\": " << w_valid_length << ",\n";
    file << "    \"slenderness\": " << w_slenderness << ",\n";
    file << "    \"persistence\": " << w_persistence << ",\n";
    file << "    \"width_trend\": " << w_width_trend << ",\n";
    file << "    \"width_continuity\": " << w_width_continuity << ",\n";
    file << "    \"edge_strength\": " << w_edge_strength << ",\n";
    file << "    \"gap_penalty\": " << w_gap_penalty << ",\n";
    file << "    \"tracking_jump_penalty\": " << w_tracking_jump_penalty << ",\n";
    file << "    \"tail_score\": " << w_tail_score << ",\n";
    file << "    \"tail_polarity_boost\": " << w_tail_polarity_boost << ",\n";
    file << "    \"confidence_divisor\": " << confidence_divisor << "\n";
    file << "  },\n";
    file << "  \"perspective_correction_enabled\": " << (perspective_correction_enabled ? "true" : "false") << ",\n";
    file << "  \"calibrated_width\": " << calibrated_width << ",\n";
    file << "  \"calibrated_height\": " << calibrated_height << ",\n";
    file << "  \"clahe_clip_limit\": " << clahe_clip_limit << ",\n";
    file << "  \"clahe_grid_size\": " << clahe_grid_size << ",\n";
    file << "  \"gaussian_blur_size\": " << gaussian_blur_size << ",\n";
    file << "  \"gaussian_blur_sigma\": " << gaussian_blur_sigma << ",\n";
    file << "  \"blackhat_kernel_size\": " << blackhat_kernel_size << ",\n";
    file << "  \"blackhat_threshold\": " << blackhat_threshold << ",\n";
    file << "  \"local_bg_kernel_size\": " << local_bg_kernel_size << ",\n";
    file << "  \"adapt_diff_threshold\": " << adapt_diff_threshold << ",\n";
    file << "  \"grad_mag_threshold\": " << grad_mag_threshold << ",\n";
    file << "  \"morph_close_size\": " << morph_close_size;
    
    if (perspective_pts.size() == 4) {
        file << ",\n";
        for (size_t i = 0; i < 4; ++i) {
            file << "  \"pt_" << i << "_x\": " << perspective_pts[i].x << ",\n";
            file << "  \"pt_" << i << "_y\": " << perspective_pts[i].y << (i == 3 ? "" : ",") << "\n";
        }
    } else {
        file << "\n";
    }
    file << "}\n";

    return true;
}
