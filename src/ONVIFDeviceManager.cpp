#include "ONVIFDeviceManager.hpp"
#include <iostream>
#include <sstream>
#include "OnvifSoap.hpp"
#include "SnapshotFrameReader.hpp"

ONVIFDeviceManager::ONVIFDeviceManager(const ONVIFDeviceInfo& info)
    : m_info(info) {}

ONVIFDeviceManager::~ONVIFDeviceManager() {
    close();
}

void ONVIFDeviceManager::logMessage(const std::string& msg) {
    std::cout << "[ONVIF] " << msg << std::endl;
    if (m_logCb) {
        m_logCb("[ONVIF] " + msg);
    }
}

#include <QUdpSocket>
#include <QTcpSocket>
#include <QHostAddress>
#include <QNetworkInterface>
#include <QElapsedTimer>
#include <QThread>
#include <QRegularExpression>
#include <set>

std::vector<ONVIFDeviceInfo> ONVIFDeviceManager::probeDevicesWSDiscovery(int timeoutMs) {
    std::vector<ONVIFDeviceInfo> list;
    std::set<std::string> discoveredIps;

    QUdpSocket socket;
    if (!socket.bind(QHostAddress::AnyIPv4, 0, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
        std::cout << "[ONVIF UDP Probe] Failed to bind UDP socket." << std::endl;
        return list;
    }

    // ONVIF WS-Discovery Probe XML Packet
    const char* probeXml = 
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" "
                    "xmlns:a=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\" "
                    "xmlns:d=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\" "
                    "xmlns:dn=\"http://www.onvif.org/ver10/network/wsdl\">"
          "<s:Header>"
            "<a:Action s:mustUnderstand=\"1\">http://schemas.xmlsoap.org/ws/2005/04/discovery/Probe</a:Action>"
            "<a:MessageID>uuid:a5f01344-9844-4861-a5d2-123456789abc</a:MessageID>"
            "<a:ReplyTo><a:Address>http://schemas.xmlsoap.org/ws/2004/08/addressing/role/anonymous</a:Address></a:ReplyTo>"
            "<a:To s:mustUnderstand=\"1\">urn:schemas-xmlsoap-org:ws:2005:04:discovery</a:To>"
          "</s:Header>"
          "<s:Body>"
            "<d:Probe>"
              "<d:Types>dn:NetworkVideoTransmitter</d:Types>"
            "</d:Probe>"
          "</s:Body>"
        "</s:Envelope>";

    QByteArray probeData(probeXml);
    QHostAddress multicastAddr("239.255.255.250");
    quint16 port = 3702;

    // Send probe multicast & broadcast packet
    socket.writeDatagram(probeData, multicastAddr, port);
    socket.writeDatagram(probeData, QHostAddress::Broadcast, port);
    socket.writeDatagram(probeData, QHostAddress("192.168.1.255"), port);
    socket.writeDatagram(probeData, QHostAddress("192.168.0.255"), port);

    // Common IP camera default IPs for USB direct connection
    const char* defaultCameraIps[] = {
        "192.168.1.64",   // Hikvision default
        "192.168.1.108",  // Dahua default
        "192.168.1.10",   // Uniview / TP-Link default
        "192.168.1.101",
        "192.168.1.102",
        "192.168.0.64",
        "192.168.0.108"
    };
    for (const char* ipStr : defaultCameraIps) {
        socket.writeDatagram(probeData, QHostAddress(QString(ipStr)), port);
    }

    // Also send multicast and directed broadcasts on active physical interfaces.
    QList<QNetworkInterface> interfaces = QNetworkInterface::allInterfaces();
    for (const auto& iface : interfaces) {
        if (iface.flags().testFlag(QNetworkInterface::IsUp) &&
            iface.flags().testFlag(QNetworkInterface::IsRunning) &&
            !iface.flags().testFlag(QNetworkInterface::IsLoopBack)) {

            // Ignore virtual network adapters (VMware, VirtualBox, vEthernet, WSL, VPN, TAP)
            QString ifaceName = iface.humanReadableName().toLower() + " " + iface.name().toLower();
            if (ifaceName.contains("vmware") || ifaceName.contains("virtualbox") || 
                ifaceName.contains("hyper-v") || ifaceName.contains("vethernet") || 
                ifaceName.contains("wsl") || ifaceName.contains("vbox") || 
                ifaceName.contains("tap") || ifaceName.contains("tun") || 
                ifaceName.contains("vpn") || ifaceName.contains("npcap") || ifaceName.contains("pcap")) {
                continue;
            }

            socket.setMulticastInterface(iface);
            socket.writeDatagram(probeData, multicastAddr, port);
            socket.writeDatagram(probeData, QHostAddress::Broadcast, port);

            // Directed subnet broadcast; avoid a 254-host unicast sweep.
            QList<QNetworkAddressEntry> entries = iface.addressEntries();
            for (const auto& entry : entries) {
                if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol) {
                    QHostAddress bcast = entry.broadcast();
                    if (!bcast.isNull()) {
                        socket.writeDatagram(probeData, bcast, port);
                    }
                }
            }
        }
    }

    QElapsedTimer timer;
    timer.start();

    while (timer.elapsed() < timeoutMs) {
        while (socket.hasPendingDatagrams()) {
            QByteArray datagram;
            datagram.resize(static_cast<int>(socket.pendingDatagramSize()));
            QHostAddress senderHost;
            quint16 senderPort;
            socket.readDatagram(datagram.data(), datagram.size(), &senderHost, &senderPort);

            QString resp = QString::fromUtf8(datagram);
            if (resp.contains("ProbeMatch", Qt::CaseInsensitive) || resp.contains("XAddrs", Qt::CaseInsensitive)) {
                QString senderIp = senderHost.toString();
                if (senderIp.startsWith("::ffff:")) {
                    senderIp = senderIp.mid(7);
                }

                // Extract XAddrs URL if present
                QString xaddr = QString("http://%1:80/onvif/device_service").arg(senderIp);
                static QRegularExpression xaddrRegex(R"(<[a-zA-Z0-9:]*XAddrs>([^<]+)</[a-zA-Z0-9:]*XAddrs>)");
                QRegularExpressionMatch match = xaddrRegex.match(resp);
                if (match.hasMatch()) {
                    QString extracted = match.captured(1).trimmed().split(' ').first();
                    if (!extracted.isEmpty()) xaddr = extracted;
                }

                // Extract IP from XAddrs if senderHost was proxy/gateway
                static QRegularExpression ipRegex(R"(http://([0-9]+\.[0-9]+\.[0-9]+\.[0-9]+))");
                QRegularExpressionMatch ipMatch = ipRegex.match(xaddr);
                if (ipMatch.hasMatch()) {
                    senderIp = ipMatch.captured(1);
                }

                std::string ipStd = senderIp.toStdString();
                if (discoveredIps.find(ipStd) == discoveredIps.end()) {
                    discoveredIps.insert(ipStd);

                    ONVIFDeviceInfo dev;
                    dev.ipAddress = ipStd;
                    dev.xaddr = xaddr.toStdString();
                    dev.manufacturer = "ONVIF IP Camera";
                    dev.model = "Network Camera";

                    // Extract Manufacturer / Model from Scopes if present
                    if (resp.contains("Hikvision", Qt::CaseInsensitive)) {
                        dev.manufacturer = "Hikvision Network Camera";
                    } else if (resp.contains("Dahua", Qt::CaseInsensitive)) {
                        dev.manufacturer = "Dahua Network Camera";
                    } else if (resp.contains("Uniview", Qt::CaseInsensitive) || resp.contains("UNV", Qt::CaseInsensitive)) {
                        dev.manufacturer = "Uniview Network Camera";
                    } else if (resp.contains("Xiongmai", Qt::CaseInsensitive) || resp.contains("XM", Qt::CaseInsensitive)) {
                        dev.manufacturer = "Xiongmai Network Camera";
                    }

                    dev.serialNumber = "SN-" + QString::number(qHash(senderIp)).toStdString();
                    dev.rtspUri.clear();
                    dev.port = QUrl(xaddr).port(80);

                    list.push_back(dev);
                }
            }
        }
        QThread::msleep(20);
    }

    return list;
}

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#endif

