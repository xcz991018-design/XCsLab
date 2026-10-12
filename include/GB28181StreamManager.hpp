#ifndef GB28181_STREAM_MANAGER_HPP
#define GB28181_STREAM_MANAGER_HPP

#include "ICameraSource.hpp"
#include <string>
#include <memory>
#include <chrono>
#include <atomic>
#include <functional>

/**
 * @brief GB28181 National Standard SIP Configuration Parameters.
 */
struct GB28181Config {
    std::string sipServerIp{"192.168.1.100"};
    int sipServerPort{5060};
    std::string sipServerId{"34020000002000000001"};
    std::string sipDomain{"3402000000"};
    std::string deviceId{"34020000001320000001"};
    std::string channelId{"34020000001320000001"};
    std::string password{"12345678"};
    int localPort{5060};
    std::string transportProtocol{"UDP"}; // UDP or TCP
    int heartbeatIntervalSec{60};
    int rtpPayloadType{96}; // 96 for PS Stream
};

/**
 * @brief GB28181 Protocol & Stream Session State.
 */
enum class GB28181SipStatus {
    UNREGISTERED,
    REGISTERING,
    REGISTERED,
    AUTH_FAILED,
    HEARTBEAT_TIMEOUT
};

/**
 * @brief GB28181 Compliant Video/Image Transmission Stream Source.
 * 
 * Manages SIP registration, SDP session negotiation, and PS/RTP real-time
 * video and image stream reception compliant with GB28181 standard.
 */
class GB28181StreamManager : public ICameraSource {
public:
    explicit GB28181StreamManager(const GB28181Config& config = GB28181Config());
    ~GB28181StreamManager() override;

    // --- ICameraSource Interface Implementation ---
    bool open() override;
    bool grabFrame(cv::Mat& frame) override;
    FrameCaptureResult readFrame() override;
    FrameCaptureResult captureSnapshot() override { return readFrame(); }
    bool captureSingleSnapshot(cv::Mat& frame) override { frame.release(); return false; }
    void close() override;
    bool isOpened() const override;
    std::string getSourceName() const override;
    CameraProtocolType getProtocolType() const override { return CameraProtocolType::GB28181_PS_STREAM; }
    CameraProtocolMetrics getMetrics() const override;

    // --- GB28181 Standard Signaling & Control ---
    bool registerSip();
    bool unregisterSip();
    bool sendHeartbeat();
    bool startPushStream();
    bool stopPushStream();

    // Set sample frame image for offline/demo streaming testing
    void setSampleFrame(const cv::Mat& sample);

    // Configuration getters & setters
    GB28181Config getConfig() const { return m_config; }
    void setConfig(const GB28181Config& cfg) { m_config = cfg; }
    GB28181SipStatus getSipStatus() const { return m_sipStatus; }

    // Log callback signature for UI log console
    using LogCallback = std::function<void(const std::string&)>;
    void setLogCallback(LogCallback cb) { m_logCb = cb; }

private:
    void logMessage(const std::string& msg);

    GB28181Config m_config;
    GB28181SipStatus m_sipStatus{GB28181SipStatus::UNREGISTERED};
    std::atomic<bool> m_isStreaming{false};
    std::atomic<uint64_t> m_frameCount{0};
    std::atomic<uint64_t> m_packetCount{0};
    
    cv::Mat m_currentFrame;
    cv::Mat m_sampleFrame;
    LogCallback m_logCb{nullptr};
    
    std::chrono::steady_clock::time_point m_startTime;
    std::string m_sdpSessionId;
};

using GB28181StreamManagerPtr = std::shared_ptr<GB28181StreamManager>;

#endif // GB28181_STREAM_MANAGER_HPP
