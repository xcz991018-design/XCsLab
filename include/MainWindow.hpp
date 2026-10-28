#ifndef MAIN_WINDOW_HPP
#define MAIN_WINDOW_HPP

#include <QMainWindow>
#include <QLabel>
#include <QPushButton>
#include <QLineEdit>
#include <QComboBox>
#include <QTextEdit>
#include <QGridLayout>
#include <QScrollArea>
#include <QGroupBox>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QListWidget>
#include <QSplitter>
#include <QTimer>
#include <QCheckBox>
#include <QSpinBox>
#include <QSet>
#include <QPointer>
#include <QMessageBox>
#include <QSystemTrayIcon>
#include "InspectionAlarmDialog.hpp"

#include <QTabWidget>
#include <QTableWidget>
#include <QHeaderView>
#include <QProgressBar>
#include <QDialog>
#include <QCloseEvent>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrent>
#include <opencv2/opencv.hpp>
#include "GaugeReaderEngine.hpp"
#include "InspectionService.hpp"
#include "CameraPreviewService.hpp"
#include "GaugeProfile.hpp"
#include "GB28181StreamManager.hpp"
#include "ONVIFDeviceManager.hpp"
#include "AutoCameraDiscoveryService.hpp"
#include "DeviceConfigManager.hpp"

/**
 * @brief Standalone Inspection & Reading Record Descriptor.
 */
struct InspectionRecord {
    QString taskId;
    DiscoveredCameraItem device;
    QDateTime capturedAt;
    QString cameraId;
    QString customName;
    QString locationInfo;
    double readingValue{0.0};
    bool readingValid{false};
    std::string unit{"MPa"};
    double pointerAngle{0.0};
    double confidence{0.0};
    QString status{"未巡视"}; // "🟢 正常", "⚠️ 待复核", "🔴 离线", "❌ 失败"
    QString timestamp;

    // Stored pipeline images for inspection review
    cv::Mat step1_original;
    cv::Mat step2_rectified;
    cv::Mat step4_recognition;
    QString step2Path;
    QString originalPath;
    QString step4Path;
    QString diagnosticText;
    QJsonObject safetyAssessment;
};

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

signals:
    void logoutRequested();

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    QPointer<InspectionAlarmDialog> m_safeRangeAlert;
    QSystemTrayIcon* m_trayIcon{nullptr};
    bool m_quitting{false};
    bool m_backgroundHintShown{false};
    void setupSystemTray();
    void restoreMainWindow();
private slots:
    void updateSelectedDeviceArchive(const QString& camId);
    void onSelectImage();
    void onManageGaugeTemplates();
    void onRunRecognition();
    void onRecognitionFinished();
    void logRecognitionStageTimings(const QString& context, const QJsonObject& engineSteps);
    void onCameraSelectionChanged();
    void onCalibrateCardinalPoints();
    void onEditCameraInfo(QListWidgetItem *item, bool archiveOnly = false);

    // Auto Discovery & System Slots
    void onTriggerAutoDiscovery();
    void onDiscoveredCameraAdded(const DiscoveredCameraItem& item);
    void onFilterDeviceList(const QString& filterText);
    void onLogReceived(const QString& logMsg);
    void onAddNewDevice();
    void onRemoveSelectedDevice();
    void onDeviceCustomContextMenu(const QPoint& pos);

    // Pointer diagnostic window
    void onShowDiagnosticWindow();

    // Stage 0: Live Stream & Capture Slots
    void onToggleLiveStream();
    void onUpdateLiveStreamFrame();
    void onTabChanged(int index);

    // Inspection & Patrol Slots
    void onCaptureAndRecognizeCurrent(const QString& cameraId);
    void onStartAllDevicesInspection();
    void onStopAllDevicesInspection();
    void onInspectionOutcome(const InspectionOutcome& outcome);
    void onExportPatrolReport();
    void onInspectionTableItemDoubleClicked(int row, int column);