bool ONVIFDeviceManager::checkTcpPort(const std::string& ip, int port, int timeoutMs) {
    if (ip.empty() || port <= 0) return false;
#ifdef _WIN32
    static bool wsaStarted = false;
    if (!wsaStarted) {
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
        wsaStarted = true;
    }

    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) return false;

    u_long mode = 1;
    ioctlsocket(sock, FIONBIO, &mode);

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(port));
    inet_pton(AF_INET, ip.c_str(), &addr.sin_addr);

    connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));

    fd_set writeSet;
    FD_ZERO(&writeSet);
    FD_SET(sock, &writeSet);

    timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;

    int res = select(0, nullptr, &writeSet, nullptr, &tv);
    bool connected = false;
    if (res > 0 && FD_ISSET(sock, &writeSet)) {
        int optErr = 0;
        int optLen = sizeof(optErr);
        if (getsockopt(sock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&optErr), &optLen) == 0) {
            connected = (optErr == 0);
        }
    }
    closesocket(sock);
    return connected;
#else
    return true;
#endif
}

bool ONVIFDeviceManager::open() {
    logMessage("Connecting to ONVIF Device XAddr: " + m_info.xaddr);

    if (m_info.rtspUri.empty() && (!fetchProfiles() || fetchStreamUri().empty())) {
        logMessage(m_lastError);
        return false;
    }
    QUrl authenticated(QString::fromStdString(m_info.rtspUri));
    if (authenticated.userName().isEmpty() && !m_info.username.empty()) {
        authenticated.setUserName(QString::fromStdString(m_info.username));
        authenticated.setPassword(QString::fromStdString(m_info.password));
    }
    m_info.rtspUri = authenticated.toEncoded().toStdString();
    logMessage("Opening camera stream for " + m_info.ipAddress);
    m_isOpen = true;
    m_capAttempted = false;
    m_isRealStreamActive = false;
    m_reconnectAttempts = 0;
    m_consecutiveFailures = 0;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_sampleFrame = cv::Mat(); // Clear sample frame so network camera never defaults to 5-1-2.png
        m_latestFrame = cv::Mat();
    }

    // Stop previous worker thread if any
    m_workerRunning = false;
    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }

    // Start background capture & reconnection watchdog thread
    m_workerRunning = true;
    m_workerThread = std::thread(&ONVIFDeviceManager::workerLoop, this);
    return true;
}

