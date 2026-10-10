#include "DeviceConfigManager.hpp"
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDateTime>
#include <QCoreApplication>
#include <QDebug>

static QString protocolToString(CameraProtocolType type) {
    switch (type) {
        case CameraProtocolType::LOCAL_FILE: return "LOCAL_FILE";
        case CameraProtocolType::ONVIF_RTSP_STREAM: return "ONVIF_RTSP_STREAM";
        case CameraProtocolType::SDK_JLINK: return "SDK_JLINK";
        case CameraProtocolType::GB28181_PS_STREAM: return "GB28181_PS_STREAM";
        case CameraProtocolType::CUSTOM_SDK: return "CUSTOM_SDK";
        default: return "LOCAL_FILE";
    }
}

static CameraProtocolType stringToProtocol(const QString& str) {
    QString s = str.trimmed().toUpper();
    if (s == "ONVIF_RTSP_STREAM" || s == "ONVIF") return CameraProtocolType::ONVIF_RTSP_STREAM;
    if (s == "SDK_JLINK" || s == "JLINK" || s == "XIONGMAI") return CameraProtocolType::SDK_JLINK;
    if (s == "GB28181_PS_STREAM" || s == "GB28181") return CameraProtocolType::GB28181_PS_STREAM;
    if (s == "CUSTOM_SDK") return CameraProtocolType::CUSTOM_SDK;
    return CameraProtocolType::LOCAL_FILE;
}

DeviceConfigManager& DeviceConfigManager::instance() {
    static DeviceConfigManager inst;
    return inst;
}

DeviceConfigManager::DeviceConfigManager() {
    m_currentFilePath = resolveConfigPath();
}

QString DeviceConfigManager::resolveConfigPath(const QString& preferredPath) {
    if (!preferredPath.isEmpty()) {
        if (QFileInfo::exists(preferredPath)) return preferredPath;
    }

    QString appDir = QCoreApplication::applicationDirPath();
    QStringList candidates = {
        appDir + "/config/devices.json",
        "config/devices.json",
        "../config/devices.json",
        appDir + "/devices.json",
        "devices.json"
    };

    for (const auto& cand : candidates) {
        if (QFileInfo::exists(cand)) {
            return cand;
        }
    }

    // Default to app local config directory
    QString defaultTarget = appDir + "/config/devices.json";
    QDir().mkpath(QFileInfo(defaultTarget).absolutePath());
    return defaultTarget;
}

void DeviceConfigManager::populateDefaultsIfEmpty() {
    if (!m_devices.isEmpty()) return;

    struct PresetDevice {
        QString id;
        QString name;
        QString ip;
        CameraProtocolType proto;
        QString protoName;
        QString model;
        int port;
        int rtspPort;
        QString rtspUrl;
        QString profile;
        bool isAuto;
    };

    PresetDevice presets[] = {
        {"CAM_5-3-1", "主变1号油压表监控", "192.168.1.101", CameraProtocolType::LOCAL_FILE, "智能图像采集终端", "Pressure Gauge Sensor Node", 80, 554, "", "PRESSURE_GAUGE_1.6MPA", true},
        {"CAM_5-1-2", "2号变压器油温表监控", "192.168.1.102", CameraProtocolType::LOCAL_FILE, "智能图像采集终端", "Pressure Gauge Sensor Node", 80, 554, "", "PRESSURE_GAUGE_1.6MPA", false},
        {"CAM_5-3-4", "高压进线柜气压表监控", "192.168.1.103", CameraProtocolType::LOCAL_FILE, "智能图像采集终端", "Pressure Gauge Sensor Node", 80, 554, "", "PRESSURE_GAUGE_1.6MPA", false},
        {"CAM_5-4-1", "GIS开关气压表监控", "192.168.1.104", CameraProtocolType::LOCAL_FILE, "智能图像采集终端", "Pressure Gauge Sensor Node", 80, 554, "", "PRESSURE_GAUGE_1.6MPA", false},
        {"CAM_20", "20号变电柜备用监测节点", "192.168.1.120", CameraProtocolType::LOCAL_FILE, "智能图像采集终端", "Pressure Gauge Sensor Node", 80, 554, "", "PRESSURE_GAUGE_1.6MPA", false}
    };

    for (const auto& p : presets) {
        DiscoveredCameraItem item;
        item.cameraId = p.id;
        item.customName = p.name;
        item.ipOrSn = p.ip;
        item.protocolType = p.proto;
        item.protocolName = p.protoName;
        item.modelName = p.model;
        item.port = p.port;
        item.rtspPort = p.rtspPort;
        item.rtspUrl = p.rtspUrl;
        item.username = "";
        item.password = "";
        item.defaultProfileId = p.profile;
        item.isAutoConnect = p.isAuto;
        item.isOnline = true;
        item.lastSeenTime = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
        m_devices.insert(item.cameraId, item);
    }

    m_lastActiveDeviceId = "CAM_5-3-1";
    m_autoConnectOnStartup = true;
}

