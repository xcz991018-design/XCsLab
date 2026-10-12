#include "DeviceConfigManager.hpp"
#include "DeviceCodec.hpp"
#include "MySqlRepository.hpp"
#include <QtConcurrent/QtConcurrent>
#include <QSaveFile>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDateTime>
#include <QCoreApplication>
#include <QDebug>
#include <QUrl>

DeviceConfigManager& DeviceConfigManager::instance() {
    static DeviceConfigManager inst;
    return inst;
}

DeviceConfigManager::DeviceConfigManager() {
    m_databasePool.setMaxThreadCount(1);
    m_currentFilePath = resolveConfigPath();
}

DeviceConfigManager::~DeviceConfigManager() { waitForDatabaseWrites(); }
void DeviceConfigManager::waitForDatabaseWrites() { m_databasePool.waitForDone(); }
QString DeviceConfigManager::lastError() const { QMutexLocker lock(&m_mutex); return m_lastError; }
void DeviceConfigManager::retryDatabaseSync() {
    if (!DatabaseSettings::fromEnvironment().enabled || m_databaseFuture.isRunning()) return;
    m_databaseFuture = QtConcurrent::run(&m_databasePool, [this]() {
        const auto error = MySqlRepository::retryPending(QString());
        { QMutexLocker lock(&m_mutex); m_lastError = error; }
        emit databaseStatus(error);
    });
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
    const bool hasLocal = file.open(QIODevice::ReadOnly);
    const auto localData = hasLocal ? file.readAll() : QByteArray();
    file.close();
    QByteArray data = localData;
    if (DatabaseSettings::fromEnvironment().enabled) {
        try {
            // Do not overwrite a newer local pending archive with an older DB snapshot.
            const auto syncError = MySqlRepository::retryPending(QString());
            if (!syncError.isEmpty()) throw std::runtime_error(syncError.toUtf8().constData());
            MySqlRepository repository;
            auto root = repository.loadRegistry();
            const bool neverInitialized = !root.contains("system");
            if (neverInitialized) {
                auto local = QJsonDocument::fromJson(localData);
                if (!local.isObject()) throw std::runtime_error("MYSQL_EMPTY_REGISTRY_REQUIRES_EXPLICIT_LOCAL_IMPORT");
                repository.saveRegistry(MySqlRepository::sanitizedRegistry(local.object()));
                root = repository.loadRegistry();
            }
            // Passwords stay in the existing local credential file, matched by camera ID.
            QMap<QString, QJsonObject> credentials;
            for (const auto& value : QJsonDocument::fromJson(localData).object()["devices"].toArray()) {
                auto device = value.toObject(); credentials.insert(device["camera_id"].toString(), device);
            }
            QJsonArray devices;
            for (const auto& value : root["devices"].toArray()) {
                auto device = value.toObject(); const auto local = credentials.value(device["camera_id"].toString());
                device["password"] = local["password"];
                const QUrl localUri(local["rtsp_url"].toString());
                if (!localUri.userName().isEmpty()) device["username"] = localUri.userName();
                if (!localUri.password().isEmpty()) device["password"] = localUri.password();
                devices.append(device);
            }
            root["devices"] = devices;
            data = QJsonDocument(root).toJson();
            m_lastError.clear();
        } catch (const std::exception& e) {
            m_lastError = QString::fromUtf8(e.what());
            return false; // Never manufacture demo devices after a DB failure.
        }
    } else if (!hasLocal) {
        populateDefaultsIfEmpty();
        locker.unlock();
        save(path);
        return true;
    }

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

        auto item = deviceFromJson(obj);
        if (item.cameraId.isEmpty()) continue;

        m_devices.insert(item.cameraId, item);
    }

    if (m_devices.isEmpty() && !DatabaseSettings::fromEnvironment().enabled) {
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
        QJsonObject obj = deviceToJson(item, true);
        devArr.append(obj);
    }
    root["devices"] = devArr;

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        qWarning() << "[DeviceConfigManager] Failed to write device registry to:" << path;
        return false;
    }

    QJsonDocument doc(root);
    const auto bytes = doc.toJson(QJsonDocument::Indented);
    if (file.write(bytes) != bytes.size() || !file.commit()) return false;
    if (DatabaseSettings::fromEnvironment().enabled) {
        try {
            MySqlRepository::queueRegistry(root);
        } catch (const std::exception& e) {
            m_lastError = QString::fromUtf8(e.what());
            emit databaseStatus(m_lastError);
            return false;
        }
        locker.unlock();
        retryDatabaseSync();
        // Database mode uses this one file as the local credential cache only.
        return true;
    }

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