bool ONVIFDeviceManager::fetchDeviceInformation() {
    QUrl endpoint(QString::fromStdString(m_info.xaddr));
    if (m_info.xaddr.empty()) {
        endpoint.setScheme("http"); endpoint.setHost(QString::fromStdString(m_info.ipAddress));
        endpoint.setPort(m_info.port); endpoint.setPath("/onvif/device_service");
    }
    QString error;
    const auto data = OnvifSoap::request(endpoint, "http://www.onvif.org/ver10/device/wsdl/GetDeviceInformation",
        "<tds:GetDeviceInformation/>", QString::fromStdString(m_info.username), QString::fromStdString(m_info.password), error);
    m_lastError = error.toStdString();
    if (data.isEmpty()) return false;
    m_info.manufacturer = OnvifSoap::element(data, "Manufacturer").toStdString();
    m_info.model = OnvifSoap::element(data, "Model").toStdString();
    m_info.serialNumber = OnvifSoap::element(data, "SerialNumber").toStdString();
    return true;
}

bool ONVIFDeviceManager::fetchProfiles() {
    QUrl endpoint(QString::fromStdString(m_info.xaddr));
    if (m_info.xaddr.empty()) {
        endpoint.setScheme("http"); endpoint.setHost(QString::fromStdString(m_info.ipAddress));
        endpoint.setPort(m_info.port); endpoint.setPath("/onvif/device_service");
    }
    QString error;
    auto data = OnvifSoap::request(endpoint, "http://www.onvif.org/ver10/device/wsdl/GetCapabilities",
        "<tds:GetCapabilities><tds:Category>Media</tds:Category></tds:GetCapabilities>",
        QString::fromStdString(m_info.username), QString::fromStdString(m_info.password), error);
    QXmlStreamReader reader(data);
    QString media;
    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isStartElement() && reader.name() == "Media") {
            while (reader.readNextStartElement()) {
                if (reader.name() == "XAddr") media = reader.readElementText();
                else reader.skipCurrentElement();
            }
        }
    }
    if (media.isEmpty()) { m_lastError = error.isEmpty() ? "Camera did not advertise an ONVIF Media service; enter a full RTSP address" : error.toStdString(); return false; }
    m_mediaEndpoint = endpoint.resolved(QUrl(media)).toString().toStdString();
    data = OnvifSoap::request(QUrl(QString::fromStdString(m_mediaEndpoint)), "http://www.onvif.org/ver10/media/wsdl/GetProfiles",
        "<trt:GetProfiles/>", QString::fromStdString(m_info.username), QString::fromStdString(m_info.password), error);
    QXmlStreamReader profiles(data);
    m_info.profileToken.clear();
    while (!profiles.atEnd()) {
        profiles.readNext();
        if (profiles.isStartElement() && profiles.name() == "Profiles") {
            const auto token = profiles.attributes().value("token").toString();
            if (!token.isEmpty()) { m_info.profileToken = token.toStdString(); break; }
        }
    }
    m_lastError = error.isEmpty() ? "No ONVIF media profile returned" : error.toStdString();
    if (!m_info.profileToken.empty()) { m_lastError.clear(); return true; }
    return false;
}