bool DeviceConfigManager::load(const QString& configPath) {
    QMutexLocker locker(&m_mutex);

    QString path = configPath.isEmpty() ? m_currentFilePath : configPath;
    path = resolveConfigPath(path);
    m_currentFilePath = path;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        populateDefaultsIfEmpty();
        locker.unlock();
        save(path);
        return true;
    }

    QByteArray data = file.readAll();
    file.close();

    QJsonDocument doc = QJsonDocument::fromJson(data);
    if (!doc.isObject()) {
        populateDefaultsIfEmpty();
        return false;
    }

    m_devices.clear();
    QJsonObject root = doc.object();

    if (root.contains("system")) {
        QJsonObject sys = root["system"].toObject();
        m_autoConnectOnStartup = sys.value("auto_connect_on_startup").toBool(true);
        m_lastActiveDeviceId = sys.value("last_active_device_id").toString();
    }

    QJsonArray devArr = root.value("devices").toArray();
    for (const auto& v : devArr) {
        if (!v.isObject()) continue;
        QJsonObject obj = v.toObject();

        DiscoveredCameraItem item;
        item.cameraId = obj.value("camera_id").toString();
        if (item.cameraId.isEmpty()) continue;

        item.customName = obj.value("custom_name").toString();
        item.ipOrSn = obj.value("ip_or_sn").toString();
        item.protocolType = stringToProtocol(obj.value("protocol_type").toString());
        item.protocolName = obj.value("protocol_name").toString("IP Camera");
        item.modelName = obj.value("model_name").toString("Network Camera");
        item.port = obj.value("port").toInt(80);
        item.rtspPort = obj.value("rtsp_port").toInt(554);
        item.rtspUrl = obj.value("rtsp_url").toString();
        item.username = obj.value("username").toString("");
        item.password = obj.value("password").toString("");
        item.defaultProfileId = obj.value("default_profile_id").toString("PRESSURE_GAUGE_1.6MPA");
        item.isAutoConnect = obj.value("is_auto_connect").toBool(false);
        item.isOnline = obj.value("is_online").toBool(true);
        item.lastSeenTime = obj.value("last_seen_time").toString();

        // Asset & Physical Installation Metadata (no fake defaults, strictly read from configuration)
        item.substation = obj.value("substation").toString("").trimmed();
        item.area = obj.value("area").toString("").trimmed();
        item.installationLocation = obj.value("installation_location").toString(obj.value("location").toString("")).trimmed();
        item.deviceTag = obj.value("device_tag").toString("").trimmed();
        item.gaugeType = obj.value("gauge_type").toString("").trimmed();
        item.manufacturer = obj.value("manufacturer").toString("").trimmed();
        item.accuracyClass = obj.value("accuracy_class").toString("").trimmed();

        // Load Calibration Parameters (supports direct fields or calibration sub-object)
        QJsonObject calObj = obj.contains("calibration") ? obj["calibration"].toObject() : obj;
        item.perspectiveEnabled = calObj.value("perspective_correction_enabled").toBool(true);
        item.calibratedWidth = calObj.value("calibrated_width").toInt(800);
        item.calibratedHeight = calObj.value("calibrated_height").toInt(448);
        item.pt0_x = calObj.value("pt_0_x").toDouble(404.444);
        item.pt0_y = calObj.value("pt_0_y").toDouble(19.2593);
        item.pt1_x = calObj.value("pt_1_x").toDouble(644.444);
        item.pt1_y = calObj.value("pt_1_y").toDouble(306.667);
        item.pt2_x = calObj.value("pt_2_x").toDouble(392.593);
        item.pt2_y = calObj.value("pt_2_y").toDouble(408.889);
        item.pt3_x = calObj.value("pt_3_x").toDouble(139.259);
        item.pt3_y = calObj.value("pt_3_y").toDouble(339.259);
        item.centerX = calObj.value("center_x").toDouble(407.407);
        item.centerY = calObj.value("center_y").toDouble(327.407);
        item.pointerBaseRadius = calObj.value("pointer_base_radius").toDouble(calObj.value("radius").toDouble(47.0));

        m_devices.insert(item.cameraId, item);
    }

    if (m_devices.isEmpty()) {
        populateDefaultsIfEmpty();
    }

    if (m_lastActiveDeviceId.isEmpty() || !m_devices.contains(m_lastActiveDeviceId)) {
        for (const auto& dev : m_devices) {
            if (dev.isAutoConnect) {
                m_lastActiveDeviceId = dev.cameraId;
                break;
            }
        }
        if (m_lastActiveDeviceId.isEmpty() && !m_devices.isEmpty()) {
            m_lastActiveDeviceId = m_devices.firstKey();
        }
    }

    return true;
}

