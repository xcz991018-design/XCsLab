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
};

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void onSelectImage();
    void onManageGaugeTemplates();
    void onRunRecognition();
    void onRecognitionFinished();
    void onCameraSelectionChanged();
    void onCalibrateCardinalPoints();
    void onEditCameraInfo(QListWidgetItem *item);

    // Auto Discovery & System Slots
    void onTriggerAutoDiscovery();
    void onDiscoveredCameraAdded(const DiscoveredCameraItem& item);
    void onFilterDeviceList(const QString& filterText);
    void onLogReceived(const QString& logMsg);
    void onAddNewDevice();
    void onRemoveSelectedDevice();
    void onSetDeviceAsDefaultAutoConnect();
    void onDeviceCustomContextMenu(const QPoint& pos);

    // Pointer diagnostic window
    void onShowDiagnosticWindow();

    // Stage 0: Live Stream & Capture Slots
    void onToggleLiveStream();
    void onCaptureCameraSnapshot();
    void onSaveSnapshotImage();
    void onUseSnapshotForRecognition();
    void onUpdateLiveStreamFrame();
    void onTabChanged(int index);

    // Inspection & Patrol Slots
    void onCaptureAndRecognizeCurrent();
    void onStartAllDevicesInspection();
    void onStopAllDevicesInspection();
    void onInspectionOutcome(const InspectionOutcome& outcome);
    void onExportPatrolReport();
    void onInspectionTableItemDoubleClicked(int row, int column);

private:
    InspectionTask makeSingleTask() const;
    void submitSingleTask(InspectionTask task);
    void setupUi();
    void createTopBanner(QVBoxLayout *rootLayout);
    QWidget* createLeftSidebar(QWidget *parent);
    QWidget* createCenterVisionArea(QWidget *parent);
    QWidget* createLiveStreamStageArea(QWidget *parent);
    QWidget* createInspectionStageArea(QWidget *parent);
    QWidget* createBottomLogConsole(QWidget *parent);
    void showInspectionPhotosDialog(int row);

    void applyDarkStyle();
    void loadAndRestoreSavedDevices();
    void updateDeviceListItemWidget(QListWidgetItem* item, const DiscoveredCameraItem& data);
    QListWidgetItem* findListItemByCameraId(const QString& camId);
    void updateImageCard(QLabel* label, const cv::Mat& mat, const QString& title);
    static QImage cvMatToQImage(const cv::Mat& mat);
    void switchActiveCameraSource(const QString& camId);
    void applyCameraCalibrationAndProfile(const QString& camId);

    // Top Status Bar Badges
    QLabel *m_statusChannelBadge{nullptr};
    QLabel *m_statusProberBadge{nullptr};
    QLabel *m_statusFpsBadge{nullptr};
    QLabel *m_statusCameraCountBadge{nullptr};

    // Left Sidebar Widgets
    QPushButton *m_autoDiscoverBtn{nullptr};
    QPushButton *m_addDeviceBtn{nullptr};
    QPushButton *m_removeDeviceBtn{nullptr};
    QLineEdit *m_deviceFilterEdit{nullptr};
    QListWidget *m_deviceListWidget{nullptr};
    QLineEdit *m_imagePathEdit{nullptr};
    QPushButton *m_browseImgBtn{nullptr};
    QPushButton *m_browseCfgBtn{nullptr};
    QPushButton *m_calibrateBtn{nullptr};
    QPushButton *m_snapAndRunBtn{nullptr};
    QPushButton *m_batchPatrolBtn{nullptr};
    QComboBox *m_runModeCombo{nullptr};
    QPushButton *m_runBtn{nullptr};

    // Metrics Display Cards
    QLabel *m_valLabel{nullptr};
    QLabel *m_angleLabel{nullptr};
    QLabel *m_centerLabel{nullptr};
    QLabel *m_timeLabel{nullptr};
    QLabel *m_confidenceLabel{nullptr};
    QLabel *m_statusLabel{nullptr};

    // Config Details Labels
    QLabel *m_cfgGaugeId{nullptr};
    QLabel *m_cfgRange{nullptr};
    QLabel *m_cfgAngleRange{nullptr};
    QLabel *m_cfgGisInfo{nullptr};
    QLabel *m_cfgLocationInfo{nullptr};
    QLabel *m_cfgGaugeSpec{nullptr};

    // Vision Area Tabbed View
    QTabWidget *m_visionTabWidget{nullptr};

    // Stage 0: 摄像头实时流与抓图 Stage Widgets
    QLabel *m_liveStreamLabel{nullptr};
    QLabel *m_capturedSnapshotLabel{nullptr};
    QPushButton *m_toggleLiveBtn{nullptr};
    QPushButton *m_captureBtn{nullptr};
    QPushButton *m_saveSnapshotBtn{nullptr};
    QPushButton *m_useSnapshotForRunBtn{nullptr};
    QLabel *m_liveCamInfoBadge{nullptr};
    QLabel *m_snapshotInfoLabel{nullptr};
    QTimer *m_liveTimer{nullptr};
    cv::Mat m_lastCapturedFrame;
    bool m_isLiveStreaming{true};

    // 4 Core Pipeline Stage Image Display Labels
    QLabel *m_imgStage1{nullptr}; // Step 1: 图像/视频采集原帧 (Original Stream Frame)
    QLabel *m_imgStage2{nullptr}; // Step 2: 视场透视矫正 (FOV Rectified Image)
    QLabel *m_imgStage3{nullptr}; // Step 3: 检测输入灰度图 (Grayscale Input)
    QLabel *m_imgStage4{nullptr}; // Step 4: 最终仪表读数识别 (Final Recognition Overlay)

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
    InspectionTask m_snapshotInput;
    bool m_hasAnalysisInput{false};
    bool m_hasSnapshotInput{false};
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
    QPushButton *m_stopPatrolBtn{nullptr};
    QPushButton *m_exportReportBtn{nullptr};
    QCheckBox *m_autoPatrolCheck{nullptr};
    QTimer *m_autoPatrolTimer{nullptr};
    InspectionService* m_inspectionService{nullptr};
    bool m_isPatrolRunning{false};
    int m_patrolCurrentIndex{0};
    QStringList m_patrolCameraIdQueue;
    QList<InspectionRecord> m_inspectionRecords;
};

#endif // MAIN_WINDOW_HPP
