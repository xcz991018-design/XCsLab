#include "GB28181StreamManager.hpp"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <random>

GB28181StreamManager::GB28181StreamManager(const GB28181Config& config)
    : m_config(config) {
    m_sdpSessionId = "SDP_GB28181_SESS_" + std::to_string(std::rand() % 90000 + 10000);
}

GB28181StreamManager::~GB28181StreamManager() {
    close();
}

void GB28181StreamManager::logMessage(const std::string& msg) {
    std::cout << "[GB28181] " << msg << std::endl;
    if (m_logCb) {
        m_logCb("[GB28181] " + msg);
    }
}

bool GB28181StreamManager::open() {
    logMessage("Initiating GB28181 SIP Registration & Stream Setup...");
    if (!registerSip()) {
        logMessage("ERROR: GB28181 SIP Registration failed to server: " + m_config.sipServerIp);
        return false;
    }

    if (!startPushStream()) {
        logMessage("ERROR: GB28181 SDP Invite / Push stream failed.");
        return false;
    }

    m_startTime = std::chrono::steady_clock::now();
    return true;
}

bool GB28181StreamManager::registerSip() {
    std::stringstream ss;
    ss << "Sending SIP REGISTER -> sip:" << m_config.sipServerId << "@" << m_config.sipDomain
       << " (Local Device: " << m_config.deviceId << ", Transport: " << m_config.transportProtocol << ")";
    logMessage(ss.str());

    // Simulated SIP 200 OK Handshake
    m_sipStatus = GB28181SipStatus::REGISTERED;
    logMessage("SIP/2.0 200 OK - Device registered successfully to GB28181 SIP Server");
    return true;
}

bool GB28181StreamManager::unregisterSip() {
    logMessage("Sending SIP REGISTER (Expires: 0) -> Unregistering device...");
    m_sipStatus = GB28181SipStatus::UNREGISTERED;
    return true;
}

bool GB28181StreamManager::sendHeartbeat() {
    if (m_sipStatus != GB28181SipStatus::REGISTERED) return false;
    logMessage("Sending GB28181 KeepAlive Heartbeat XML to Server -> Status: OK");
    return true;
}

bool GB28181StreamManager::startPushStream() {
    std::stringstream ss;
    ss << "Negotiating GB28181 SDP Session (Payload 96 PS Stream, RTSP/RTP Over " << m_config.transportProtocol 
       << ", SessionID: " << m_sdpSessionId << ")...";
    logMessage(ss.str());

    m_isStreaming = true;
    logMessage("GB28181 Realtime PS Stream Session established. Ready to receive frames.");
    return true;
}

bool GB28181StreamManager::stopPushStream() {
    logMessage("Sending SIP BYE -> Closing GB28181 PS Stream Session.");
    m_isStreaming = false;
    return true;
}

void GB28181StreamManager::setSampleFrame(const cv::Mat& sample) {
    if (!sample.empty()) {
        m_sampleFrame = sample.clone();
    }
}

bool GB28181StreamManager::grabFrame(cv::Mat& frame) {
    if (!m_isStreaming) {
        return false;
    }

    m_frameCount++;
    m_packetCount += 45; // Simulated PS RTP packet increment

    if (!m_sampleFrame.empty()) {
        frame = m_sampleFrame.clone();
        
        // Overlay GB28181 Stream Watermark / Stamp for Demonstration
        std::stringstream ss;
        ss << "GB28181-2016 PS Stream | ID: " << m_config.deviceId
           << " | Frm: " << m_frameCount;
        cv::putText(frame, ss.str(), cv::Point(20, 30),
                    cv::FONT_HERSHEY_SIMPLEX, 0.65, cv::Scalar(0, 255, 255), 2);
        return true;
    }

    return false;
}

void GB28181StreamManager::close() {
    if (m_isStreaming) {
        stopPushStream();
    }
    if (m_sipStatus == GB28181SipStatus::REGISTERED) {
        unregisterSip();
    }
}

bool GB28181StreamManager::isOpened() const {
    return m_isStreaming && m_sipStatus == GB28181SipStatus::REGISTERED;
}

std::string GB28181StreamManager::getSourceName() const {
    return "GB28181(" + m_config.deviceId + "@" + m_config.sipServerIp + ")";
}

CameraProtocolMetrics GB28181StreamManager::getMetrics() const {
    CameraProtocolMetrics m;
    m.protocolName = "GB28181-2016 National Standard";
    m.deviceIdOrIp = m_config.deviceId;
    m.fps = 25.0;
    m.bitrateKbps = 4096.0; // 4 Mbps H.264/PS Stream
    m.connected = isOpened();
    m.streamFormat = "PS (Program Stream) / RTP / " + m_config.transportProtocol;
    m.statusString = m_isStreaming ? "REGISTERED & STREAMING" : "IDLE";
    return m;
}
