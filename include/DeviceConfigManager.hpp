#ifndef DEVICE_CONFIG_MANAGER_HPP
#define DEVICE_CONFIG_MANAGER_HPP

#include <QString>
#include <QList>
#include <QMap>
#include <QMutex>
#include <QObject>
#include <QThreadPool>
#include <QFuture>
#include "AutoCameraDiscoveryService.hpp"

/**
 * @brief Persistent Device Registry and Configuration Manager.
 * 
 * Manages device persistence (config/devices.json), auto-connection state machine,
 * network parameters, and device life-cycle management.
 */
class DeviceConfigManager : public QObject {
    Q_OBJECT
public:
    static DeviceConfigManager& instance();

    bool load(const QString& configPath = "");
    bool save(const QString& configPath = "");

    QList<DiscoveredCameraItem> getAllDevices() const;
    DiscoveredCameraItem getDevice(const QString& cameraId) const;
    bool hasDevice(const QString& cameraId) const;

    void addOrUpdateDevice(const DiscoveredCameraItem& item, bool autoSave = true);
    bool removeDevice(const QString& cameraId, bool autoSave = true);

    QString getLastActiveDeviceId() const;
    void setLastActiveDeviceId(const QString& id, bool autoSave = true);

    bool isAutoConnectEnabled() const;
    void setAutoConnectEnabled(bool enable, bool autoSave = true);

    void setDeviceAutoConnect(const QString& cameraId, bool isAuto, bool autoSave = true);
    void markDeviceOnline(const QString& cameraId, bool online);

    void updateDeviceCalibration(const QString& cameraId,
                                 bool perspectiveEnabled,
                                 int calibW, int calibH,
                                 double p0x, double p0y,
                                 double p1x, double p1y,
                                 double p2x, double p2y,
                                 double p3x, double p3y,
                                 double cx, double cy,
                                 double baseRadius,
                                 bool autoSave = true);

    QString getConfigPath() const { return m_currentFilePath; }
    static QString resolveConfigPath(const QString& preferredPath = "");
    QString lastError() const;
    void retryDatabaseSync();
    void waitForDatabaseWrites();
signals:
    void databaseStatus(const QString& error);

private:
    DeviceConfigManager();
    ~DeviceConfigManager() override;
    DeviceConfigManager(const DeviceConfigManager&) = delete;
    DeviceConfigManager& operator=(const DeviceConfigManager&) = delete;

    void populateDefaultsIfEmpty();

    mutable QMutex m_mutex;
    QMap<QString, DiscoveredCameraItem> m_devices;
    QString m_lastActiveDeviceId;
    bool m_autoConnectOnStartup{true};
    QString m_currentFilePath;
    QString m_lastError;
    QThreadPool m_databasePool;
    QFuture<void> m_databaseFuture;
};

#endif // DEVICE_CONFIG_MANAGER_HPP
