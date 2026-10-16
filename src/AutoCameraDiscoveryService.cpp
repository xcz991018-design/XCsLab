#include "AutoCameraDiscoveryService.hpp"
#include <QtConcurrent/QtConcurrent>
#include <QTcpSocket>
#include <QNetworkInterface>
#include <QSet>
#include <QDebug>
#include <set>
#include "DeviceConfigManager.hpp"
#include "CameraIdentity.hpp"
#include <QUrl>
#include "CameraDiscoveryTargets.hpp"
#include <QFile>
#include <QJsonDocument>

AutoCameraDiscoveryService::AutoCameraDiscoveryService(QObject *parent)
    : QObject(parent) {
    qRegisterMetaType<DiscoveredCameraItem>("DiscoveredCameraItem");

    m_discoveryPool.setMaxThreadCount(1);
    m_clock.start();
    m_healthTimer = new QTimer(this);
    m_healthTimer->setInterval(1000);
    connect(m_healthTimer, &QTimer::timeout, this, &AutoCameraDiscoveryService::pollHealth);
    m_scanTimer = new QTimer(this);
    connect(m_scanTimer, &QTimer::timeout, this, &AutoCameraDiscoveryService::onScanTimerTimeout);
}

AutoCameraDiscoveryService::~AutoCameraDiscoveryService() {
    stopAutoScan();
    m_scanFuture.waitForFinished();
}

void AutoCameraDiscoveryService::startAutoScan(int intervalMs) {
    m_stopping.store(false);
    m_scanTimer->start(std::max(intervalMs, 300000));
    m_healthTimer->start();
    emit logGenerated(tr("[AUTO-DISCOVERY] 启用低频新设备发现（%1 分钟）及分批健康检查（并发上限 16，每秒最多 32 台）。")
        .arg(m_scanTimer->interval()/60000));
    startDiscovery(false);
    pollHealth();
}

void AutoCameraDiscoveryService::stopAutoScan() {
    m_stopping.store(true);
    m_scanTimer->stop(); m_healthTimer->stop();
    const auto sockets = m_healthSockets;
    m_healthSockets.clear();
    for (auto socket : sockets) if (socket) { socket->disconnect(this); socket->abort(); socket->deleteLater(); }
    for (auto& state : m_health.states) { ++state.revision; state.inFlight = false; }
}

void AutoCameraDiscoveryService::onScanTimerTimeout() { startDiscovery(false); }

void AutoCameraDiscoveryService::triggerScanNow() {
    if (m_stopping.load()) return;
    for (auto& state : m_health.states) {
        const auto now = m_clock.elapsed();
        if (state.captureBusyUntil <= now && (state.lastLiveAt < 0 || now - state.lastLiveAt >= 30000)) state.due = 0;
    }
    pollHealth();
    startDiscovery(true);
}

void AutoCameraDiscoveryService::startDiscovery(bool manual) {
    {
        QMutexLocker locker(&m_mutex);
        if (m_stopping.load() || m_isScanning) return;
        m_isScanning = true;
    }
    if (manual) emit logGenerated(tr("[AUTO-DISCOVERY] 手动刷新：发现新设备并分批检查已登记设备。"));
    m_scanFuture = QtConcurrent::run(&m_discoveryPool, [this, manual]() {
        try { executeDiscoveryProcess(manual); }
        catch (const std::exception& error) {
            emit logGenerated(tr("[AUTO-DISCOVERY] 发现任务失败: %1").arg(QString::fromUtf8(error.what())));
            emit discoveryCompleted(0);
        }
        QMutexLocker locker(&m_mutex); m_isScanning = false;
    });
}

void AutoCameraDiscoveryService::publishHealth(const QString& cameraId, bool online, bool changed) {
    if (!changed || m_stopping.load()) return;
    emit cameraOnlineStateChanged(cameraId, online);
    emit logGenerated(tr("[CAMERA-HEALTH] 设备 %1 状态变为%2。").arg(cameraId, online ? tr("在线") : tr("离线")));
}

