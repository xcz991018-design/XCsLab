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
#include "CameraHealthProbe.hpp"
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
}

AutoCameraDiscoveryService::~AutoCameraDiscoveryService() {
    stopHealthMonitoring();
    m_scanFuture.waitForFinished();
}

void AutoCameraDiscoveryService::startHealthMonitoring() {
    if (m_healthTimer->isActive()) { pollHealth(); return; }
    m_stopping.store(false);
    m_healthTimer->start();
    emit logGenerated(tr("[CAMERA-HEALTH] 后台连接检测已启动：在线设备每 %1 秒检查，离线设备每 %2 秒重试；必须收到有效 HTTP/RTSP 响应，检测独立于实时预览和视频缓冲帧。")
        .arg(CameraHealthSchedule::onlineIntervalMs / 1000).arg(CameraHealthSchedule::offlineIntervalMs / 1000));
    emit logGenerated(tr("[DISCOVERY] 新设备发现仅在点击刷新设备时执行；已登记设备继续维护连接状态。"));
    pollHealth();
}

bool AutoCameraDiscoveryService::knownOnlineState(const QString& cameraId, bool& online) const {
    const auto state=m_health.states.constFind(cameraId);
    if (state==m_health.states.cend() || !state->known) return false;
    online=state->online;
    return true;
}

void AutoCameraDiscoveryService::stopHealthMonitoring() {
    m_stopping.store(true);
    m_healthTimer->stop();
    const auto sockets = m_healthSockets;
    m_healthSockets.clear();
    for (auto socket : sockets) if (socket) { socket->disconnect(); socket->abort(); socket->deleteLater(); }
    for (auto& state : m_health.states) { ++state.revision; state.inFlight = false; }
}

void AutoCameraDiscoveryService::triggerScanNow() {
    if (m_stopping.load()) return;
    for (auto& state : m_health.states) {
        const auto now = m_clock.elapsed();
        if (state.captureBusyUntil <= now && (state.lastLiveAt < 0 || now - state.lastLiveAt >= 30000)) state.due = 0;
    }
    pollHealth();
    startDiscovery();
}

void AutoCameraDiscoveryService::startDiscovery() {
    {
        QMutexLocker locker(&m_mutex);
        if (m_stopping.load() || m_isScanning) return;
        m_isScanning = true;
    }
    emit logGenerated(tr("[AUTO-DISCOVERY] 手动刷新：发现新设备并分批检查已登记设备。"));
    m_scanFuture = QtConcurrent::run(&m_discoveryPool, [this]() {
        try { executeDiscoveryProcess(); }
        catch (const std::exception& error) {
            emit logGenerated(tr("[AUTO-DISCOVERY] 发现任务失败: %1").arg(QString::fromUtf8(error.what())));
            emit discoveryCompleted(0);
        }
        QMutexLocker locker(&m_mutex); m_isScanning = false;
    });
}

void AutoCameraDiscoveryService::publishHealth(const QString& cameraId, bool online, bool changed, const QString& details) {
    if (!changed || m_stopping.load()) return;
    emit cameraOnlineStateChanged(cameraId, online);
    const auto endpoint=m_health.states.value(cameraId).endpoint;
    emit logGenerated(tr("[CAMERA-HEALTH] 设备 %1 状态变为%2，检测结果：%3。")
        .arg(cameraId, online ? tr("在线") : tr("离线"), details.isEmpty()?endpoint:details));
}

void AutoCameraDiscoveryService::recordCaptureState(const QString& cameraId, bool online) {
    if (m_stopping.load() || !DeviceConfigManager::instance().hasDevice(cameraId)) return;
    const auto camera = DeviceConfigManager::instance().getDevice(cameraId);
    if (camera.protocolType == CameraProtocolType::LOCAL_FILE) return;
    m_health.configure(cameraId, cameraHealthEndpointKey(camera));
    const bool changed = m_health.update(cameraId, online, m_clock.elapsed(), true);
    if (auto socket = m_healthSockets.take(cameraId)) { socket->disconnect(); socket->abort(); socket->deleteLater(); }
    publishHealth(cameraId, online, changed);
    if (!changed && camera.isOnline != online) emit cameraOnlineStateChanged(cameraId, online);
}

void AutoCameraDiscoveryService::deferHealthForCapture(const QString& cameraId) {
    if (m_stopping.load() || !DeviceConfigManager::instance().hasDevice(cameraId)) return;
    const auto camera = DeviceConfigManager::instance().getDevice(cameraId);
    if (camera.protocolType == CameraProtocolType::LOCAL_FILE) return;
    m_health.configure(cameraId, cameraHealthEndpointKey(camera));
    if (!m_health.deferForCapture(cameraId, m_clock.elapsed())) return;
    if (auto socket = m_healthSockets.take(cameraId)) { socket->disconnect(); socket->abort(); socket->deleteLater(); }
}

