#ifndef ONVIF_DEVICE_MANAGER_HPP
#define ONVIF_DEVICE_MANAGER_HPP

#include "ICameraSource.hpp"
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <atomic>
#include <chrono>

/**
 * @brief ONVIF Standard Device Metadata & Service Parameters.
 */
struct ONVIFDeviceInfo {
    std::string xaddr{"http://192.168.1.101:80/onvif/device_service"};
    std::string ipAddress{"192.168.1.101"};
    int port{80};
    std::string manufacturer{"Hikvision Network Camera"};
    std::string model{"DS-2CD2087G2-L"};
    std::string serialNumber{"HKV-2026-ONVIF-991"};
    std::string firmwareVersion{"V5.7.10"};
    std::string profileToken{"Profile_1_MainStream"};
    std::string rtspUri{"rtsp://192.168.1.101:554/h264/ch1/main/av_stream"};
    std::string username{""};
    std::string password{""};
};

/**
 * @brief ONVIF Compliant Device Manager & Media Stream Source.
 * 
 * Implements ONVIF WS-Discovery probe, Device Service management, and Media Profile 
 * URI retrieval.
 */
class ONVIFDeviceManager : public ICameraSource {
public:
    explicit ONVIFDeviceManager(const ONVIFDeviceInfo& info = ONVIFDeviceInfo());
    ~ONVIFDeviceManager() override;

    // --- ICameraSource Interface Implementation ---
    bool open() override;
    bool grabFrame(cv::Mat& frame) override;
    bool captureSingleSnapshot(cv::Mat& frame) override;
    void close() override;
    bool isOpened() const override;
    std::string getSourceName() const override;
    CameraProtocolType getProtocolType() const override { return CameraProtocolType::ONVIF_RTSP_STREAM; }
    CameraProtocolMetrics getMetrics() const override;

    // --- ONVIF Standard Services ---
    static std::vector<ONVIFDeviceInfo> probeDevicesWSDiscovery(int timeoutMs = 2000);
    bool fetchDeviceInformation();
    bool fetchProfiles();
    std::string fetchStreamUri(const std::string& profileToken = "");

    // Set sample frame image for offline/demo testing
    void setSampleFrame(const cv::Mat& sample);

    ONVIFDeviceInfo getDeviceInfo() const { return m_info; }
    void setDeviceInfo(const ONVIFDeviceInfo& info) { m_info = info; }
    void setDeviceIp(const std::string& ip) {
        m_info.ipAddress = ip;
        m_info.xaddr = "http://" + ip + ":80/onvif/device_service";
        m_info.rtspUri = "rtsp://" + ip + ":554/h264/ch1/main/av_stream";
    }

    using LogCallback = std::function<void(const std::string&)>;
    void setLogCallback(LogCallback cb) { m_logCb = cb; }

private:
    void logMessage(const std::string& msg);
    void workerLoop();
    static bool checkTcpPort(const std::string& ip, int port, int timeoutMs = 300);

    ONVIFDeviceInfo m_info;
    bool m_isOpen{false};
    bool m_capAttempted{false};
    bool m_isRealStreamActive{false};
    std::atomic<bool> m_workerRunning{false};
    std::atomic<int> m_consecutiveFailures{0};
    std::atomic<int> m_reconnectAttempts{0};
    std::chrono::steady_clock::time_point m_lastFrameTime;
    std::thread m_workerThread;
    std::string m_connectedUrl;
    cv::Mat m_latestFrame;
    cv::Mat m_sampleFrame;
    cv::VideoCapture m_cap;
    std::mutex m_mutex;
    uint64_t m_frameCount{0};
    LogCallback m_logCb{nullptr};
};

using ONVIFDeviceManagerPtr = std::shared_ptr<ONVIFDeviceManager>;

#endif // ONVIF_DEVICE_MANAGER_HPP
