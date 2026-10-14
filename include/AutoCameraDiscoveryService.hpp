#ifndef AUTO_CAMERA_DISCOVERY_SERVICE_HPP
#define AUTO_CAMERA_DISCOVERY_SERVICE_HPP

#include <QObject>
#include <QThread>
#include <QMutex>
#include <QTimer>
#include <QString>
#include <QJsonArray>
#include <QList>
#include <memory>
#include <QFuture>
#include <QElapsedTimer>
#include <QThreadPool>
#include <QTcpSocket>
#include <QPointer>
#include <atomic>
#include "CameraHealthSchedule.hpp"
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
    QJsonArray scalePoints; // Original-frame x/y and normalized scale ratio.
    bool hasCalibration{false};
    bool perspectiveEnabled{false};
    int calibratedWidth{0};
    int calibratedHeight{0};
    double pt0_x{-1.0};
    double pt0_y{-1.0};
    double pt1_x{-1.0};
    double pt1_y{-1.0};
    double pt2_x{-1.0};
    double pt2_y{-1.0};
    double pt3_x{-1.0};
    double pt3_y{-1.0};
    double centerX{-1.0};
    double centerY{-1.0};
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

    void startAutoScan(int intervalMs = 300000);
    void stopAutoScan();
    void recordCaptureState(const QString& cameraId, bool online);
    void deferHealthForCapture(const QString& cameraId);
    void recordInspectionState(const QString& cameraId, const QString& errorCode, bool captured);

public slots:
    void triggerScanNow();

signals:
    void cameraDiscovered(const DiscoveredCameraItem& item);
    void cameraOnlineStateChanged(const QString& cameraId, bool online);
    void logGenerated(const QString& logMsg);
    void discoveryCompleted(int totalCount);

private slots:
    void onScanTimerTimeout();
    void pollHealth();

private:
    void executeDiscoveryProcess(bool manual);
    void startDiscovery(bool manual);
    void publishHealth(const QString& cameraId, bool online, bool changed);

    QTimer *m_scanTimer{nullptr};
    QMutex m_mutex;
    bool m_isScanning{false};
    QFuture<void> m_scanFuture;
    QThreadPool m_discoveryPool;
    QTimer* m_healthTimer{nullptr};
    QElapsedTimer m_clock;
    CameraHealthSchedule m_health;
    QHash<QString, QPointer<QTcpSocket>> m_healthSockets;
    int m_healthCursor{0};
    qint64 m_healthWindow{-1};
    int m_healthStarted{0};
    std::atomic<bool> m_stopping{false};
};

#endif // AUTO_CAMERA_DISCOVERY_SERVICE_HPP