void AutoCameraDiscoveryService::recordCaptureState(const QString& cameraId, bool online) {
    if (m_stopping.load() || !DeviceConfigManager::instance().hasDevice(cameraId)) return;
    const auto camera = DeviceConfigManager::instance().getDevice(cameraId);
    if (camera.protocolType == CameraProtocolType::LOCAL_FILE) return;
    const QUrl uri(camera.rtspUrl);
    const auto host = uri.host().isEmpty() ? camera.ipOrSn : uri.host();
    const auto port = uri.port(camera.rtspPort > 0 ? camera.rtspPort : 554);
    m_health.configure(cameraId, host+":"+QString::number(port));
    const bool changed = m_health.update(cameraId, online, m_clock.elapsed(), true);
    if (auto socket = m_healthSockets.take(cameraId)) { socket->disconnect(this); socket->abort(); socket->deleteLater(); }
    publishHealth(cameraId, online, changed);
    if (!changed && camera.isOnline != online) emit cameraOnlineStateChanged(cameraId, online);
}

void AutoCameraDiscoveryService::deferHealthForCapture(const QString& cameraId) {
    if (m_stopping.load() || !DeviceConfigManager::instance().hasDevice(cameraId)) return;
    const auto camera = DeviceConfigManager::instance().getDevice(cameraId);
    if (camera.protocolType == CameraProtocolType::LOCAL_FILE) return;
    const QUrl uri(camera.rtspUrl);
    const auto host = uri.host().isEmpty() ? camera.ipOrSn : uri.host();
    auto& state = m_health.configure(cameraId, host+":"+QString::number(uri.port(camera.rtspPort > 0 ? camera.rtspPort : 554)));
    ++state.revision; state.inFlight = false; state.due = m_clock.elapsed() + 30000;
    state.captureBusyUntil = state.due;
    if (auto socket = m_healthSockets.take(cameraId)) { socket->disconnect(this); socket->abort(); socket->deleteLater(); }
}

void AutoCameraDiscoveryService::recordInspectionState(const QString& cameraId, const QString& errorCode, bool captured) {
    if (captured) { recordCaptureState(cameraId, true); return; }
    static const QSet<QString> captureFailures = {"OPEN_FAILED", "NOT_OPEN", "READ_FAILED", "NO_FRAME", "STALE_FRAME",
        "CAPTURE_TIMEOUT", "CAPTURE_WORKER_FAILED", "CAPTURE_EXCEPTION"};
    if (captureFailures.contains(errorCode)) recordCaptureState(cameraId, false);
}

void AutoCameraDiscoveryService::pollHealth() {
    if (m_stopping.load()) return;
    const auto window = m_clock.elapsed() / 1000;
    if (window != m_healthWindow) { m_healthWindow = window; m_healthStarted = 0; }
    QList<DiscoveredCameraItem> cameras;
    // A database archive load must not block the UI event loop's health timer.
    if (!DeviceConfigManager::instance().tryGetAllDevices(cameras)) return;
    QSet<QString> ids;
    for (const auto& camera : cameras) if (camera.protocolType != CameraProtocolType::LOCAL_FILE) ids.insert(camera.cameraId);
    m_health.prune(ids);
    const auto oldSockets = m_healthSockets;
    for (auto it = oldSockets.begin(); it != oldSockets.end(); ++it) if (!ids.contains(it.key())) {
        m_healthSockets.remove(it.key());
        if (it.value()) { it.value()->disconnect(this); it.value()->abort(); it.value()->deleteLater(); }
    }
    int started = 0;
    for (int visited = 0; visited < cameras.size() && started < 32 && m_healthStarted < 32 && m_healthSockets.size() < 16; ++visited) {
        if (cameras.isEmpty()) break;
        const auto camera = cameras[m_healthCursor++ % cameras.size()];
        if (m_healthCursor >= cameras.size()) m_healthCursor = 0;
        if (camera.protocolType != CameraProtocolType::ONVIF_RTSP_STREAM) continue;
        const QUrl uri(camera.rtspUrl);
        const auto host = uri.host().isEmpty() ? camera.ipOrSn : uri.host();
        const int port = uri.port(camera.rtspPort > 0 ? camera.rtspPort : 554);
        auto& state = m_health.configure(camera.cameraId, host+":"+QString::number(port));
        if (state.inFlight || state.due > m_clock.elapsed() || m_healthSockets.contains(camera.cameraId)) continue;
        const auto revision = m_health.begin(camera.cameraId);
        auto* socket = new QTcpSocket(this);
        m_healthSockets.insert(camera.cameraId, socket); ++started; ++m_healthStarted;
        auto* timeout = new QTimer(socket); timeout->setSingleShot(true);
        const auto done = [this, id = camera.cameraId, wasOnline = camera.isOnline, revision, socket, timeout](bool online) {
            if (m_healthSockets.value(id) != socket) return;
            m_healthSockets.remove(id); timeout->stop();
            socket->disconnect(this); socket->abort(); socket->deleteLater();
            bool changed = false;
            if (m_health.complete(id, revision, online, m_clock.elapsed(), changed)) {
                publishHealth(id, online, changed);
                if (!changed && wasOnline != online && !m_stopping.load()) emit cameraOnlineStateChanged(id, online);
            }
        };
        connect(socket, &QTcpSocket::connected, this, [done]() { done(true); });
        connect(socket, QOverload<QAbstractSocket::SocketError>::of(&QTcpSocket::error), this,
            [done](QAbstractSocket::SocketError) { done(false); });
        connect(timeout, &QTimer::timeout, this, [done]() { done(false); });
        timeout->start(1000);
        socket->connectToHost(host, static_cast<quint16>(port));
    }
}