void AutoCameraDiscoveryService::recordInspectionState(const QString& cameraId, const QString& errorCode, bool captured) {
    // Buffered frames must never cancel an independent probe or overwrite offline state.
    if (captured || m_stopping.load()) return;
    static const QSet<QString> captureFailures = {"OPEN_FAILED", "NOT_OPEN", "READ_FAILED", "NO_FRAME", "STALE_FRAME",
        "CAPTURE_TIMEOUT", "CAPTURE_WORKER_FAILED", "CAPTURE_EXCEPTION"};
    if (!captureFailures.contains(errorCode) || !DeviceConfigManager::instance().hasDevice(cameraId)) return;
    const auto camera=DeviceConfigManager::instance().getDevice(cameraId);
    if (camera.protocolType!=CameraProtocolType::ONVIF_RTSP_STREAM || cameraHealthEndpoints(camera).isEmpty()) return;
    const auto key=cameraHealthEndpointKey(camera);
    if (m_health.states.value(cameraId).endpoint!=key) m_captureFailureChecks.remove(cameraId);
    auto& state=m_health.configure(cameraId,key);
    // Already-offline devices retain the configured retry interval. Reuse an in-flight probe.
    if ((state.known && !state.online) || state.inFlight || m_healthSockets.contains(cameraId)) return;
    const auto now=m_clock.elapsed();
    if (now-m_captureFailureChecks.value(cameraId,-5000)<5000) return;
    m_captureFailureChecks[cameraId]=now;
    state.captureBusyUntil=0;
    state.due=0; // Bypass the normal interval and acquisition grace period.
    m_urgentHealthChecks.insert(cameraId);
    emit logGenerated(tr("[CAMERA-HEALTH] 设备 %1 采集失败（%2），立即复核连接：%3。")
        .arg(cameraId,errorCode,key));
    pollHealth();
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
    for (auto it=m_healthDiagnostics.begin();it!=m_healthDiagnostics.end();) {
        if (!ids.contains(it.key())) {
            m_healthDiagnosticTimes.remove(it.key()); m_captureFailureChecks.remove(it.key());
            m_urgentHealthChecks.remove(it.key()); it=m_healthDiagnostics.erase(it);
        }
        else ++it;
    }
    const auto oldSockets = m_healthSockets;
    for (auto it = oldSockets.begin(); it != oldSockets.end(); ++it) if (!ids.contains(it.key())) {
        m_healthSockets.remove(it.key());
        if (it.value()) { it.value()->disconnect(); it.value()->abort(); it.value()->deleteLater(); }
    }
    // Prioritize a capture-triggered check while preserving concurrency/rate limits.
    for (int index=0;index<cameras.size();++index) {
        if (m_urgentHealthChecks.contains(cameras[index].cameraId)) { m_healthCursor=index; break; }
    }
    int started = 0;
    for (int visited = 0; visited < cameras.size() && started < 32 && m_healthStarted < 32 && m_healthSockets.size() < 16; ++visited) {
        if (cameras.isEmpty()) break;
        const auto camera = cameras[m_healthCursor++ % cameras.size()];
        if (m_healthCursor >= cameras.size()) m_healthCursor = 0;
        if (camera.protocolType != CameraProtocolType::ONVIF_RTSP_STREAM) continue;
        const auto endpoints = cameraHealthEndpoints(camera);
        if (endpoints.isEmpty()) continue;
        auto& state = m_health.configure(camera.cameraId, cameraHealthEndpointKey(camera));
        if (state.inFlight || state.due > m_clock.elapsed() || m_healthSockets.contains(camera.cameraId)) continue;
        m_urgentHealthChecks.remove(camera.cameraId);
        const auto revision = m_health.begin(camera.cameraId);
        const auto probeSocket = std::make_shared<QPointer<QTcpSocket>>();
        const auto details = std::make_shared<QStringList>();
        auto* socket = startCameraHealthProbe(endpoints, this,
            [this, id = camera.cameraId, wasOnline = camera.isOnline, revision, probeSocket, details](bool online) {
                if (!*probeSocket || m_healthSockets.value(id) != *probeSocket) return;
                auto* completedSocket = m_healthSockets.take(id).data();
                if (!completedSocket) return;
                completedSocket->disconnect(); completedSocket->abort(); completedSocket->deleteLater();
                bool changed = false;
                if (m_health.complete(id, revision, online, m_clock.elapsed(), changed)) {
                    const auto diagnosis=details->join(QStringLiteral(u"；"));
                    const auto now=m_clock.elapsed();
                    const bool diagnosisChanged=m_healthDiagnostics.value(id)!=diagnosis;
                    const bool repeatDue=now-m_healthDiagnosticTimes.value(id,-30000)>=30000;
                    publishHealth(id, online, changed, diagnosis);
                    if (!changed && !diagnosis.isEmpty() && (diagnosisChanged || (!online && repeatDue)))
                        emit logGenerated(tr("[CAMERA-HEALTH] 设备 %1 检测%2：%3。")
                            .arg(id,online?tr("在线"):tr("离线"),diagnosis));
                    if (changed || diagnosisChanged || (!online && repeatDue)) m_healthDiagnosticTimes[id]=now;
                    m_healthDiagnostics[id]=diagnosis;
                    if (!changed && wasOnline != online && !m_stopping.load()) emit cameraOnlineStateChanged(id, online);
                }
            }, 2000, [details](const CameraHealthAttempt& attempt) { details->append(attempt.description()); });
        *probeSocket = socket;
        m_healthSockets.insert(camera.cameraId, socket); ++started; ++m_healthStarted;
    }
}

void AutoCameraDiscoveryService::executeDiscoveryProcess() {
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
        item.defaultProfileId.clear();
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
    if (newFound == 0) {
        emit logGenerated(tr("[AUTO-DISCOVERY] Scan completed. No real cameras responded on the local network (Total: 0)."));
    } else {
        emit logGenerated(tr("[AUTO-DISCOVERY] Scan completed. Active real cameras updated. Total: %1.").arg(newFound));
    }
}
