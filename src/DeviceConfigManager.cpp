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
#include <QHostAddress>
#include <QStandardPaths>
#include "CameraIdentity.hpp"
#include "DeviceCredentialStore.hpp"

namespace {
bool automaticId(const QString& id) {
    return id.startsWith("CAM_ONVIF_") || id.startsWith("CAM_IP_") || id.startsWith("CAM_UUID_");
}
bool mergeDuplicateDevices(QJsonObject& root) {
    QJsonArray retained;
    auto system = root["system"].toObject();
    bool changed = false;
    for (const auto& value : root["devices"].toArray()) {
        auto incoming = value.toObject();
        const auto identity = CameraIdentity::normalize(incoming["hardware_id"].toString());
        int match = -1;
        for (int i = 0; !identity.isEmpty() && i < retained.size(); ++i) {
            const auto existing = retained[i].toObject();
            if (CameraIdentity::normalize(existing["hardware_id"].toString()) == identity) {
                match = i; break;
            }
        }
        if (match < 0) { retained.append(incoming); continue; }
        auto existing = retained[match].toObject();
        // Prefer a user-created or calibrated archive over a discovery placeholder.
        const auto score = [&](const QJsonObject& device) {
            int result = automaticId(device["camera_id"].toString()) ? 0 : 1000;
            if (deviceFromJson(device).hasCalibration) result += 100;
            if (!device["password"].toString().isEmpty()) result += 1;
            return result;
        };
        if (score(incoming) > score(existing)) { auto temp = existing; existing = incoming; incoming = temp; }
        for (auto it = incoming.begin(); it != incoming.end(); ++it) {
            if (!existing.contains(it.key()) || existing[it.key()].isNull() ||
                (existing[it.key()].isString() && existing[it.key()].toString().isEmpty()))
                existing[it.key()] = it.value();
        }
        retained[match] = existing;
        changed = true;
    }
    if (changed) { root["devices"] = retained; root["system"] = system; }
    return changed;
}
// Retire only the original demo records, never cameras converted to a real protocol.
bool removeLegacyDemoDevices(QJsonObject& root) {
    const QStringList demoIds = {"CAM_20", "CAM_5-1-2", "CAM_5-3-1", "CAM_5-3-4", "CAM_5-4-1"};
    QJsonArray retained;
    bool removed = false;
    auto system = root["system"].toObject();
    for (const auto& value : root["devices"].toArray()) {
        const auto device = value.toObject();
        if (demoIds.contains(device["camera_id"].toString()) &&
            device["protocol_type"].toString() == "LOCAL_FILE") {
            removed = true;
        } else retained.append(value);
    }
    if (removed) {
        root["devices"] = retained;
        root["system"] = system;
    }
    return removed;
}
}

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
bool DeviceConfigManager::hasPendingRegistrySave() const { QMutexLocker lock(&m_mutex); return m_registrySavePending; }
void DeviceConfigManager::retryDatabaseSync() {
    if (!DatabaseSettings::fromEnvironment().enabled || m_databaseFuture.isRunning()) return;
    m_databaseFuture = QtConcurrent::run(&m_databasePool, [this]() {
        const auto error = MySqlRepository::retryPending(QString());
        // A background success must not erase an unsaved local-file failure.
        { QMutexLocker lock(&m_mutex); if (!m_registrySavePending) m_lastError = error; }
        emit databaseStatus(error);
    });
}

QString DeviceConfigManager::resolveConfigPath(const QString& preferredPath) {
    if (!preferredPath.isEmpty()) {
        return QFileInfo(preferredPath).absoluteFilePath();
    }

    return DeviceCredentialStore::defaultPath();
}

