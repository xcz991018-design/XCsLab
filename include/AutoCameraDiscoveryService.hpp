#ifndef AUTO_CAMERA_DISCOVERY_SERVICE_HPP
#define AUTO_CAMERA_DISCOVERY_SERVICE_HPP

#include <QObject>
#include <QThread>
#include <QMutex>
#include <QTimer>
#include <QString>
#include <QList>
#include <memory>
#include "ICameraSource.hpp"
#include "GB28181StreamManager.hpp"
#include "ONVIFDeviceManager.hpp"

/**
 * @brief Discovered Camera Descriptor Structure.
 */
struct DiscoveredCameraItem {
    QString cameraId;
    QString ipOrSn;
    CameraProtocolType protocolType;
    QString protocolName;
    QString modelName;
    bool isOnline{true};
    QString defaultProfileId{"PRESSURE_GAUGE_1.6MPA"};
};

Q_DECLARE_METATYPE(DiscoveredCameraItem)

/**
 * @brief Automatic Camera Plug-and-Play & Network Discovery Service.
 * 
 * Periodically or on-demand scans the local network using ONVIF WS-Discovery,
 * GB28181 Catalog Probe, and Vendor SDK Enumeration APIs. Automatically reports
 * newly connected cameras to the system GUI.
 */
class AutoCameraDiscoveryService : public QObject {
    Q_OBJECT

public:
    explicit AutoCameraDiscoveryService(QObject *parent = nullptr);
    ~AutoCameraDiscoveryService() override;

    void startAutoScan(int intervalMs = 15000);
    void stopAutoScan();

public slots:
    void triggerScanNow();

signals:
    void cameraDiscovered(const DiscoveredCameraItem& item);
    void cameraOnlineStateChanged(const QString& cameraId, bool online);
    void logGenerated(const QString& logMsg);
    void discoveryCompleted(int totalCount);

private slots:
    void onScanTimerTimeout();

private:
    void executeDiscoveryProcess();

    QTimer *m_scanTimer{nullptr};
    QMutex m_mutex;
    bool m_isScanning{false};
    QList<DiscoveredCameraItem> m_knownCameras;
};

#endif // AUTO_CAMERA_DISCOVERY_SERVICE_HPP
