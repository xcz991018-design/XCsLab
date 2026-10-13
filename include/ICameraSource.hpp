#ifndef I_CAMERA_SOURCE_HPP
#define I_CAMERA_SOURCE_HPP

#include <opencv2/opencv.hpp>
#include <string>
#include <memory>
#include <chrono>

enum class CaptureFailure { None, NotOpen, OpenFailed, ReadFailed, NoFrame, StaleFrame, UnsupportedProtocol, InvalidInput, Timeout, Cancelled, WorkerFailed };
inline const char* captureFailureName(CaptureFailure error) {
    switch (error) {
    case CaptureFailure::None: return "OK";
    case CaptureFailure::NotOpen: return "NOT_OPEN";
    case CaptureFailure::OpenFailed: return "OPEN_FAILED";
    case CaptureFailure::ReadFailed: return "READ_FAILED";
    case CaptureFailure::NoFrame: return "NO_FRAME";
    case CaptureFailure::StaleFrame: return "STALE_FRAME";
    case CaptureFailure::UnsupportedProtocol: return "UNSUPPORTED_PROTOCOL";
    case CaptureFailure::InvalidInput: return "INVALID_INPUT";
    case CaptureFailure::Timeout: return "CAPTURE_TIMEOUT";
    case CaptureFailure::Cancelled: return "CAPTURE_CANCELLED";
    case CaptureFailure::WorkerFailed: return "CAPTURE_WORKER_FAILED";
    }
    return "INVALID_INPUT";
}
struct FrameCaptureResult {
    cv::Mat frame;
    CaptureFailure failure{CaptureFailure::None};
    std::string message;
    std::chrono::system_clock::time_point capturedAt;
    bool ok() const { return failure == CaptureFailure::None && !frame.empty(); }
};

/**
 * @brief Camera Protocol Classification Enum.
 */
enum class CameraProtocolType {
    LOCAL_FILE,
    GB28181_PS_STREAM,
    ONVIF_RTSP_STREAM,
    SDK_JLINK,
    CUSTOM_SDK
};

/**
 * @brief Dynamic Camera & Protocol Metrics Info.
 */
struct CameraProtocolMetrics {
    std::string protocolName{"Local File"};
    std::string deviceIdOrIp{"127.0.0.1"};
    double fps{25.0};
    double bitrateKbps{2048.0};
    bool connected{true};
    std::string streamFormat{"H.264/PS"};
    std::string statusString{"STREAMING_OK"};
};

/**
 * @brief Abstract Interface for Camera / Image Source.
 * 
 * Supports hundreds of cameras via SDK & GB28181/ONVIF standard abstraction.
 * Derived classes can be FileCameraSource, GB28181StreamSource, ONVIFStreamSource,
 * or JLinkCameraSource (JFTech JLink RESTful C++ SDK wrapper).
 */
class ICameraSource {
public:
    virtual ~ICameraSource() = default;

    /**
     * @brief Initialize and open camera device / image file stream.
     * @return true if successful, false otherwise.
     */
    virtual bool open() = 0;

    /**
     * @brief Grab the next available frame.
     * @param frame Output OpenCV matrix.
     * @return true if frame acquired successfully.
     */
    virtual bool grabFrame(cv::Mat& frame) = 0;

    /**
     * @brief Capture a standalone snapshot frame without maintaining continuous stream.
     * @param frame Output OpenCV matrix.
     * @return true if snapshot acquired successfully.
     */
    virtual bool captureSingleSnapshot(cv::Mat& frame) {
        frame.release();
        if (!isOpened()) {
            if (!open()) return false;
            bool ok = grabFrame(frame);
            close();
            return ok;
        }
        return grabFrame(frame);
    }

    // Typed acquisition returns only real image data; diagnostic text belongs to UI.
    virtual FrameCaptureResult readFrame() {
        FrameCaptureResult result;
        if (!isOpened()) { result.failure = CaptureFailure::NotOpen; return result; }
        if (!grabFrame(result.frame)) { result.frame.release(); result.failure = CaptureFailure::ReadFailed; }
        else if (result.frame.empty()) result.failure = CaptureFailure::NoFrame;
        else result.capturedAt = std::chrono::system_clock::now();
        return result;
    }
    virtual FrameCaptureResult captureSnapshot() {
        FrameCaptureResult result;
        const bool alreadyOpen = isOpened();
        if (!alreadyOpen && !open()) { result.failure = CaptureFailure::OpenFailed; return result; }
        result = readFrame();
        if (!alreadyOpen) close();
        return result;
    }

    /**
     * @brief Release and close device resources.
     */
    virtual void close() = 0;

    /**
     * @brief Check if camera source is connected and open.
     */
    virtual bool isOpened() const = 0;

    /**
     * @brief Get human-readable camera / stream identifier.
     */
    virtual std::string getSourceName() const = 0;

    /**
     * @brief Get protocol classification.
     */
    virtual CameraProtocolType getProtocolType() const { return CameraProtocolType::LOCAL_FILE; }

    /**
     * @brief Get real-time stream & protocol metrics.
     */
    virtual CameraProtocolMetrics getMetrics() const { return CameraProtocolMetrics(); }
};

using CameraSourcePtr = std::shared_ptr<ICameraSource>;

#endif // I_CAMERA_SOURCE_HPP