bool DeviceConfigManager::save(const QString& configPath) {
    QMutexLocker locker(&m_mutex);

    QString path = configPath.isEmpty() ? m_currentFilePath : configPath;
    if (path.isEmpty()) {
        path = resolveConfigPath();
    }
    m_currentFilePath = path;

    QDir().mkpath(QFileInfo(path).absolutePath());

    QJsonObject root;
    QJsonObject sys;
    sys["auto_connect_on_startup"] = m_autoConnectOnStartup;
    sys["last_active_device_id"] = m_lastActiveDeviceId;
    sys["updated_at"] = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
    root["system"] = sys;

    QJsonArray devArr;
    for (const auto& item : m_devices) {
        QJsonObject obj;
        obj["camera_id"] = item.cameraId;
        obj["custom_name"] = item.customName;
        obj["ip_or_sn"] = item.ipOrSn;
        obj["protocol_type"] = protocolToString(item.protocolType);
        obj["protocol_name"] = item.protocolName;
        obj["model_name"] = item.modelName;
        obj["port"] = item.port;
        obj["rtsp_port"] = item.rtspPort;
        obj["rtsp_url"] = item.rtspUrl;
        obj["username"] = item.username;
        obj["password"] = item.password;
        obj["default_profile_id"] = item.defaultProfileId;
        obj["is_auto_connect"] = item.isAutoConnect;
        obj["is_online"] = item.isOnline;
        obj["last_seen_time"] = item.lastSeenTime;

        // Asset & Physical Installation Metadata
        obj["substation"] = item.substation;
        obj["area"] = item.area;
        obj["installation_location"] = item.installationLocation;
        obj["device_tag"] = item.deviceTag;
        obj["gauge_type"] = item.gaugeType;
        obj["manufacturer"] = item.manufacturer;
        obj["accuracy_class"] = item.accuracyClass;

        // Calibration Sub-Object
        QJsonObject calObj;
        calObj["perspective_correction_enabled"] = item.perspectiveEnabled;
        calObj["calibrated_width"] = item.calibratedWidth;
        calObj["calibrated_height"] = item.calibratedHeight;
        calObj["pt_0_x"] = item.pt0_x;
        calObj["pt_0_y"] = item.pt0_y;
        calObj["pt_1_x"] = item.pt1_x;
        calObj["pt_1_y"] = item.pt1_y;
        calObj["pt_2_x"] = item.pt2_x;
        calObj["pt_2_y"] = item.pt2_y;
        calObj["pt_3_x"] = item.pt3_x;
        calObj["pt_3_y"] = item.pt3_y;
        calObj["center_x"] = item.centerX;
        calObj["center_y"] = item.centerY;
        calObj["pointer_base_radius"] = item.pointerBaseRadius;
        obj["calibration"] = calObj;

        devArr.append(obj);
    }
    root["devices"] = devArr;

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        qWarning() << "[DeviceConfigManager] Failed to write device registry to:" << path;
        return false;
    }

    QJsonDocument doc(root);
    file.write(doc.toJson(QJsonDocument::Indented));
    file.close();

    // Synchronize to workspace devices.json copies if running from build directory
    QStringList syncCandidates = {
        "config/devices.json",
        "../config/devices.json",
        "../../config/devices.json"
    };
    for (const auto& altPath : syncCandidates) {
        if (QFileInfo::exists(altPath)) {
            QString can1 = QFileInfo(path).canonicalFilePath();
            QString can2 = QFileInfo(altPath).canonicalFilePath();
            if (can1.isEmpty() || can2.isEmpty() || can1 != can2) {
                QFile altFile(altPath);
                if (altFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                    altFile.write(doc.toJson(QJsonDocument::Indented));
                    altFile.close();
                }
            }
        }
    }

    return true;
}