std::string ONVIFDeviceManager::fetchStreamUri(const std::string& profileToken) {
    if (m_mediaEndpoint.empty() && !fetchProfiles()) return {};
    QString error;
    const auto token = QString::fromStdString(profileToken.empty() ? m_info.profileToken : profileToken);
    const auto data = OnvifSoap::request(QUrl(QString::fromStdString(m_mediaEndpoint)), "http://www.onvif.org/ver10/media/wsdl/GetStreamUri",
        QString("<trt:GetStreamUri><trt:StreamSetup><tt:Stream>RTP-Unicast</tt:Stream><tt:Transport><tt:Protocol>RTSP</tt:Protocol></tt:Transport></trt:StreamSetup><trt:ProfileToken>%1</trt:ProfileToken></trt:GetStreamUri>").arg(OnvifSoap::escape(token)),
        QString::fromStdString(m_info.username), QString::fromStdString(m_info.password), error);
    const QUrl uri(OnvifSoap::element(data, "Uri"));
    if (!uri.isValid() || uri.host().isEmpty() || uri.scheme().compare("rtsp", Qt::CaseInsensitive) != 0) {
        m_lastError = error.isEmpty() ? "No valid RTSP URI returned; enter a full RTSP address" : error.toStdString(); return {};
    }
    m_info.rtspUri = uri.toEncoded().toStdString();
    m_lastError.clear();
    return m_info.rtspUri;
}

void ONVIFDeviceManager::setSampleFrame(const cv::Mat& sample) {
    if (!sample.empty()) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_sampleFrame = sample.clone();
    }
}

void ONVIFDeviceManager::workerLoop() {
    while (m_workerRunning) {
        if (!m_isRealStreamActive) {
            // STATE: DISCONNECTED / RECONNECTING
            std::string ip = m_info.ipAddress;
            const QUrl stream(QString::fromStdString(m_info.rtspUri));
            int rtspPort = stream.port(554);

            // 1. Check physical network link (RTSP 554, Web 80, JLink 34567)
            bool portOpen = checkTcpPort(stream.host().toStdString(), rtspPort, 250);

            if (!portOpen) {
                m_reconnectAttempts++;
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }

            // 2. Physical link established! Camera is online!
            logMessage("[ONVIF] 🟢 检测到摄像头物理网线已接通 (IP: " + ip + ")，正在启动自动拉流恢复...");

            std::vector<std::string> streamUrls;
            if (!m_info.rtspUri.empty()) streamUrls.push_back(m_info.rtspUri);

            bool connectSuccess = false;
            for (const auto& url : streamUrls) {
                if (!m_workerRunning) break;

                cv::VideoCapture testCap;
                std::vector<int> params = {
                    cv::CAP_PROP_OPEN_TIMEOUT_MSEC, 2500,
                    cv::CAP_PROP_READ_TIMEOUT_MSEC, 2500
                };
                testCap.open(url, cv::CAP_FFMPEG, params);
                if (testCap.isOpened()) {
                    cv::Mat testMat;
                    if (testCap.read(testMat) && !testMat.empty()) {
                        std::lock_guard<std::mutex> lock(m_mutex);
                        if (m_cap.isOpened()) m_cap.release();
                        m_cap = std::move(testCap);
                        m_latestFrame = testMat.clone();
                        m_isRealStreamActive = true;
                        m_connectedUrl = url;
                        m_consecutiveFailures = 0;
                        m_reconnectAttempts = 0;
                        m_lastFrameTime = std::chrono::steady_clock::now();
                        m_lastCaptureTime = std::chrono::system_clock::now();
                        connectSuccess = true;
                        logMessage("[ONVIF] 视频流连接成功: " + m_info.ipAddress);
                        break;
                    }
                }
            }

            if (!connectSuccess) {
                m_reconnectAttempts++;
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
        } else {
            // STATE: ACTIVE STREAMING
            cv::Mat frame;
            bool ok = false;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_cap.isOpened()) {
                    ok = m_cap.read(frame);
                }
            }

            if (ok && !frame.empty()) {
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_latestFrame = std::move(frame);
                    m_lastFrameTime = std::chrono::steady_clock::now();
                    m_lastCaptureTime = std::chrono::system_clock::now();
                    m_consecutiveFailures = 0;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            } else {
                // Read frame failed
                m_consecutiveFailures++;
                if (m_consecutiveFailures >= 3) {
                    logMessage("[ONVIF] 检测到网络视频流中断（网线拔出或连接断开），释放底层连接并进入自动重连监控...");
                    std::lock_guard<std::mutex> lock(m_mutex);
                    if (m_cap.isOpened()) m_cap.release();
                    m_isRealStreamActive = false;
                    m_consecutiveFailures = 0;
                }
            }
        }
    }
}

