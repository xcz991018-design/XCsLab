#include "FileCameraSource.hpp"
#include "SDKCameraSourceStub.hpp"
#include "GaugeProfile.hpp"
#include "GaugeReaderEngine.hpp"
#include "MySqlRepository.hpp"
#include "DeviceCodec.hpp"
#include <QCoreApplication>
#include <QJsonArray>

#include <iostream>
#include <iomanip>
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
              << "  --template-id <id>   Algorithm template ID in MySQL\n"
              << "  --camera-id <id>     Target Camera ID in database registry\n"
              << "  --output <path>      Path to save overlay result image (default: result.png)\n"
              << "  --mode <mode>        production, evidence, debug (default: debug)\n"
              << "  --sdk-stub           Run in reserved Camera SDK Stub mode\n"
              << "  --help               Display this help message\n\n";
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    printBanner();

    std::string imagePath = "";
    std::string templateId = "PRESSURE_GAUGE_1.6MPA";
    std::string cameraId;
    std::string outputPath = "result.png";
    bool runSdkStub = false;
    GaugeRunMode runMode = GaugeRunMode::Debug;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--image" && i + 1 < argc) {
            imagePath = argv[++i];
        } else if (arg == "--template-id" && i + 1 < argc) {
            templateId = argv[++i];
        } else if (arg == "--camera-id" && i + 1 < argc) {
            cameraId = argv[++i];
        } else if (arg == "--output" && i + 1 < argc) {
            outputPath = argv[++i];
        } else if (arg == "--mode" && i + 1 < argc) {
            const std::string mode = argv[++i];
            if (mode == "production") runMode = GaugeRunMode::Production;
            else if (mode == "evidence") runMode = GaugeRunMode::Evidence;
            else if (mode == "debug") runMode = GaugeRunMode::Debug;
            else { std::cerr << "Invalid run mode: " << mode << std::endl; return 2; }
        } else if (arg == "--sdk-stub") {
            runSdkStub = true;
        } else if (arg == "--help" || arg == "-h") {
            printHelp(argv[0]);
            return 0;
        }
    }

    // All normal runtime templates come from MySQL.
    GaugeProfile profile;
    try {
        MySqlRepository repository;
        if (!cameraId.empty()) {
            bool found = false;
            for (const auto& value : repository.loadRegistry()["devices"].toArray()) {
                const auto device = deviceFromJson(value.toObject());
                if (device.cameraId.toStdString() == cameraId) {
                    templateId = device.defaultProfileId.toStdString(); found = true; break;
                }
            }
            if (!found) { std::cerr << "CAMERA_NOT_FOUND\n"; return 2; }
        }
        if (!profile.loadFromJson(repository.loadTemplate(QString::fromStdString(templateId)).definition)) {
            std::cerr << "DATABASE_TEMPLATE_INVALID\n"; return 2;
        }
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 2; }

    // Select Camera Source (Abstraction Layer)
    CameraSourcePtr cameraSource;

    if (runSdkStub) {
        std::cout << "[Main] Initializing Industrial Camera SDK Stub..." << std::endl;
        cameraSource = std::make_shared<SDKCameraSourceStub>("CAM_SN_8839210", 0);
    } else {
        if (imagePath.empty()) {
            std::cerr << "INVALID_INPUT: Local analysis requires an explicit --image path" << std::endl;
            return 2;
        }
        std::cout << "[Main] Initializing File Camera Source from: " << imagePath << std::endl;
        cameraSource = std::make_shared<FileCameraSource>(imagePath);
    }

    const auto acquired = cameraSource->captureSnapshot();
    if (!acquired.ok()) {
        std::cerr << "[Main] Capture error: " << captureFailureName(acquired.failure) << std::endl;
        return -1;
    }
    const cv::Mat frame = acquired.frame;

    std::cout << "[Main] Frame acquired. Resolution: " << frame.cols << "x" << frame.rows 
              << " (" << frame.channels() << " channels)" << std::endl;

    // Execute Gauge Recognition Pipeline
    GaugeReaderEngine engine;

    auto tStart = std::chrono::high_resolution_clock::now();
    GaugeResult result = engine.process(frame, profile, runMode);
    if (runMode == GaugeRunMode::Evidence) result.overlayImage = result.step4_recognition;
    auto tEnd = std::chrono::high_resolution_clock::now();

    double elapsedMs = std::chrono::duration<double, std::milli>(tEnd - tStart).count();

    std::cout << "\n------------------ RECOGNITION RESULT ------------------\n";
    if (result.success) {
        std::cout << " Gauge ID       : " << profile.gauge_id << "\n";
        std::cout << " Reading Value  : " << std::fixed << std::setprecision(3) << result.value << " " << result.unit << "\n" << std::defaultfloat << std::setprecision(6);
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
