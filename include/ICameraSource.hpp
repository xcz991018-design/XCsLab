#ifndef I_CAMERA_SOURCE_HPP
#define I_CAMERA_SOURCE_HPP

#include <opencv2/opencv.hpp>
#include <string>
#include <memory>

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
