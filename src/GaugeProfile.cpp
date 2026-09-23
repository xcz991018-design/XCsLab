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

            if (key == "gauge_profile" || key == "use_model" || key == "profile_model" || key == "parent_model_path") cameraModel = val;
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
            else if (key == "pointerMinWidth" || key == "pointer_min_width") pointerMinWidth = std::stod(val);
            else if (key == "pointerMaxWidth" || key == "pointer_max_width") pointerMaxWidth = std::stod(val);
            else if (key == "pointerMinLength" || key == "pointer_min_length") pointerMinLength = std::stod(val);
            else if (key == "coarseAngleStep" || key == "coarse_angle_step") coarseAngleStep = std::stod(val);
            else if (key == "fineAngleStep" || key == "fine_angle_step") fineAngleStep = std::stod(val);
            else if (key == "fineAngleRange" || key == "fine_angle_range") fineAngleRange = std::stod(val);
            else if (key == "minValidSectionRatio" || key == "min_valid_section_ratio") minValidSectionRatio = std::stod(val);
            else if (key == "maxAllowedWidthStep" || key == "max_allowed_width_step") maxAllowedWidthStep = std::stod(val);
            else if (key == "maxCenterlineOffset" || key == "max_centerline_offset") maxCenterlineOffset = std::stod(val);
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

                auto updateOrInsertKey = [&](const std::string& key, float val) {
                    size_t kpos = block.find("\"" + key + "\"");
                    if (kpos != std::string::npos) {
                        size_t colon = block.find(':', kpos);
                        size_t comma = block.find_first_of(",\r\n}", colon);
                        if (colon != std::string::npos && comma != std::string::npos) {
                            char buf[64];
                            snprintf(buf, sizeof(buf), " %.1f", val);
                            block.replace(colon + 1, comma - colon - 1, buf);
                        }
                    } else {
                        size_t closeBrace = block.rfind('}');
                        if (closeBrace != std::string::npos) {
                            char buf[128];
                            snprintf(buf, sizeof(buf), ",\n      \"%s\": %.1f\n    ", key.c_str(), val);
                            block.insert(closeBrace, buf);
                        }
                    }
                };

                auto updateOrInsertBoolKey = [&](const std::string& key, bool val) {
                    size_t kpos = block.find("\"" + key + "\"");
                    if (kpos != std::string::npos) {
                        size_t colon = block.find(':', kpos);
                        size_t comma = block.find_first_of(",\r\n}", colon);
                        if (colon != std::string::npos && comma != std::string::npos) {
                            std::string strVal = val ? " true" : " false";
                            block.replace(colon + 1, comma - colon - 1, strVal);
                        }
                    } else {
                        size_t closeBrace = block.rfind('}');
                        if (closeBrace != std::string::npos) {
                            char buf[128];
                            snprintf(buf, sizeof(buf), ",\n      \"%s\": %s\n    ", key.c_str(), val ? "true" : "false");
                            block.insert(closeBrace, buf);
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

                content.replace(blockStart, blockEnd - blockStart + 1, block);

                std::ofstream out(resolvedPath);
                if (!out.is_open()) return false;
                out << content;
                out.close();
                std::cout << "[GaugeProfile] Updated cardinal points & center for camera " << searchId << " in " << resolvedPath << std::endl;
                return true;
            }
        }
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
    file << "  \"center_search_tolerance\": " << center_search_tolerance << ",\n";
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