void AutoCameraDiscoveryService::executeDiscoveryProcess(bool manual) {
    const auto knownCameras = DeviceConfigManager::instance().getAllDevices();
    QSet<QString> registeredHosts;
    for (const auto& camera : knownCameras) {
        const QUrl uri(camera.rtspUrl);
        if (camera.protocolType != CameraProtocolType::LOCAL_FILE)
            registeredHosts.insert(uri.host().isEmpty() ? camera.ipOrSn : uri.host());
    }
    auto onvifList = ONVIFDeviceManager::probeDevicesWSDiscovery(1500);

    int newFound = 0;
    std::set<std::string> foundIps;

    for (const auto& dev : onvifList) {
        DiscoveredCameraItem item;
        item.hardwareId = CameraIdentity::normalize(QString::fromStdString(dev.hardwareId));
        item.cameraId = CameraIdentity::archiveId(item.hardwareId);
        if (item.cameraId.isEmpty()) {
            emit logGenerated(tr("[AUTO-DISCOVERY] %1 未返回有效设备 UUID，不能自动绑定档案。").arg(QString::fromStdString(dev.ipAddress)));
            foundIps.insert(dev.ipAddress);
            continue;
        }
        item.customName = QString("ONVIF 网络相机 (%1)").arg(QString::fromStdString(dev.ipAddress));
        item.ipOrSn = QString::fromStdString(dev.ipAddress);
        item.protocolType = CameraProtocolType::ONVIF_RTSP_STREAM;
        item.protocolName = "ONVIF (RTSP)";
        item.modelName = QString::fromStdString(dev.manufacturer + " " + dev.model);
        item.port = dev.port > 0 ? dev.port : 80;
        item.rtspPort = 554;
        item.rtspUrl = QString::fromStdString(dev.rtspUri);
        item.username = "";
        item.password = "";
        item.isOnline = true;
        item.defaultProfileId = "PRESSURE_GAUGE_1.6MPA";
        item.lastSeenTime = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");

        if (m_stopping.load()) return;
        emit cameraDiscovered(item);
        emit logGenerated(tr("[AUTO-DISCOVERY] 发现设备 UUID: %1，当前地址: %2。").arg(item.hardwareId, item.ipOrSn));
        newFound++;
        foundIps.insert(dev.ipAddress);
    }

    // Verify unregistered candidate RTSP endpoints with bounded workers.
    QSet<QString> candidateIpSet = {
        "192.168.1.10",   // Xiongmai / JFTech default IP
        "192.168.1.64",   // Hikvision default IP
        "192.168.1.108",  // Dahua / Xiongmai candidate IP
        "192.168.1.101",
        "192.168.1.102",
        "192.168.0.10",
        "192.168.0.64",
        "192.168.0.108"
    };

    QFile credentials(DeviceConfigManager::resolveConfigPath());
    QJsonObject credentialAddresses;
    if (credentials.open(QIODevice::ReadOnly))
        credentialAddresses = QJsonDocument::fromJson(credentials.readAll()).object();
    // Release the Windows read handle before emitting discovery results that save this archive.
    credentials.close();
    const auto savedTargets = cameraDiscoveryTargets(knownCameras, credentialAddresses);
    for (auto it = savedTargets.begin(); it != savedTargets.end(); ++it) candidateIpSet.insert(it.key());

    // Dynamically add active local network interface gateway & subnet IPs to candidate set
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

            for (const auto& entry : iface.addressEntries()) {
                if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol) {
                    QString ipStr = entry.ip().toString();
                    int lastDot = ipStr.lastIndexOf('.');
                    if (lastDot != -1) {
                        QString prefix = ipStr.left(lastDot + 1);
                        candidateIpSet.insert(prefix + "10");
                        candidateIpSet.insert(prefix + "64");
                        candidateIpSet.insert(prefix + "108");
                        candidateIpSet.insert(prefix + "101");
                        candidateIpSet.insert(prefix + "102");
                    }
                }
            }
        }
    }

    const auto candidates = candidateIpSet.values();
    std::atomic<int> nextCandidate{0}, fallbackFound{0};
    QThreadPool candidatePool; candidatePool.setMaxThreadCount(8);
    QList<QFuture<void>> candidateJobs;
    for (int worker = 0; worker < std::min(8, candidates.size()); ++worker) {
        candidateJobs.append(QtConcurrent::run(&candidatePool, [&]() {
            while (!m_stopping.load()) {
                const int index = nextCandidate.fetch_add(1);
                if (index >= candidates.size()) break;
                const QString candIp = candidates[index];
                if (m_stopping.load()) return;
                if (registeredHosts.contains(candIp) || foundIps.find(candIp.toStdString()) != foundIps.end()) continue;

                // An arbitrary HTTP service must not be registered as a camera.
                const auto portsToTest = savedTargets.value(candIp, QList<int>{554});
                bool isReachable = false;
                int hitPort = 0;

                for (int testPort : portsToTest) {
                    QTcpSocket socket;
                    socket.connectToHost(candIp, static_cast<quint16>(testPort));
                    if (socket.waitForConnected(350)) { // 350ms check
                        socket.write("OPTIONS * RTSP/1.0\r\nCSeq: 1\r\n\r\n");
                        socket.waitForBytesWritten(350);
                        if (socket.waitForReadyRead(500) && socket.readLine().startsWith("RTSP/1.0 ")) {
                            isReachable = true;
                            hitPort = testPort;
                        }
                        socket.abort(); // Immediately release the camera connection slot.
                        if (isReachable) break;
                    }
                    socket.abort();
                }

                if (isReachable) {
                    emit logGenerated(tr("[AUTO-DISCOVERY] %1 仅验证了 RTSP 服务，未取得硬件 UUID，暂不自动登记。").arg(candIp));
                    emit logGenerated(tr("[AUTO-DISCOVERY] Found Active Camera at %1 (RTSP Port %2).").arg(candIp).arg(hitPort));
                    ++fallbackFound;
                }
            }
        }));
    }
    for (auto& job : candidateJobs) job.waitForFinished();
    newFound += fallbackFound.load();
    if (m_stopping.load()) return;

    emit discoveryCompleted(newFound);
    if (!manual) return;
    if (newFound == 0) {
        emit logGenerated(tr("[AUTO-DISCOVERY] Scan completed. No real cameras responded on the local network (Total: 0)."));
    } else {
        emit logGenerated(tr("[AUTO-DISCOVERY] Scan completed. Active real cameras updated. Total: %1.").arg(newFound));
    }
}
