#include "AutoCameraDiscoveryService.hpp"
#include <QtConcurrent/QtConcurrent>
#include <QTcpSocket>
#include <QNetworkInterface>
#include <QSet>
#include <QDebug>
#include <set>
#include "DeviceConfigManager.hpp"
#include <QUrl>
#include "CameraDiscoveryTargets.hpp"
#include <QFile>
#include <QJsonDocument>

AutoCameraDiscoveryService::AutoCameraDiscoveryService(QObject *parent)
    : QObject(parent) {
    qRegisterMetaType<DiscoveredCameraItem>("DiscoveredCameraItem");
    
    m_scanTimer = new QTimer(this);
    connect(m_scanTimer, &QTimer::timeout, this, &AutoCameraDiscoveryService::onScanTimerTimeout);
}

AutoCameraDiscoveryService::~AutoCameraDiscoveryService() {
    stopAutoScan();
    m_scanFuture.waitForFinished();
}

void AutoCameraDiscoveryService::startAutoScan(int intervalMs) {
    emit logGenerated(tr("[AUTO-DISCOVERY] Starting periodic network camera auto-discovery (Interval: %1 ms)...").arg(intervalMs));
    m_scanTimer->start(intervalMs);
    triggerScanNow();
}

void AutoCameraDiscoveryService::stopAutoScan() {
    if (m_scanTimer && m_scanTimer->isActive()) {
        m_scanTimer->stop();
        emit logGenerated(tr("[AUTO-DISCOVERY] Network camera auto-discovery stopped."));
    }
}

void AutoCameraDiscoveryService::onScanTimerTimeout() {
    triggerScanNow();
}

void AutoCameraDiscoveryService::triggerScanNow() {
    {
        QMutexLocker locker(&m_mutex);
        if (m_isScanning) return;
        m_isScanning = true;
    }

    emit logGenerated(tr("[AUTO-DISCOVERY] 正在执行 ONVIF 发现与已登记设备连通性检查..."));

    m_scanFuture = QtConcurrent::run([this]() {
        executeDiscoveryProcess();

        QMutexLocker locker(&m_mutex);
        m_isScanning = false;
    });
}

void AutoCameraDiscoveryService::executeDiscoveryProcess() {
    const auto knownCameras = DeviceConfigManager::instance().getAllDevices();
    // Probe saved addresses too: routed cameras need not answer LAN discovery.
    for (const auto& camera : knownCameras) {
        if (camera.protocolType != CameraProtocolType::ONVIF_RTSP_STREAM) continue;
        const QUrl uri(camera.rtspUrl);
        const QString host = uri.host().isEmpty() ? camera.ipOrSn : uri.host();
        const int port = camera.rtspUrl.isEmpty() ? camera.port : uri.port(camera.rtspPort > 0 ? camera.rtspPort : 554);
        QTcpSocket socket;
        if (!host.isEmpty()) socket.connectToHost(host, static_cast<quint16>(port));
        const bool reachable = !host.isEmpty() && socket.waitForConnected(350);
        socket.abort();
        emit cameraOnlineStateChanged(camera.cameraId, reachable);
    }
    // 1. Real ONVIF WS-Discovery UDP Socket Probe
    emit logGenerated(tr("[AUTO-DISCOVERY] [ONVIF Phase] Broadcasting Probe to 239.255.255.250:3702 on local net interfaces..."));
    auto onvifList = ONVIFDeviceManager::probeDevicesWSDiscovery(1500);
    
    int newFound = 0;
    std::set<std::string> foundIps;

    for (const auto& dev : onvifList) {
        DiscoveredCameraItem item;
        item.cameraId = QString("CAM_ONVIF_%1").arg(QString::fromStdString(dev.ipAddress).replace('.', '_'));
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

        emit cameraDiscovered(item);
        emit logGenerated(tr("[AUTO-DISCOVERY] Found Real ONVIF Device: %1 (%2).").arg(item.cameraId).arg(item.modelName));
        newFound++;
        foundIps.insert(dev.ipAddress);
    }

    // 2. Direct TCP Port Reachability Probe (for Xiongmai 34567 / RTSP 554 / HTTP 80)
    emit logGenerated(tr("[AUTO-DISCOVERY] [TCP Probe Phase] Probing candidate camera IPs on ports 554/34567/80..."));
    
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

    for (const QString& candIp : candidateIpSet) {
        if (foundIps.find(candIp.toStdString()) != foundIps.end()) continue;

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
                socket.abort(); // Immediately send TCP RST to free Xiongmai device socket slot
                break;
            }
            socket.abort();
        }

        if (isReachable) {
            DiscoveredCameraItem item;
            item.cameraId = QString("CAM_IP_%1").arg(QString(candIp).replace('.', '_'));
            item.customName = QString("%1 网络相机 (%2)").arg(hitPort == 34567 ? "雄迈" : "标准").arg(candIp);
            item.ipOrSn = candIp;
            item.protocolType = (hitPort == 34567 ? CameraProtocolType::SDK_JLINK : CameraProtocolType::ONVIF_RTSP_STREAM);
            item.protocolName = (hitPort == 34567 ? "Xiongmai JLink Camera" : "IP Camera (RTSP)");
            item.modelName = (hitPort == 34567 ? "JFTech Xiongmai Camera" : "Network Camera");
            item.port = 80;
            item.rtspPort = hitPort;
            item.rtspUrl = QString(); // Configure the actual vendor path and credentials.
            item.username = "";
            item.password = "";
            item.isOnline = true;
            item.defaultProfileId = "PRESSURE_GAUGE_1.6MPA";
            item.lastSeenTime = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");

            emit cameraDiscovered(item);
            emit logGenerated(tr("[AUTO-DISCOVERY] Found Active Camera at %1 (RTSP Port %2).").arg(candIp).arg(hitPort));
            newFound++;
            foundIps.insert(candIp.toStdString());
        }
    }

    emit discoveryCompleted(newFound);
    if (newFound == 0) {
        emit logGenerated(tr("[AUTO-DISCOVERY] Scan completed. No real cameras responded on the local network (Total: 0)."));
    } else {
        emit logGenerated(tr("[AUTO-DISCOVERY] Scan completed. Active real cameras updated. Total: %1.").arg(newFound));
    }
}