bool DeviceConfigManager::load(const QString& configPath) {
    QMutexLocker locker(&m_mutex);

    QString path = configPath.isEmpty() ? m_currentFilePath : configPath;
    path = resolveConfigPath(path);
    m_currentFilePath = path;
    if (path == DeviceCredentialStore::defaultPath() &&
        !DeviceCredentialStore::migrate(DeviceCredentialStore::legacyPaths(), {}, m_lastError)) return false;

    QFile file(path);
    const bool hasLocal = file.open(QIODevice::ReadOnly);
    const auto localData = hasLocal ? file.readAll() : QByteArray();
    file.close();
    QByteArray data = localData;
    bool migrated = false;
    if (DatabaseSettings::fromEnvironment().enabled) {
        try {
            // Do not overwrite a newer local pending archive with an older DB snapshot.
            const auto syncError = MySqlRepository::retryPending(QString());
            if (!syncError.isEmpty()) throw std::runtime_error(syncError.toUtf8().constData());
            MySqlRepository repository;
            auto root = repository.loadRegistry();
            const bool neverInitialized = !root.contains("system");
            if (neverInitialized) {
                // Start from the DB records; do not import stale demo/local devices implicitly.
                root["system"] = QJsonObject{};
                repository.saveRegistry(MySqlRepository::sanitizedRegistry(root));
                root = repository.loadRegistry();
            }
            // Match local secrets by archive key or a unique verified hardware UUID, never by IP.
            const auto localDevices = QJsonDocument::fromJson(localData).object()["devices"].toArray();
            QJsonArray devices;
            for (const auto& value : root["devices"].toArray())
                devices.append(DeviceCredentialStore::withCredentials(value.toObject(),localDevices));
            root["devices"] = devices;
            migrated = removeLegacyDemoDevices(root);
            migrated = mergeDuplicateDevices(root) || migrated;
            if (migrated) repository.saveRegistry(MySqlRepository::sanitizedRegistry(root));
            data = QJsonDocument(root).toJson();
            m_lastError.clear();
        } catch (const std::exception& e) {
            m_lastError = QString::fromUtf8(e.what());
            return false; // Never manufacture demo devices after a DB failure.
        }
    } else if (!hasLocal) {
        m_devices.clear();
        locker.unlock();
        return save(path);
    }

    QJsonDocument doc = QJsonDocument::fromJson(data);
    if (!doc.isObject()) {
        return false;
    }

    m_devices.clear();
    QJsonObject root = doc.object();
    migrated = removeLegacyDemoDevices(root) || migrated;
    migrated = mergeDuplicateDevices(root) || migrated;

    QJsonArray devArr = root.value("devices").toArray();
    for (const auto& v : devArr) {
        if (!v.isObject()) continue;
        QJsonObject obj = v.toObject();

        auto item = deviceFromJson(obj);
        if (item.cameraId.isEmpty()) continue;
        item.isOnline = false; // Current connectivity must be checked after restoring the archive.

        if (obj["safe_working_range"].toObject()["defaults_version"].toInt() < 2) migrated = true;
        m_devices.insert(item.cameraId, item);
    }

    if (migrated) {
        locker.unlock();
        return save(path);
    }
    m_lastError.clear();
    return true;
}

bool DeviceConfigManager::save(const QString& configPath) {
    QMutexLocker locker(&m_mutex);
    m_registrySavePending = true;

    QString path = configPath.isEmpty() ? m_currentFilePath : configPath;
    if (path.isEmpty()) {
        path = resolveConfigPath();
    }
    path = QFileInfo(path).absoluteFilePath();

    QJsonObject root;
    QJsonObject sys;
    sys["credential_storage_version"] = 1;
    sys["updated_at"] = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
    root["system"] = sys;

    QJsonArray devArr;
    for (const auto& item : m_devices) {
        QJsonObject obj = deviceToJson(item, true);
        devArr.append(obj);
    }
    root["devices"] = devArr;

    QJsonDocument doc(root);
    const auto bytes = doc.toJson(QJsonDocument::Indented);
    QString stage, reason;
    const auto writeLocal = [&](const QString& target) {
        const bool directoryReady = QDir().mkpath(QFileInfo(target).absolutePath());
        QSaveFile file(target);
        if (!file.open(QIODevice::WriteOnly)) {
            stage = directoryReady ? "LOCAL_ARCHIVE_OPEN_FAILED" : "LOCAL_ARCHIVE_DIRECTORY_FAILED";
            reason = file.errorString();
            return false;
        }
        const auto written = file.write(bytes);
        if (written != bytes.size()) {
            stage = "LOCAL_ARCHIVE_WRITE_FAILED";
            reason = tr("%1（期望 %2 字节，写入 %3 字节）").arg(file.errorString()).arg(bytes.size()).arg(written);
            file.cancelWriting();
            return false;
        }
        if (!file.commit()) {
            stage = "LOCAL_ARCHIVE_COMMIT_FAILED";
            reason = file.errorString();
            return false;
        }
        return true;
    };
    QList<QStringList> failures;
    QString recoveredFrom;
    if (!writeLocal(path)) {
        failures.append({stage, path, reason});
        // Preserve atomic writes; do not truncate a locked or protected credentials file.
        const auto fallback = DeviceCredentialStore::defaultPath();
        if (configPath.isEmpty() && DatabaseSettings::fromEnvironment().enabled && path != fallback) {
            recoveredFrom = path;
            path = QFileInfo(fallback).absoluteFilePath();
            if (!writeLocal(path)) { failures.append({stage, path, reason}); recoveredFrom.clear(); }
        }
        if (recoveredFrom.isEmpty()) {
            m_lastError = tr("%1 | 文件: %2 | 原因: %3").arg(stage, path, reason);
            locker.unlock();
            for (const auto& failure : failures) {
                qWarning().noquote() << failure.join(" | ");
                emit archiveSaveFailed(failure[0], failure[1], failure[2]);
            }
            return false;
        }
    }
    m_currentFilePath = path;
    m_lastError.clear();
    QString queueError;
    if (DatabaseSettings::fromEnvironment().enabled) {
        try {
            MySqlRepository::queueRegistry(root);
        } catch (const std::exception& e) {
            queueError = QString::fromUtf8(e.what());
            m_lastError = tr("REGISTRY_QUEUE_FAILED | 文件: %1 | 原因: %2")
                .arg(QFileInfo(QDir(MySqlRepository::registryQueueDirectory()).filePath("latest.json")).absoluteFilePath(), queueError);
        }
        if (queueError.isEmpty()) m_registrySavePending = false;
        locker.unlock();
        for (const auto& failure : failures) emit archiveSaveFailed(failure[0], failure[1], failure[2]);
        if (!recoveredFrom.isEmpty()) emit archiveSaveRecovered(recoveredFrom, path);
        if (!queueError.isEmpty()) {
            emit archiveSaveFailed("REGISTRY_QUEUE_FAILED",
                QFileInfo(QDir(MySqlRepository::registryQueueDirectory()).filePath("latest.json")).absoluteFilePath(), queueError);
            return false;
        }
        retryDatabaseSync();
        // Database mode uses this one file as the local credential cache only.
        return true;
    }

    m_registrySavePending = false;
    return true;
}

