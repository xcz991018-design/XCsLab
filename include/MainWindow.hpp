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

#include <QTabWidget>
#include <QDialog>
#include <QCloseEvent>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrent>
#include <opencv2/opencv.hpp>
#include "GaugeReaderEngine.hpp"
#include "GaugeProfile.hpp"
#include "GB28181StreamManager.hpp"
#include "ONVIFDeviceManager.hpp"
#include "AutoCameraDiscoveryService.hpp"

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override = default;

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void onSelectImage();
    void onLoadConfig();
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

    // Intermediate Processing Steps Slots
    void onExportAllSteps();
    void onShowEnlargedStepImage(int stepNumber);
    void onShowDiagnosticWindow();

    // Stage 0: Live Stream & Capture Slots
    void onToggleLiveStream();
    void onCaptureCameraSnapshot();
    void onUseSnapshotForRecognition();
    void onUpdateLiveStreamFrame();
    void onTabChanged(int index);

private:
    void setupUi();
    void createTopBanner(QVBoxLayout *rootLayout);
    QWidget* createLeftSidebar(QWidget *parent);
    QWidget* createCenterVisionArea(QWidget *parent);
    QWidget* createLiveStreamStageArea(QWidget *parent);
    QWidget* createBottomLogConsole(QWidget *parent);

    void applyDarkStyle();
    void updateCameraListFromConfig(const QString& configPath);
    QStringList loadCameraIdsFromConfig(const QString& configPath);
    void updateImageCard(QLabel* label, const cv::Mat& mat, const QString& title);
    static QImage cvMatToQImage(const cv::Mat& mat);
    void switchActiveCameraSource(const QString& camId);
    void renderIntermediateStepsUI(const std::vector<ProcessingStepImage>& steps);

    // Top Status Bar Badges
    QLabel *m_statusChannelBadge{nullptr};
    QLabel *m_statusProberBadge{nullptr};
    QLabel *m_statusFpsBadge{nullptr};
    QLabel *m_statusCameraCountBadge{nullptr};

    // Left Sidebar Widgets
    QPushButton *m_autoDiscoverBtn{nullptr};
    QLineEdit *m_deviceFilterEdit{nullptr};
    QListWidget *m_deviceListWidget{nullptr};
    QLineEdit *m_imagePathEdit{nullptr};
    QPushButton *m_browseImgBtn{nullptr};
    QLineEdit *m_configPathEdit{nullptr};
    QPushButton *m_browseCfgBtn{nullptr};
    QPushButton *m_calibrateBtn{nullptr};
    QPushButton *m_runBtn{nullptr};
    QPushButton *m_exportStepsBtn{nullptr};

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
    QPushButton *m_useSnapshotForRunBtn{nullptr};
    QLabel *m_liveCamInfoBadge{nullptr};
    QLabel *m_snapshotInfoLabel{nullptr};
    QTimer *m_liveTimer{nullptr};
    cv::Mat m_lastCapturedFrame;
    bool m_isLiveStreaming{true};

    // 4 Core Pipeline Stage Image Display Labels
    QLabel *m_imgStage1{nullptr}; // Step 1: 图像/视频采集原帧 (Original Stream Frame)
    QLabel *m_imgStage2{nullptr}; // Step 2: 视场透视矫正 (FOV Rectified Image)
    QLabel *m_imgStage3{nullptr}; // Step 3: 图像二值预处理 (Preprocessed Image)
    QLabel *m_imgStage4{nullptr}; // Step 4: 最终仪表读数识别 (Final Recognition Overlay)

    // Complete Step-by-Step Intermediate Pipeline Cards Container
    QWidget *m_allStepsGridContainer{nullptr};
    QGridLayout *m_allStepsGridLayout{nullptr};
    std::vector<QLabel*> m_imgStepLabels;
    std::vector<QLabel*> m_titleStepLabels;
    std::vector<QLabel*> m_descStepLabels;

    // Bottom System Log Console
    QTextEdit *m_logConsole{nullptr};

    // Step 12 Diagnostics Independent Window
    QDialog *m_diagnosticDialog{nullptr};
    QLabel *m_diagnosticImgLabel{nullptr};
    QTextEdit *m_diagnosticTextEdit{nullptr};

    // Core Managers & Engines (GB28181 & ONVIF encapsulated in backend)
    GaugeReaderEngine m_engine;
    GaugeProfile m_profile;
    GaugeResult m_latestResult;
    cv::Mat m_currentFrame;
    QString m_currentImagePath;
    QString m_currentConfigPath;
    QFutureWatcher<GaugeResult> m_watcher;
    std::chrono::high_resolution_clock::time_point m_startTime;

    // Backend Services
    GB28181StreamManagerPtr m_gbManager;
    ONVIFDeviceManagerPtr m_onvifManager;
    AutoCameraDiscoveryService *m_discoveryService{nullptr};
    
    // Active Camera Source
    CameraSourcePtr m_activeCameraSource{nullptr};
    QMap<QString, DiscoveredCameraItem> m_discoveredItemsMap;
};

#endif // MAIN_WINDOW_HPP