QList<DiscoveredCameraItem> DeviceConfigManager::getAllDevices() const {
    QMutexLocker locker(&m_mutex);
    return m_devices.values();
}

DiscoveredCameraItem DeviceConfigManager::getDevice(const QString& cameraId) const {
    QMutexLocker locker(&m_mutex);
    return m_devices.value(cameraId);
}

bool DeviceConfigManager::hasDevice(const QString& cameraId) const {
    QMutexLocker locker(&m_mutex);
    return m_devices.contains(cameraId);
}

void DeviceConfigManager::addOrUpdateDevice(const DiscoveredCameraItem& item, bool autoSave) {
    {
        QMutexLocker locker(&m_mutex);
        if (m_devices.contains(item.cameraId)) {
            // Merge existing config (preserve user customName, credentials, autoConnect flag)
            auto& existing = m_devices[item.cameraId];
            if (!item.customName.isEmpty()) existing.customName = item.customName;
            if (!item.ipOrSn.isEmpty()) existing.ipOrSn = item.ipOrSn;
            if (item.protocolType != CameraProtocolType::LOCAL_FILE || existing.protocolType == CameraProtocolType::LOCAL_FILE) {
                existing.protocolType = item.protocolType;
                existing.protocolName = item.protocolName;
            }
            if (!item.modelName.isEmpty()) existing.modelName = item.modelName;
            if (item.port > 0) existing.port = item.port;
            if (item.rtspPort > 0) existing.rtspPort = item.rtspPort;
            if (!item.rtspUrl.isEmpty()) existing.rtspUrl = item.rtspUrl;
            if (!item.username.isEmpty()) existing.username = item.username;
            if (!item.password.isEmpty()) existing.password = item.password;
            if (!item.defaultProfileId.isEmpty()) existing.defaultProfileId = item.defaultProfileId;
            existing.isAutoConnect = item.isAutoConnect;
            existing.isOnline = item.isOnline;
            existing.lastSeenTime = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");

            // Update Asset & Physical Installation Metadata
            existing.substation = item.substation;
            existing.area = item.area;
            existing.installationLocation = item.installationLocation;
            existing.deviceTag = item.deviceTag;
            existing.gaugeType = item.gaugeType;
            existing.manufacturer = item.manufacturer;
            existing.accuracyClass = item.accuracyClass;
        } else {
            DiscoveredCameraItem newItem = item;
            if (newItem.lastSeenTime.isEmpty()) {
                newItem.lastSeenTime = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
            }
            m_devices.insert(newItem.cameraId, newItem);
        }
    }

    if (autoSave) {
        save();
    }
}