private:
    InspectionTask makeSingleTask() const;
    void submitSingleTask(InspectionTask task, bool historicalCalibrated = false);
    void setupUi();
    void createTopBanner(QVBoxLayout *rootLayout);
    QWidget* createLeftSidebar(QWidget *parent);
    QWidget* createCenterVisionArea(QWidget *parent);
    void showVisionPage(QWidget* page, const QString& title);
    void updatePatrolControls(bool stopping = false);
    QWidget* createLiveStreamStageArea(QWidget *parent);
    QWidget* createInspectionStageArea(QWidget *parent);
    QWidget* createBottomLogConsole(QWidget *parent);
    void showInspectionPhotosDialog(int row);
    QPushButton* createDeviceLiveButton(const QString& cameraId);
    void openDeviceLivePreview(const QString& cameraId);
    void checkAndSynchronizeCameraClock(const QString& cameraId);
    QTimer* m_clockCheckTimer{nullptr};
    QSet<QString> m_clockChecksInFlight;
    QHash<QString, QDateTime> m_lastClockCheck;

    void applyDarkStyle();
    void loadAndRestoreSavedDevices();
    void updateDeviceListItemWidget(QListWidgetItem* item, const DiscoveredCameraItem& data);
    QListWidgetItem* findListItemByCameraId(const QString& camId);
    void updateImageCard(QLabel* label, const cv::Mat& mat, const QString& title);
    static QImage cvMatToQImage(const cv::Mat& mat);
    void selectCameraForDetails(const QString& camId);
    void disconnectCameraPreview();
    QString m_selectedCameraId;
    QString m_previewCameraId;
    QSet<QString> m_offlineCameraIds; // Confirmed failures in this session; restored archives start unknown.
    void switchActiveCameraSource(const QString& camId);
    void applyCameraCalibrationAndProfile(const QString& camId);
    void checkCameraCalibration(const QString& camId);
    void tryOpenCameraCalibration();
    void requestCalibrationSnapshot(const QString& cameraId);
    QSet<QString> m_calibrationChecked;
    QSet<QString> m_calibrationQueries;
    QString m_pendingCalibrationCamera;
    quint64 m_cameraConnectionGeneration{0};
    bool m_calibrationDialogOpen{false};
    bool m_historicalCalibrationPending{false};
    bool m_cameraHasFrame{false};
    FrameCaptureResult m_calibrationSnapshot;
    QString m_calibrationSnapshotCamera;
    bool m_calibrationSnapshotLoading{false};

    // Top Status Bar Badges
    QLabel *m_statusCameraCountBadge{nullptr};

    // Left Sidebar Widgets
    QPushButton *m_autoDiscoverBtn{nullptr};
    QPushButton *m_addDeviceBtn{nullptr};
    QPushButton *m_removeDeviceBtn{nullptr};
    QLineEdit *m_deviceFilterEdit{nullptr};
    QListWidget *m_deviceListWidget{nullptr};

    // Metrics Display Cards
    QLabel *m_valLabel{nullptr};
    QLabel *m_centerLabel{nullptr};
    QLabel *m_timeLabel{nullptr};
    QLabel *m_confidenceLabel{nullptr};
    QLabel *m_statusLabel{nullptr};

    // Config Details Labels
    QLabel *m_cfgGaugeId{nullptr};
    QLabel *m_cfgRange{nullptr};
    QLabel *m_cfgHardwareId{nullptr};
    QLabel *m_cfgLocationInfo{nullptr};
    QLabel *m_cfgGaugeSpec{nullptr};
    QLabel *m_cfgNetworkInfo{nullptr};
    QLabel *m_cfgSafeRange{nullptr};
    QLabel *m_cfgCalibration{nullptr};

    // Vision Area Tabbed View
    QTabWidget *m_visionTabWidget{nullptr};
    QWidget *m_livePage{nullptr};
    QWidget *m_recognitionPage{nullptr};
    QWidget *m_inspectionPage{nullptr};

    // Stage 0: 摄像头实时预览 Stage Widgets
    QLabel *m_liveStreamLabel{nullptr};
    QPushButton *m_toggleLiveBtn{nullptr};
    QLabel *m_liveCamInfoBadge{nullptr};
    QTimer *m_liveTimer{nullptr};
    bool m_isLiveStreaming{false};

    // Original capture and final recognition image displays
    QLabel *m_originalImage{nullptr};
    QLabel *m_undistortedImage{nullptr};
    QLabel *m_rectifiedImage{nullptr};
    QLabel *m_recognitionImage{nullptr};

    // Bottom System Log Console
    QTextEdit *m_logConsole{nullptr};

    // Pointer Diagnostics Independent Window
    QDialog *m_diagnosticDialog{nullptr};
    QLabel *m_diagnosticImgLabel{nullptr};
    QTextEdit *m_diagnosticTextEdit{nullptr};

    // Core Managers & Engines (GB28181 & ONVIF encapsulated in backend)
    GaugeProfile m_profile;
    GaugeResult m_latestResult;
    cv::Mat m_currentFrame;
    QString m_currentImagePath;
    quint64 m_profileGeneration{0};
    bool m_profileReady{false};
    InspectionService* m_singleService{nullptr};
    CameraPreviewService* m_previewService{nullptr};
    InspectionTask m_singleSubmittedTask;
    InspectionTask m_analysisInput;
    bool m_hasAnalysisInput{false};
    bool m_registryLoaded{false};
    QFutureWatcher<bool>* m_registryWatcher{nullptr};
    QTimer* m_databaseSyncTimer{nullptr};
    QLabel* m_databaseStatusLabel{nullptr};
    std::chrono::high_resolution_clock::time_point m_startTime;

    // Backend Services
    AutoCameraDiscoveryService *m_discoveryService{nullptr};
    
    // Active Camera Source
    QMap<QString, DiscoveredCameraItem> m_discoveredItemsMap;

    // Stage 6: 全站设备巡检读数总览 (Batch Patrol Inspection Matrix)
    QTableWidget *m_inspectionTable{nullptr};
    QProgressBar *m_inspectionProgressBar{nullptr};
    QLabel *m_inspectionSummaryLabel{nullptr};
    QPushButton *m_startPatrolBtn{nullptr};

    QPushButton *m_exportReportBtn{nullptr};
    QCheckBox *m_autoPatrolCheck{nullptr};
    QSpinBox *m_autoPatrolInterval{nullptr};
    QTimer *m_autoPatrolTimer{nullptr};
    InspectionService* m_inspectionService{nullptr};
    bool m_isPatrolRunning{false};
    int m_patrolCurrentIndex{0};
    QStringList m_patrolCameraIdQueue;
    QList<InspectionRecord> m_inspectionRecords;
};

#endif // MAIN_WINDOW_HPP