FrameCaptureResult ONVIFDeviceManager::readFrame() {
    FrameCaptureResult result;
    if (!m_isOpen) { result.failure = CaptureFailure::NotOpen; return result; }
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_isRealStreamActive || m_latestFrame.empty()) {
        result.failure = CaptureFailure::NoFrame;
        result.message = "No live frame available";
    } else if (std::chrono::steady_clock::now() - m_lastFrameTime >= std::chrono::seconds(2)) {
        result.failure = CaptureFailure::StaleFrame;
        result.message = "Cached frame is older than two seconds";
    } else {
        result.frame = m_latestFrame.clone();
        result.capturedAt = m_lastCaptureTime;
        ++m_frameCount;
    }
    return result;
}

bool ONVIFDeviceManager::grabFrame(cv::Mat& frame) {
    auto result = readFrame();
    frame = result.frame;
    return result.ok();
}

FrameCaptureResult ONVIFDeviceManager::captureHttpSnapshot() {
    FrameCaptureResult result;
    result.failure = CaptureFailure::ReadFailed;
    result.message = "ONVIF HTTP snapshot unavailable";
    if (m_mediaEndpoint.empty() && !fetchProfiles()) return result;
    QString error;
    const auto xml = OnvifSoap::request(QUrl(QString::fromStdString(m_mediaEndpoint)),
        "http://www.onvif.org/ver10/media/wsdl/GetSnapshotUri",
        QString("<trt:GetSnapshotUri><trt:ProfileToken>%1</trt:ProfileToken></trt:GetSnapshotUri>")
            .arg(OnvifSoap::escape(QString::fromStdString(m_info.profileToken))),
        QString::fromStdString(m_info.username), QString::fromStdString(m_info.password), error);
    const auto address = OnvifSoap::element(xml, "Uri");
    QUrl uri = QUrl(QString::fromStdString(m_mediaEndpoint)).resolved(QUrl(address));
    if (address.isEmpty() || !uri.isValid() || uri.host() != QString::fromStdString(m_info.ipAddress) ||
        (uri.scheme() != "http" && uri.scheme() != "https")) return result;
    uri.setUserInfo(QString());
    QNetworkAccessManager manager;
    int authAttempts = 0;
    QObject::connect(&manager, &QNetworkAccessManager::authenticationRequired,
        [&](QNetworkReply*, QAuthenticator* auth) {
            if (++authAttempts == 1) {
                auth->setUser(QString::fromStdString(m_info.username));
                auth->setPassword(QString::fromStdString(m_info.password));
            }
        });
    QNetworkRequest request(uri);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setRawHeader("Cache-Control", "no-cache");
    auto* reply = manager.get(request);
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timeout, &QTimer::timeout, &loop, [&]() { reply->abort(); loop.quit(); });
    QObject::connect(reply, &QNetworkReply::readyRead, [&]() {
        if (reply->bytesAvailable() > 16 * 1024 * 1024) reply->abort();
    });
    // Reserve time for RTSP fallback inside the existing capture-worker deadline.
    timeout.start(2000);
    loop.exec();
    const auto bytes = reply->readAll();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (reply->error() != QNetworkReply::NoError || status != 200 || bytes.size() > 16 * 1024 * 1024) return result;
    try { result.frame = cv::imdecode(std::vector<uchar>(bytes.begin(), bytes.end()), cv::IMREAD_COLOR); }
    catch (...) { result.frame.release(); }
    if (isUniformSnapshotFrame(result.frame)) { result.frame.release(); return result; }
    result.failure = CaptureFailure::None;
    result.capturedAt = std::chrono::system_clock::now();
    result.message = "ONVIF_HTTP";
    return result;
}

