#include "FileCameraSource.hpp"
#include "SDKCameraSourceStub.hpp"
#include "GaugeProfile.hpp"
#include "GaugeReaderEngine.hpp"

#include <iostream>
#include <memory>
#include <string>
#include <chrono>
#include <filesystem>

void printBanner() {
    std::cout << "=====================================================\n";
    std::cout << "  C++ Gauge Reader System (GaugeReaderTest v1.0.0)   \n";
    std::cout << "  Industrial Multi-Camera Abstraction & Polar Engine \n";
    std::cout << "=====================================================\n\n";
}

void printHelp(const char* progName) {
    std::cout << "Usage: " << progName << " [options]\n\n"
              << "Options:\n"
              << "  --image <path>       Path to gauge input image or directory\n"
              << "  --config <path>      Path to centralized devices.json or gauge_profile.json\n"
              << "  --camera-id <id>     Target Camera ID inside central devices.json registry\n"
              << "  --output <path>      Path to save overlay result image (default: result.png)\n"
              << "  --sdk-stub           Run in reserved Camera SDK Stub mode\n"
              << "  --help               Display this help message\n\n";
}

int main(int argc, char** argv) {
    printBanner();

    std::string imagePath = "";
    std::string configPath = "config/devices.json";
    try {
        std::filesystem::path exeDir = std::filesystem::absolute(argv[0]).parent_path();
        std::filesystem::path localConfig = exeDir / "config" / "devices.json";
        if (std::filesystem::exists(localConfig)) {
            configPath = localConfig.string();
        } else if (std::filesystem::exists("config/devices.json")) {
            configPath = "config/devices.json";
        } else if (std::filesystem::exists("../config/devices.json")) {
            configPath = "../config/devices.json";
        } else if (std::filesystem::exists("config/gauge_profile.json")) {
            configPath = "config/gauge_profile.json";
        }
    } catch (...) {
        // Fallback to relative default
    }
    std::string cameraId = "CAM_5-3-1";
    std::string outputPath = "result.png";
    bool runSdkStub = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--image" && i + 1 < argc) {
            imagePath = argv[++i];
        } else if (arg == "--config" && i + 1 < argc) {
            configPath = argv[++i];
        } else if (arg == "--camera-id" && i + 1 < argc) {
            cameraId = argv[++i];
        } else if (arg == "--output" && i + 1 < argc) {
            outputPath = argv[++i];
        } else if (arg == "--sdk-stub") {
            runSdkStub = true;
        } else if (arg == "--help" || arg == "-h") {
            printHelp(argv[0]);
            return 0;
        }
    }

    // Load Calibration Profile from Central Registry or Single File
    GaugeProfile profile;
    if (!profile.loadFromFile(configPath, cameraId)) {
        std::cout << "[Main] Using default calibration profile." << std::endl;
    }

    // Select Camera Source (Abstraction Layer)
    CameraSourcePtr cameraSource;

    if (runSdkStub) {
        std::cout << "[Main] Initializing Industrial Camera SDK Stub..." << std::endl;
        cameraSource = std::make_shared<SDKCameraSourceStub>("CAM_SN_8839210", 0);
    } else {
        if (imagePath.empty()) {
            imagePath = "test_images/222.png"; // Default test image (large perspective tilt)
        }
        std::cout << "[Main] Initializing File Camera Source from: " << imagePath << std::endl;
        cameraSource = std::make_shared<FileCameraSource>(imagePath);
    }

    if (!cameraSource->open()) {
        std::cerr << "[Main] Error: Could not open camera source: " << cameraSource->getSourceName() << std::endl;
        std::cerr << "Hint: Provide a valid image via '--image <path_to_image>'" << std::endl;
        return -1;
    }

    cv::Mat frame;
    if (!cameraSource->grabFrame(frame)) {
        std::cerr << "[Main] Error: Failed to acquire frame from " << cameraSource->getSourceName() << std::endl;
        return -1;
    }

    std::cout << "[Main] Frame acquired. Resolution: " << frame.cols << "x" << frame.rows 
              << " (" << frame.channels() << " channels)" << std::endl;

    // Execute Gauge Recognition Pipeline
    GaugeReaderEngine engine;

    auto tStart = std::chrono::high_resolution_clock::now();
    GaugeResult result = engine.process(frame, profile);
    auto tEnd = std::chrono::high_resolution_clock::now();

    double elapsedMs = std::chrono::duration<double, std::milli>(tEnd - tStart).count();

    std::cout << "\n------------------ RECOGNITION RESULT ------------------\n";
    if (result.success) {
        std::cout << " Gauge ID       : " << profile.gauge_id << "\n";
        std::cout << " Reading Value  : " << result.value << " " << result.unit << "\n";
        std::cout << " Pointer Angle  : " << result.pointer_angle_deg << " deg\n";
        std::cout << " Dial Center    : (" << result.center.x << ", " << result.center.y << ")\n";
        std::cout << " Dial Radius    : " << result.radius << " px\n";
        std::cout << " Processing Time: " << elapsedMs << " ms\n";
        std::cout << " Confidence     : " << (result.confidence * 100.0) << " %\n";
        std::cout << "--------------------------------------------------------\n\n";

        if (!result.overlayImage.empty()) {
            if (cv::imwrite(outputPath, result.overlayImage)) {
                std::cout << "[Main] Result overlay saved successfully to: " << outputPath << std::endl;
            } else {
                std::cerr << "[Main] Error: Failed to save output image to " << outputPath << std::endl;
            }
        }

        if (!result.rectifiedImage.empty()) {
            std::string rectPath = "result_rectified.png";
            if (cv::imwrite(rectPath, result.rectifiedImage)) {
                std::cout << "[Main] Perspective rectified front-view image saved successfully to: " << rectPath << std::endl;
            } else {
                std::cerr << "[Main] Error: Failed to save rectified image to " << rectPath << std::endl;
            }
        }
    } else {
        std::cerr << " Status         : FAILED\n";
        std::cerr << " Error Step     : " << (result.errorStep.empty() ? "Unknown" : result.errorStep) << "\n";
        std::cerr << " Failure Reason : " << result.errorMessage << "\n";
        if (!result.detailedDebugInfo.empty()) {
            std::cerr << " Debug Details  :\n" << result.detailedDebugInfo << "\n";
        }
        std::cerr << " Processing Time: " << elapsedMs << " ms\n";
        std::cerr << "--------------------------------------------------------\n\n";

        if (!result.overlayImage.empty()) {
            if (cv::imwrite(outputPath, result.overlayImage)) {
                std::cout << "[Main] Diagnostic error overlay saved to: " << outputPath << std::endl;
            }
        }
        cameraSource->close();
        return -1;
    }

    cameraSource->close();
    std::cout << "[Main] Execution completed successfully.\n";
    return 0;
}