bool DeviceConfigManager::removeDevice(const QString& cameraId, bool autoSave) {
    bool removed = false;
    {
        QMutexLocker locker(&m_mutex);
        removed = (m_devices.remove(cameraId) > 0);
        if (m_lastActiveDeviceId == cameraId) {
            m_lastActiveDeviceId = m_devices.isEmpty() ? "" : m_devices.firstKey();
        }
    }
    if (removed && autoSave) {
        save();
    }
    return removed;
}

QString DeviceConfigManager::getLastActiveDeviceId() const {
    QMutexLocker locker(&m_mutex);
    return m_lastActiveDeviceId;
}

void DeviceConfigManager::setLastActiveDeviceId(const QString& id, bool autoSave) {
    {
        QMutexLocker locker(&m_mutex);
        m_lastActiveDeviceId = id;
    }
    if (autoSave) {
        save();
    }
}

bool DeviceConfigManager::isAutoConnectEnabled() const {
    QMutexLocker locker(&m_mutex);
    return m_autoConnectOnStartup;
}

void DeviceConfigManager::setAutoConnectEnabled(bool enable, bool autoSave) {
    {
        QMutexLocker locker(&m_mutex);
        m_autoConnectOnStartup = enable;
    }
    if (autoSave) {
        save();
    }
}

void DeviceConfigManager::setDeviceAutoConnect(const QString& cameraId, bool isAuto, bool autoSave) {
    {
        QMutexLocker locker(&m_mutex);
        if (m_devices.contains(cameraId)) {
            if (isAuto) {
                // Clear other auto-connect flags if only one default is desired
                for (auto& dev : m_devices) {
                    dev.isAutoConnect = false;
                }
                m_lastActiveDeviceId = cameraId;
            }
            m_devices[cameraId].isAutoConnect = isAuto;
        }
    }
    if (autoSave) {
        save();
    }
}

void DeviceConfigManager::markDeviceOnline(const QString& cameraId, bool online) {
    QMutexLocker locker(&m_mutex);
    if (m_devices.contains(cameraId)) {
        m_devices[cameraId].isOnline = online;
        if (online) {
            m_devices[cameraId].lastSeenTime = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
        }
    }
}

void DeviceConfigManager::updateDeviceCalibration(const QString& cameraId,
                                                   bool perspectiveEnabled,
                                                   int calibW, int calibH,
                                                   double p0x, double p0y,
                                                   double p1x, double p1y,
                                                   double p2x, double p2y,
                                                   double p3x, double p3y,
                                                   double cx, double cy,
                                                   double baseRadius,
                                                   bool autoSave) {
    bool found = false;
    {
        QMutexLocker locker(&m_mutex);
        if (m_devices.contains(cameraId)) {
            auto& item = m_devices[cameraId];
            item.perspectiveEnabled = perspectiveEnabled;
            item.calibratedWidth = calibW;
            item.calibratedHeight = calibH;
            item.pt0_x = p0x; item.pt0_y = p0y;
            item.pt1_x = p1x; item.pt1_y = p1y;
            item.pt2_x = p2x; item.pt2_y = p2y;
            item.pt3_x = p3x; item.pt3_y = p3y;
            item.centerX = cx; item.centerY = cy;
            item.pointerBaseRadius = baseRadius;
            found = true;
        }
    }
    if (found && autoSave) {
        save();
    }
}