FrameCaptureResult ONVIFDeviceManager::captureSnapshot() {
    if (m_info.preferHttpSnapshot || m_info.requireHttpSnapshot) {
        auto snapshot = captureHttpSnapshot();
        if (snapshot.ok() || m_info.requireHttpSnapshot) return snapshot;
        logMessage("ONVIF HTTP snapshot failed; falling back to RTSP");
    }
    auto cached = readFrame();
    if (cached.ok() && !isUniformSnapshotFrame(cached.frame)) return cached;
    FrameCaptureResult result;
    // Use the configured, authenticated endpoint; never guess another identity or use samples.
    if (m_info.rtspUri.empty()) {
        if (fetchStreamUri().empty()) {
            result.failure = CaptureFailure::OpenFailed;
            result.message = m_lastError;
            return result;
        }
    }
    QUrl uri(QString::fromStdString(m_info.rtspUri));
    if (uri.userName().isEmpty() && !m_info.username.empty()) {
        uri.setUserName(QString::fromStdString(m_info.username));
        uri.setPassword(QString::fromStdString(m_info.password));
    }
    cv::VideoCapture cap;
    const std::vector<int> params = {cv::CAP_PROP_OPEN_TIMEOUT_MSEC, 2000,
                                     cv::CAP_PROP_READ_TIMEOUT_MSEC, 2000};
    if (!cap.open(uri.toEncoded().toStdString(), cv::CAP_FFMPEG, params)) {
        result.failure = CaptureFailure::OpenFailed;
        result.message = "Unable to open configured camera stream";
        return result;
    }
    return readDecodedSnapshot([&cap](cv::Mat& frame) { return cap.read(frame); }, SnapshotDecodePolicy(),
                               [&cap]() { return static_cast<int>(cap.get(cv::CAP_PROP_FRAME_TYPE)); });
}

bool ONVIFDeviceManager::captureSingleSnapshot(cv::Mat& frame) {
    auto result = captureSnapshot();
    frame = result.frame;
    return result.ok();
}

void ONVIFDeviceManager::close() {
    if (m_isOpen || m_workerRunning) {
        logMessage("Closing ONVIF RTSP Stream connection for " + m_info.ipAddress);
        m_isOpen = false;
        m_workerRunning = false;
        if (m_workerThread.joinable()) {
            m_workerThread.join();
        }
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_cap.isOpened()) {
            m_cap.release();
        }
        m_isRealStreamActive = false;
    }
}

bool ONVIFDeviceManager::isOpened() const {
    return m_isOpen;
}

std::string ONVIFDeviceManager::getSourceName() const {
    return "ONVIF_IPCam(" + m_info.ipAddress + ")";
}

CameraProtocolMetrics ONVIFDeviceManager::getMetrics() const {
    CameraProtocolMetrics m;
    m.protocolName = "ONVIF Profile S/G/T (RTSP/RTP)";
    m.deviceIdOrIp = m_info.ipAddress;
    m.fps = 30.0;
    m.bitrateKbps = 6144.0; // 6 Mbps H.264
    m.connected = m_isOpen;
    m.streamFormat = "H.264 / RTSP / UDP";
    m.statusString = m_isOpen ? "ONVIF_CONNECTED" : "OFFLINE";
    return m;
}
