#ifndef AUTO_CAMERA_DISCOVERY_SERVICE_HPP
#define AUTO_CAMERA_DISCOVERY_SERVICE_HPP

#include <QObject>
#include <QThread>
#include <QMutex>
#include <QTimer>
#include <QString>
#include <QList>
#include <memory>
#include <QFuture>
#include "ICameraSource.hpp"
#include "GB28181StreamManager.hpp"
#include "ONVIFDeviceManager.hpp"

/**
 * @brief Discovered Camera Descriptor Structure.
 */
struct DiscoveredCameraItem {
    QString cameraId;
    QString customName;
    QString ipOrSn;
    CameraProtocolType protocolType{CameraProtocolType::LOCAL_FILE};
    QString protocolName{"智能图像采集终端"};
    QString modelName{"Pressure Gauge Sensor Node"};
    int port{80};
    int rtspPort{554};
    QString rtspUrl;
    QString username{"admin"};
    QString password{""};
    bool isOnline{true};
    QString defaultProfileId{"PRESSURE_GAUGE_1.6MPA"};
    bool isAutoConnect{false};
    QString lastSeenTime;

    // Asset & Physical Installation Metadata (no defaults, requires explicit input)
    QString substation;
    QString area;
    QString installationLocation;
    QString deviceTag;
    QString gaugeType;
    QString manufacturer;
    QString accuracyClass;

    // Field Installation & Perspective Geometry Calibration
    bool perspectiveEnabled{true};
    int calibratedWidth{800};
    int calibratedHeight{448};
    double pt0_x{404.444};
    double pt0_y{19.2593};
    double pt1_x{644.444};
    double pt1_y{306.667};
    double pt2_x{392.593};
    double pt2_y{408.889};
    double pt3_x{139.259};
    double pt3_y{339.259};
    double centerX{407.407};
    double centerY{327.407};
    double pointerBaseRadius{47.0};
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
    QFuture<void> m_scanFuture;
};

#endif // AUTO_CAMERA_DISCOVERY_SERVICE_HPP