QList<DiscoveredCameraItem> DeviceConfigManager::getAllDevices() const {
    QMutexLocker locker(&m_mutex);
    return m_devices.values();
}

bool DeviceConfigManager::tryGetAllDevices(QList<DiscoveredCameraItem>& devices) const {
    if (!m_mutex.tryLock()) return false;
    devices = m_devices.values();
    m_mutex.unlock();
    return true;
}

DiscoveredCameraItem DeviceConfigManager::getDevice(const QString& cameraId) const {
    QMutexLocker locker(&m_mutex);
    return m_devices.value(cameraId);
}

bool DeviceConfigManager::hasDevice(const QString& cameraId) const {
    QMutexLocker locker(&m_mutex);
    return m_devices.contains(cameraId);
}

DiscoveredCameraItem DeviceConfigManager::registerDiscoveredDevice(const DiscoveredCameraItem& item, bool autoSave) {
    DiscoveredCameraItem registered = item;
    registered.hardwareId = CameraIdentity::normalize(item.hardwareId);
    // A scan without a verified stable identity cannot claim a persisted archive.
    if (registered.hardwareId.isEmpty()) return {};
    bool newDevice = false;
    bool needsSave = false;
    {
        QMutexLocker locker(&m_mutex);
        auto match = m_devices.end();
        for (auto it = m_devices.begin(); it != m_devices.end(); ++it) {
            if (CameraIdentity::normalize(it->hardwareId) == registered.hardwareId) { match = it; break; }
        }
        registered.cameraId = CameraIdentity::archiveId(registered.hardwareId);
        if (match != m_devices.end()) {
            registered = match.value();
            const auto oldHost = registered.ipOrSn;
            if (oldHost != item.ipOrSn || registered.port != item.port) {
                registered.ipOrSn = item.ipOrSn;
                registered.port = item.port;
                // Keep vendor path, stream selection, credentials and calibrated archive key.
                QUrl uri(registered.rtspUrl);
                if (!uri.host().isEmpty()) { uri.setHost(item.ipOrSn); registered.rtspUrl = uri.toString(); }
                needsSave = true;
            }
            registered.isOnline = item.isOnline;
            registered.lastSeenTime = item.lastSeenTime;
        } else newDevice = true;
        if (registered.lastSeenTime.isEmpty())
            registered.lastSeenTime = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
        m_devices.insert(registered.cameraId, registered);
        needsSave = needsSave || newDevice || m_registrySavePending;
    }
    if (autoSave && needsSave) save();
    return registered;
}

void DeviceConfigManager::addOrUpdateDevice(const DiscoveredCameraItem& item, bool autoSave) {
    {
        QMutexLocker locker(&m_mutex);
        if (m_devices.contains(item.cameraId)) {
            // Merge existing config (preserve user customName, credentials)
            auto& existing = m_devices[item.cameraId];
            if (!item.hardwareId.isEmpty()) existing.hardwareId = CameraIdentity::normalize(item.hardwareId);
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
            existing.safeWorkingRange = item.safeWorkingRange;
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

    }
    if (removed && autoSave) {
        save();
    }
    return removed;
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
                                                   bool autoSave,
                                                   const QJsonArray& scalePoints) {
    bool found = false;
    {
        QMutexLocker locker(&m_mutex);
        if (m_devices.contains(cameraId)) {
            auto& item = m_devices[cameraId];
            item.hasCalibration = true;
            item.perspectiveEnabled = perspectiveEnabled;
            item.calibratedWidth = calibW;
            item.calibratedHeight = calibH;
            item.pt0_x = p0x; item.pt0_y = p0y;
            item.pt1_x = p1x; item.pt1_y = p1y;
            item.pt2_x = p2x; item.pt2_y = p2y;
            item.pt3_x = p3x; item.pt3_y = p3y;
            item.centerX = cx; item.centerY = cy;
            item.pointerBaseRadius = baseRadius;
            item.scalePoints = scalePoints;
            found = true;
        }
    }
    if (found && autoSave) {
        save();
    }
}
