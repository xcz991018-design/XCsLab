#include "MainWindow.hpp"
#include <QBrush>
#include <QColor>
#include "InspectionCalibration.hpp"
#include "RecognitionQuality.hpp"
#include "CardinalSelectorDialog.hpp"
#include "ScaleCalibration.hpp"
#include "CameraInfoEditDialog.hpp"
#include "GaugeTemplateDialog.hpp"
#include <QFileDialog>
#include <QMessageBox>
#include <QFileInfo>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDateTime>
#include <QDebug>
#include <QMenu>
#include <QAction>
#include <chrono>
#include "SDKCameraSourceStub.hpp"
#include "FileCameraSource.hpp"
#include "DeviceConfigManager.hpp"
#include "MySqlRepository.hpp"
#include "DatabaseHistoryDialog.hpp"
#include "DatabaseConnectionDialog.hpp"

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    m_discoveryService = new AutoCameraDiscoveryService(this);
    connect(m_discoveryService, &AutoCameraDiscoveryService::logGenerated, this, &MainWindow::onLogReceived);
    connect(m_discoveryService, &AutoCameraDiscoveryService::cameraDiscovered, this, &MainWindow::onDiscoveredCameraAdded);
    connect(m_discoveryService, &AutoCameraDiscoveryService::cameraOnlineStateChanged, this,
            [this](const QString& cameraId, bool online) {
        if (!m_discoveredItemsMap.contains(cameraId)) return;
        DeviceConfigManager::instance().markDeviceOnline(cameraId, online);
        auto& device = m_discoveredItemsMap[cameraId];
        device.isOnline = online;
        if (online) device.lastSeenTime = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
        if (auto* row = findListItemByCameraId(cameraId)) updateDeviceListItemWidget(row, device);
    });

    setupUi();
    applyDarkStyle();

    m_inspectionService = new InspectionService(this);
    connect(m_inspectionService, &InspectionService::taskFinished, this, &MainWindow::onInspectionOutcome);
    connect(m_inspectionService, &InspectionService::taskStarted, this, [this](const QString&, const QString& cameraId) {
        const int row = m_patrolCameraIdQueue.indexOf(cameraId);
        if (row >= 0) m_inspectionTable->item(row, 7)->setText(tr("抓拍识别中..."));
    });
    connect(m_inspectionService, &InspectionService::progressChanged, this, [this](int done, int total) {
        m_patrolCurrentIndex = done;
        m_inspectionProgressBar->setValue(total ? done * 100 / total : 0);
        m_inspectionSummaryLabel->setText(tr("正在全站巡检 (%1/%2)").arg(done).arg(total));
    });
    connect(m_inspectionService, &InspectionService::finished, this, [this](bool cancelled) {
        m_isPatrolRunning = false;
        m_startPatrolBtn->setEnabled(true);
        m_batchPatrolBtn->setEnabled(true);
        m_stopPatrolBtn->setEnabled(false);
        if (cancelled) {
            for (int i = 0; i < m_inspectionRecords.size(); ++i) {
                if (m_inspectionRecords[i].timestamp == "-") {
                    m_inspectionRecords[i].status = tr("已取消");
                    m_inspectionTable->item(i, 7)->setText(tr("已取消"));
                }
            }
        }
        m_inspectionSummaryLabel->setText(tr("巡检%1 (%2/%3)")
            .arg(cancelled ? tr("已停止") : tr("已完成"))
            .arg(m_patrolCurrentIndex).arg(m_patrolCameraIdQueue.size()));
    });

    m_singleService = new InspectionService(this, 1);
    m_previewService = new CameraPreviewService(this);
    connect(m_singleService, &InspectionService::taskFinished, this, [this](const InspectionOutcome& out) {
        const auto submitted = m_singleSubmittedTask;
        if (!out.databaseError.isEmpty()) onLogReceived(tr("[MYSQL] %1 · 待同步: %2").arg(out.databaseError).arg(out.databasePending));
        if (!out.capturedFrame.empty()) {
            m_currentFrame = out.capturedFrame;
            m_analysisInput = submitted;
            m_analysisInput.databaseOriginalTaskId.clear();
            m_analysisInput.historicalProfile = out.profileSnapshot;
            m_analysisInput.localImagePath = submitted.inputKind == InspectionInputKind::LocalImage
                ? submitted.localImagePath : out.originalPath;
            if (submitted.inputKind != InspectionInputKind::LocalImage) {
                m_analysisInput.inputKind = InspectionInputKind::HistoricalImage;
                m_analysisInput.parentTaskId = out.taskId;
                m_analysisInput.originalCapturedAt = out.capturedAt;
                if (out.databaseSaved) m_analysisInput.databaseOriginalTaskId = out.taskId;
            }
            m_hasAnalysisInput = !m_analysisInput.localImagePath.isEmpty() || !m_analysisInput.databaseOriginalTaskId.isEmpty();
            m_currentImagePath = m_analysisInput.localImagePath;
            m_imagePathEdit->setText(m_currentImagePath);
            updateImageCard(m_imgStage1, m_currentFrame, "Task original");
            if (submitted.inputKind == InspectionInputKind::CameraSnapshot) {
                m_lastCapturedFrame = out.capturedFrame;
                m_snapshotInput = m_analysisInput;
                m_hasSnapshotInput = m_hasAnalysisInput;
                updateImageCard(m_capturedSnapshotLabel, m_lastCapturedFrame, "Snapshot");
                m_useSnapshotForRunBtn->setEnabled(m_hasSnapshotInput);
                m_saveSnapshotBtn->setEnabled(true);
            }
        } else {
            m_currentFrame.release();
            m_lastCapturedFrame.release();
            m_currentImagePath.clear();
            m_imagePathEdit->clear();
            m_hasAnalysisInput = m_hasSnapshotInput = false;
            m_useSnapshotForRunBtn->setEnabled(false);
            m_saveSnapshotBtn->setEnabled(false);
            updateImageCard(m_capturedSnapshotLabel, cv::Mat(), "Snapshot");
        }
        m_snapshotInfoLabel->setText(tr("来源: %1 | 设备: %2 | 任务: %3 | 原始时间: %4")
            .arg(inspectionInputName(out.inputKind)).arg(out.cameraId).arg(out.taskId)
            .arg(out.capturedAt.toLocalTime().toString(Qt::ISODateWithMs)));
        if (!submitted.captureOnly || !out.errorCode.isEmpty()) {
            m_latestResult = out.result;
            if (!out.errorCode.isEmpty()) {
                m_latestResult.success = false;
                m_latestResult.errorMessage = (out.errorCode + ": " + out.errorMessage).toStdString();
            }
            onRecognitionFinished();
            if (!out.capturedFrame.empty()) updateImageCard(m_imgStage1, out.capturedFrame, "Task original");
        } else m_statusLabel->setText(tr("图片已就绪；分析将仅使用此任务对应的指定图片。"));
    });
    connect(m_singleService, &InspectionService::imageExported, this, [this](const QString& path, const QString& error) {
        m_saveSnapshotBtn->setEnabled(m_hasSnapshotInput);
        onLogReceived(error.isEmpty() ? tr("[EXPORT] 已保存: %1").arg(path) : tr("[EXPORT] 失败: %1").arg(error));
    });
    connect(m_singleService, &InspectionService::finished, this, [this](bool cancelled) {
        m_runBtn->setEnabled(m_hasAnalysisInput);
        m_captureBtn->setEnabled(true);
        m_snapAndRunBtn->setEnabled(true);
        m_browseImgBtn->setEnabled(true);
        if (cancelled) m_statusLabel->setText(tr("单设备任务已取消。"));
    });
    connect(m_previewService, &CameraPreviewService::frameReady, this,
            [this](const QString& cameraId, const FrameCaptureResult& result) {
        if (!result.ok()) m_cameraHasFrame = false;
        if (result.ok()) {
            if (!m_cameraHasFrame) m_calibrationChecked.remove(cameraId);
            m_cameraHasFrame = true;
            checkCameraCalibration(cameraId);
            tryOpenCameraCalibration();
        }
        if (!m_isLiveStreaming) return;
        if (m_discoveredItemsMap.contains(cameraId)) {
            DeviceConfigManager::instance().markDeviceOnline(cameraId, result.ok());
            auto& device = m_discoveredItemsMap[cameraId];
            device.isOnline = result.ok();
            if (result.ok()) device.lastSeenTime = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
            if (auto* row = findListItemByCameraId(cameraId)) updateDeviceListItemWidget(row, device);
        }
        m_liveCamInfoBadge->setText(tr("设备: %1 | 采集状态: %2").arg(cameraId, captureFailureName(result.failure)));
        if (m_visionTabWidget->currentIndex() == 0) {
            if (result.ok()) updateImageCard(m_liveStreamLabel, result.frame, "Live frame");
            else {
                m_liveStreamLabel->clear();
                m_liveStreamLabel->setText(tr("暂无真实画面: %1").arg(captureFailureName(result.failure)));
                if (!result.message.empty()) m_liveStreamLabel->setText(tr("暂无真实画面: %1\n%2").arg(captureFailureName(result.failure), QString::fromStdString(result.message)));
            }
        }
    });

    m_autoPatrolTimer = new QTimer(this);
    connect(m_autoPatrolTimer, &QTimer::timeout, this, &MainWindow::onStartAllDevicesInspection);

    m_currentImagePath.clear();
    m_imagePathEdit->clear();

    // Load the device list; preview starts only after user selection.
    loadAndRestoreSavedDevices();

    // Start background periodic network discovery without clearing user list
    m_discoveryService->startAutoScan(30000);
    if (DatabaseSettings::fromEnvironment().enabled) {
        connect(&DeviceConfigManager::instance(), &DeviceConfigManager::databaseStatus, this, [this](const QString& error) {
            if (error == "REGISTRY_SYNC_IN_PROGRESS") {
                m_databaseStatusLabel->setText(tr("MySQL 档案正在同步…")); return;
            }
            m_databaseStatusLabel->setText(error.isEmpty() ? tr("MySQL 档案已同步") : tr("MySQL 待同步: %1").arg(error));
            if (!error.isEmpty()) onLogReceived(tr("[MYSQL] 档案同步失败: %1").arg(error));
        });
        connect(m_inspectionService, &InspectionService::databaseSyncFinished, this, [this](const QString& error) {
            if (error == "REGISTRY_SYNC_IN_PROGRESS") {
                m_databaseStatusLabel->setText(tr("MySQL 档案正在同步…")); return;
            }
            m_databaseStatusLabel->setText(error.isEmpty() ? tr("MySQL 同步检查完成") : tr("MySQL 待同步: %1").arg(error));
        });
        m_databaseSyncTimer = new QTimer(this);
        connect(m_databaseSyncTimer, &QTimer::timeout, this, [this]() {
            if (!m_registryLoaded) { loadAndRestoreSavedDevices(); return; }
            m_inspectionService->retryDatabaseWrites(QCoreApplication::applicationDirPath() + "/inspection_snapshots");
        });
        m_databaseSyncTimer->start(30000);
        if (m_registryLoaded)
            m_inspectionService->retryDatabaseWrites(QCoreApplication::applicationDirPath() + "/inspection_snapshots");
    }

    onLogReceived(tr("[SYSTEM] 工业指针仪表智能感知识别与检测平台已就绪。"));
    onLogReceived(tr("[SYSTEM] 采集与预览由后台服务执行；未接入的协议明确返回失败。"));

    // Setup Live Stream Frame Grabber Timer (25 FPS)
    m_liveTimer = new QTimer(this);
    connect(m_liveTimer, &QTimer::timeout, this, &MainWindow::onUpdateLiveStreamFrame);
    m_liveTimer->start(40);
}

void MainWindow::setupUi() {
    setWindowTitle(tr("工业指针仪表智能感知识别与检测平台 Demo"));
    resize(1550, 960);

    QWidget *centralWidget = new QWidget(this);
    setCentralWidget(centralWidget);

    QVBoxLayout *rootLayout = new QVBoxLayout(centralWidget);
    rootLayout->setContentsMargins(10, 10, 10, 10);
    rootLayout->setSpacing(8);

    // 1. Top Banner
    createTopBanner(rootLayout);
    auto* databaseBar = new QHBoxLayout;
    auto* connectionButton = new QPushButton(tr("MySQL 连接配置"), centralWidget);
    connectionButton->setObjectName("mysqlConnectionConfigurationButton");
    auto* historyButton = new QPushButton(tr("数据库历史查询 / 原图复核"), centralWidget);
    historyButton->setEnabled(DatabaseSettings::fromEnvironment().enabled);
    m_databaseStatusLabel = new QLabel(DatabaseSettings::fromEnvironment().enabled ? tr("MySQL 正在加载档案…")
        : DatabaseSettings::fromEnvironment().configurationError.isEmpty() ? tr("MySQL 未配置；请点击 MySQL 连接配置")
        : DatabaseSettings::fromEnvironment().configurationError, centralWidget);
    databaseBar->addWidget(connectionButton);
    auto* templateButton = new QPushButton(tr("表计算法模板管理"), centralWidget);
    templateButton->setObjectName("gaugeTemplateManagementButton");
    databaseBar->addWidget(templateButton);
    connect(templateButton, &QPushButton::clicked, this, &MainWindow::onManageGaugeTemplates);
    databaseBar->addWidget(historyButton); databaseBar->addWidget(m_databaseStatusLabel, 1);
    rootLayout->addLayout(databaseBar);
    connect(connectionButton, &QPushButton::clicked, this, [this]() {
        DatabaseConnectionDialog dialog(this);
        dialog.exec();
    });
    connect(historyButton, &QPushButton::clicked, this, [this]() {
        DatabaseHistoryDialog dialog(this, [this](InspectionTask task) {
            if (m_singleService->isRunning()) {
                onLogReceived(tr("[MYSQL] 单设备任务正在运行，请完成后重新分析历史原图。"));
                return;
            }
            m_runModeCombo->setCurrentIndex(2);
            submitSingleTask(task);
        });
        dialog.exec();
    });

    // 2. Main Content Splitter (Left Sidebar | Center Vision Area)
    QSplitter *mainSplitter = new QSplitter(Qt::Horizontal, centralWidget);

    QWidget *leftSidebar = createLeftSidebar(mainSplitter);
    QWidget *centerArea = createCenterVisionArea(mainSplitter);

    mainSplitter->addWidget(leftSidebar);
    mainSplitter->addWidget(centerArea);

    mainSplitter->setStretchFactor(0, 3); // Left 25%
    mainSplitter->setStretchFactor(1, 8); // Center 75%

    rootLayout->addWidget(mainSplitter, 1);

    // 3. Bottom System Log Console
    QWidget *bottomConsole = createBottomLogConsole(centralWidget);
    rootLayout->addWidget(bottomConsole);
}

void MainWindow::createTopBanner(QVBoxLayout *rootLayout) {
    QGroupBox *bannerBox = new QGroupBox(this);
    bannerBox->setStyleSheet("QGroupBox { background-color: #1a1c24; border: 1px solid #2d3142; border-radius: 6px; }");
    QHBoxLayout *bannerLayout = new QHBoxLayout(bannerBox);
    bannerLayout->setContentsMargins(12, 6, 12, 6);

    QLabel *titleLabel = new QLabel(tr("🌐 工业指针仪表智能感知识别与检测平台"), bannerBox);
    titleLabel->setStyleSheet("font-size: 17px; font-weight: bold; color: #00e5ff;");

    bannerLayout->addWidget(titleLabel);
    bannerLayout->addStretch();

    m_statusChannelBadge = new QLabel(tr("🟢 视频传输: 实时连通"), bannerBox);
    m_statusChannelBadge->setStyleSheet("background-color: #1b3a2f; color: #69f0ae; border: 1px solid #00c853; border-radius: 12px; padding: 4px 10px; font-weight: bold;");

    m_statusProberBadge = new QLabel(tr("🔵 探针服务: 活动中"), bannerBox);
    m_statusProberBadge->setStyleSheet("background-color: #1a2f4c; color: #448aff; border: 1px solid #2979ff; border-radius: 12px; padding: 4px 10px; font-weight: bold;");

    m_statusFpsBadge = new QLabel(tr("⚡ 实时帧率: 25.0 FPS"), bannerBox);
    m_statusFpsBadge->setStyleSheet("background-color: #3e2723; color: #ffd740; border: 1px solid #ffab00; border-radius: 12px; padding: 4px 10px; font-weight: bold;");

    m_statusCameraCountBadge = new QLabel(tr("📷 在线设备: 5 台"), bannerBox);
    m_statusCameraCountBadge->setStyleSheet("background-color: #311b92; color: #e040fb; border: 1px solid #d500f9; border-radius: 12px; padding: 4px 10px; font-weight: bold;");

    bannerLayout->addWidget(m_statusChannelBadge);
    bannerLayout->addWidget(m_statusProberBadge);
    bannerLayout->addWidget(m_statusFpsBadge);
    bannerLayout->addWidget(m_statusCameraCountBadge);

    rootLayout->addWidget(bannerBox);
}

QWidget* MainWindow::createLeftSidebar(QWidget *parent) {
    QWidget *sidebar = new QWidget(parent);
    QVBoxLayout *sideLayout = new QVBoxLayout(sidebar);
    sideLayout->setContentsMargins(0, 0, 0, 0);
    sideLayout->setSpacing(8);

    // 1. Auto-Discovery & Devices Box
    QGroupBox *devBox = new QGroupBox(tr("摄像头设备管理库（点击设备查看实时画面）"), sidebar);
    QVBoxLayout *devLayout = new QVBoxLayout(devBox);

    QHBoxLayout *devBtnLayout = new QHBoxLayout();
    devBtnLayout->setSpacing(6);

    m_autoDiscoverBtn = new QPushButton(tr("🔄 刷新探针"), devBox);
    m_autoDiscoverBtn->setToolTip(tr("向局域网广播 ONVIF/GB28181/JLink 探针报文，增量更新在线状态（不会清空已知设备）"));
    m_autoDiscoverBtn->setStyleSheet("QPushButton { background-color: #0091ea; color: white; font-weight: bold; padding: 7px; border-radius: 4px; }"
                                     "QPushButton:hover { background-color: #00b0ff; }"
                                     "QPushButton:pressed { background-color: #007cbb; }");

    m_addDeviceBtn = new QPushButton(tr("➕ 添加"), devBox);
    m_addDeviceBtn->setToolTip(tr("手动登记并添加局域网或指定IP网络摄像头到持久化设备库"));
    m_addDeviceBtn->setStyleSheet("QPushButton { background-color: #00897b; color: white; font-weight: bold; padding: 7px; border-radius: 4px; }"
                                  "QPushButton:hover { background-color: #00bfa5; }");

    m_removeDeviceBtn = new QPushButton(tr("🗑️ 删除"), devBox);
    m_removeDeviceBtn->setToolTip(tr("从持久化设备库中移除当前选中的设备"));
    m_removeDeviceBtn->setStyleSheet("QPushButton { background-color: #c62828; color: white; font-weight: bold; padding: 7px; border-radius: 4px; }"
                                     "QPushButton:hover { background-color: #e53935; }");

    devBtnLayout->addWidget(m_autoDiscoverBtn, 2);
    devBtnLayout->addWidget(m_addDeviceBtn, 1);
    devBtnLayout->addWidget(m_removeDeviceBtn, 1);
    devLayout->addLayout(devBtnLayout);

    m_deviceFilterEdit = new QLineEdit(devBox);
    m_deviceFilterEdit->setPlaceholderText(tr("🔍 输入 IP 或名称快速筛选设备..."));
    m_deviceFilterEdit->setClearButtonEnabled(true);
    m_deviceFilterEdit->setStyleSheet("QLineEdit { background-color: #1a1b24; border: 1px solid #00e5ff; border-radius: 4px; padding: 5px; color: #00e5ff; font-weight: bold; }");
    devLayout->addWidget(m_deviceFilterEdit);

    m_deviceListWidget = new QListWidget(devBox);
    m_deviceListWidget->setContextMenuPolicy(Qt::CustomContextMenu);
    m_deviceListWidget->setStyleSheet("QListWidget { background-color: #12131a; border: 1px solid #2d3142; border-radius: 4px; }"
                                       "QListWidget::item { padding: 6px; border-bottom: 1px solid #1e2130; }"
                                       "QListWidget::item:selected { background-color: #005c99; color: #ffffff; }");
    devLayout->addWidget(m_deviceListWidget);
    sideLayout->addWidget(devBox);

    // 2. Local File & Config Group
    QGroupBox *fileBox = new QGroupBox(tr("测试图像源与仪表配置"), sidebar);
    QVBoxLayout *fileLayout = new QVBoxLayout(fileBox);

    fileLayout->addWidget(new QLabel(tr("静态图像源:")));
    QHBoxLayout *imgFileLayout = new QHBoxLayout();
    m_imagePathEdit = new QLineEdit(fileBox);
    m_browseImgBtn = new QPushButton(tr("浏览..."), fileBox);
    imgFileLayout->addWidget(m_imagePathEdit);
    imgFileLayout->addWidget(m_browseImgBtn);
    fileLayout->addLayout(imgFileLayout);

    fileLayout->addWidget(new QLabel(tr("算法模板：MySQL 数据库"), fileBox));
    m_snapAndRunBtn = new QPushButton(tr("📷 拍摄并识别当前表计"), fileBox);
    m_snapAndRunBtn->setStyleSheet("background-color: #00897b; color: white; font-weight: bold; font-size: 13px; padding: 9px; border-radius: 6px;");
    fileLayout->addWidget(m_snapAndRunBtn);

    m_batchPatrolBtn = new QPushButton(tr("🚀 启动全站批量巡检"), fileBox);
    m_batchPatrolBtn->setStyleSheet("background-color: #0288d1; color: white; font-weight: bold; font-size: 13px; padding: 8px; border-radius: 6px;");
    fileLayout->addWidget(m_batchPatrolBtn);


    m_runModeCombo = new QComboBox(fileBox);
    m_runModeCombo->addItem(tr("生产模式：读数与原图留存"), int(GaugeRunMode::Production));
    m_runModeCombo->addItem(tr("证据模式：核心四步图片"), int(GaugeRunMode::Evidence));
    m_runModeCombo->addItem(tr("调试模式：识别图片与算法诊断"), int(GaugeRunMode::Debug));
    m_runModeCombo->setCurrentIndex(1);
    m_runModeCombo->setToolTip(tr("生产模式不绘制中间图；全站巡检在所有模式下保留本次原图供人工复核。"));
    fileLayout->addWidget(m_runModeCombo);

    m_runBtn = new QPushButton(tr("分析指定图片（本地/历史）"), fileBox);
    m_runBtn->setStyleSheet("background-color: #2e7d32; color: white; font-weight: bold; font-size: 12px; padding: 6px; border-radius: 4px;");
    fileLayout->addWidget(m_runBtn);

    sideLayout->addWidget(fileBox);

    // 3. Profile Info Card
    QGroupBox *profileBox = new QGroupBox(tr("摄像头与仪表规格档案 (GIS/位置/表头)"), sidebar);
    QVBoxLayout *profLayout = new QVBoxLayout(profileBox);
    m_cfgGaugeId = new QLabel(tr("设备/节点编号: -"), profileBox);
    m_cfgLocationInfo = new QLabel(tr("安装位置: -"), profileBox);
    m_cfgGisInfo = new QLabel(tr("GIS 坐标: -"), profileBox);
    m_cfgGaugeSpec = new QLabel(tr("表头规格: -"), profileBox);
    m_cfgRange = new QLabel(tr("量程范围: -"), profileBox);

    profLayout->addWidget(m_cfgGaugeId);
    profLayout->addWidget(m_cfgLocationInfo);
    profLayout->addWidget(m_cfgGisInfo);
    profLayout->addWidget(m_cfgGaugeSpec);
    profLayout->addWidget(m_cfgRange);
    sideLayout->addWidget(profileBox);

    // 4. Metrics Summary Card
    QGroupBox *resultBox = new QGroupBox(tr("仪表识别结果与分析指标"), sidebar);
    QVBoxLayout *resLayout = new QVBoxLayout(resultBox);

    m_valLabel = new QLabel(tr("<b>读取数值:</b> <font color='#00e676' size='+2'>--</font>"), resultBox);
    m_angleLabel = new QLabel(tr("指针角度: --°"), resultBox);
    m_centerLabel = new QLabel(tr("表盘中心: (-- , --) px"), resultBox);
    m_confidenceLabel = new QLabel(tr("识别置信度: -- %"), resultBox);
    m_timeLabel = new QLabel(tr("算法耗时: -- ms"), resultBox);
    m_statusLabel = new QLabel(tr("系统状态: 就绪"), resultBox);

    resLayout->addWidget(m_valLabel);
    resLayout->addWidget(m_angleLabel);
    resLayout->addWidget(m_centerLabel);
    resLayout->addWidget(m_confidenceLabel);
    auto qualityLabel = new QLabel(tr("旁路质量：未评估"), resultBox);
    qualityLabel->setObjectName("recognitionQuality");
    qualityLabel->setWordWrap(true);
    resLayout->addWidget(qualityLabel);
    resLayout->addWidget(m_timeLabel);
    resLayout->addWidget(m_statusLabel);
    sideLayout->addWidget(resultBox);

    // Connections
    connect(m_autoDiscoverBtn, &QPushButton::clicked, this, &MainWindow::onTriggerAutoDiscovery);
    connect(m_addDeviceBtn, &QPushButton::clicked, this, &MainWindow::onAddNewDevice);
    connect(m_removeDeviceBtn, &QPushButton::clicked, this, &MainWindow::onRemoveSelectedDevice);
    connect(m_deviceFilterEdit, &QLineEdit::textChanged, this, &MainWindow::onFilterDeviceList);
    connect(m_deviceListWidget, &QListWidget::itemClicked, this, &MainWindow::onCameraSelectionChanged);
    connect(m_deviceListWidget, &QListWidget::itemActivated, this, &MainWindow::onCameraSelectionChanged);
    connect(m_deviceListWidget, &QListWidget::customContextMenuRequested, this, &MainWindow::onDeviceCustomContextMenu);
    connect(m_browseImgBtn, &QPushButton::clicked, this, &MainWindow::onSelectImage);
    connect(m_snapAndRunBtn, &QPushButton::clicked, this, &MainWindow::onCaptureAndRecognizeCurrent);
    connect(m_batchPatrolBtn, &QPushButton::clicked, this, &MainWindow::onStartAllDevicesInspection);
    connect(m_runBtn, &QPushButton::clicked, this, &MainWindow::onRunRecognition);

    return sidebar;
}

QWidget* MainWindow::createCenterVisionArea(QWidget *parent) {
    m_visionTabWidget = new QTabWidget(parent);
    m_visionTabWidget->setStyleSheet(
        "QTabWidget::pane { border: 1px solid #272a38; background-color: #121216; border-radius: 4px; }"
        "QTabBar::tab { background-color: #1a1c26; color: #9fa6b2; padding: 8px 16px; font-weight: bold; border: 1px solid #272a38; border-top-left-radius: 4px; border-top-right-radius: 4px; margin-right: 2px; }"
        "QTabBar::tab:selected { background-color: #272a3e; color: #00e5ff; border-bottom-color: #00e5ff; }"
        "QTabBar::tab:hover { color: #ffffff; background-color: #202332; }"
    );

    // --- Tab 0: Stage 0 摄像头实时流与抓图 ---
    QWidget *liveStageArea = createLiveStreamStageArea(m_visionTabWidget);
    m_visionTabWidget->addTab(liveStageArea, tr("📷 摄像头实时流与抓图 (Live Stream & Capture)"));

    // --- Tab 1: 核心 4 步主流程 ---
    QScrollArea *tab1Scroll = new QScrollArea(m_visionTabWidget);
    tab1Scroll->setWidgetResizable(true);
    tab1Scroll->setStyleSheet("QScrollArea { border: none; background-color: #121216; }");

    QWidget *gridContainer = new QWidget(tab1Scroll);
    QGridLayout *gridLayout = new QGridLayout(gridContainer);
    gridLayout->setSpacing(10);

    auto createStageCard = [this, gridContainer](const QString& title, QLabel*& outLabel) {
        QGroupBox *card = new QGroupBox(title, gridContainer);
        QVBoxLayout *cardLayout = new QVBoxLayout(card);
        cardLayout->setContentsMargins(6, 10, 6, 6);

        outLabel = new QLabel(card);
        outLabel->setAlignment(Qt::AlignCenter);
        outLabel->setMinimumSize(420, 320);
        outLabel->setStyleSheet("background-color: #171822; border: 1px solid #2b2e3e; border-radius: 4px;");

        cardLayout->addWidget(outLabel);
        return card;
    };

    gridLayout->addWidget(createStageCard(tr("步骤 1: 实时传输/采集原帧 (Original Stream Frame)"), m_imgStage1), 0, 0);
    gridLayout->addWidget(createStageCard(tr("步骤 2: 4切点视场透视矫正图 (FOV Rectified Image)"), m_imgStage2), 0, 1);
    gridLayout->addWidget(createStageCard(tr("步骤 3: 检测输入灰度图 (Grayscale Input)"), m_imgStage3), 1, 0);

    QGroupBox *stage4Card = createStageCard(tr("步骤 4: 最终仪表读数识别 (Final Recognition Result)"), m_imgStage4);
    QPushButton *stage4DiagBtn = new QPushButton(tr("📊 独立窗口查看指针算法诊断信息 (HUD)"), stage4Card);
    stage4DiagBtn->setStyleSheet("QPushButton { background-color: #1a2a3a; color: #00e5ff; border: 1px solid #00a0e9; padding: 5px; border-radius: 4px; font-size: 12px; font-weight: bold; margin-top: 4px; }"
                                 "QPushButton:hover { background-color: #243b55; border-color: #00e5ff; }");
    connect(stage4DiagBtn, &QPushButton::clicked, this, &MainWindow::onShowDiagnosticWindow);
    stage4Card->layout()->addWidget(stage4DiagBtn);
    gridLayout->addWidget(stage4Card, 1, 1);

    tab1Scroll->setWidget(gridContainer);
    m_visionTabWidget->addTab(tab1Scroll, tr("⚡ 核心 4 步主流程 (Core 4 Stages)"));

    // --- Tab 2: 全站设备巡检读数总览 (Batch Patrol Inspection Matrix) ---
    QWidget *inspectionArea = createInspectionStageArea(m_visionTabWidget);
    m_visionTabWidget->addTab(inspectionArea, tr("📊 全站设备巡检报表 (Batch Inspection Matrix)"));

    connect(m_visionTabWidget, &QTabWidget::currentChanged, this, &MainWindow::onTabChanged);

    return m_visionTabWidget;
}

QWidget* MainWindow::createLiveStreamStageArea(QWidget *parent) {
    QWidget *container = new QWidget(parent);
    QVBoxLayout *mainLayout = new QVBoxLayout(container);
    mainLayout->setContentsMargins(8, 8, 8, 8);
    mainLayout->setSpacing(8);

    // 1. Top Header & Control Toolbar
    QGroupBox *toolbarBox = new QGroupBox(tr("📷 摄像头实时流监控与快照抓取控制台 (Stage 0: Live Stream & Capture)"), container);
    QHBoxLayout *tbLayout = new QHBoxLayout(toolbarBox);
    tbLayout->setContentsMargins(10, 6, 10, 6);

    m_liveCamInfoBadge = new QLabel(tr("📷 当前通道: 未选择摄像头"), toolbarBox);
    m_liveCamInfoBadge->setStyleSheet("color: #00e5ff; font-weight: bold; font-size: 13px;");
    tbLayout->addWidget(m_liveCamInfoBadge);

    tbLayout->addStretch();

    m_toggleLiveBtn = new QPushButton(tr("⏸ 暂停实时预览"), toolbarBox);
    m_toggleLiveBtn->setStyleSheet("QPushButton { background-color: #f57c00; color: white; font-weight: bold; padding: 6px 14px; border-radius: 4px; }"
                                   "QPushButton:hover { background-color: #ff9800; }");
    tbLayout->addWidget(m_toggleLiveBtn);

    m_captureBtn = new QPushButton(tr("📸 抓取当前帧快照"), toolbarBox);
    m_captureBtn->setStyleSheet("QPushButton { background-color: #0091ea; color: white; font-weight: bold; padding: 6px 14px; border-radius: 4px; }"
                                "QPushButton:hover { background-color: #00b0ff; }");
    tbLayout->addWidget(m_captureBtn);

    m_saveSnapshotBtn = new QPushButton(tr("💾 保存图像"), toolbarBox);
    m_saveSnapshotBtn->setStyleSheet("QPushButton { background-color: #5c6bc0; color: white; font-weight: bold; padding: 6px 14px; border-radius: 4px; }"
                                     "QPushButton:hover { background-color: #7986cb; }"
                                     "QPushButton:disabled { background-color: #1a1c24; color: #555868; }");
    m_saveSnapshotBtn->setEnabled(false);
    tbLayout->addWidget(m_saveSnapshotBtn);

    m_useSnapshotForRunBtn = new QPushButton(tr("分析此任务的已保存原图"), toolbarBox);
    m_useSnapshotForRunBtn->setStyleSheet("QPushButton { background-color: #00897b; color: white; font-weight: bold; padding: 6px 14px; border-radius: 4px; }"
                                          "QPushButton:hover { background-color: #00bfa5; }"
                                          "QPushButton:disabled { background-color: #1a2725; color: #556b67; }");
    m_useSnapshotForRunBtn->setEnabled(false);
    tbLayout->addWidget(m_useSnapshotForRunBtn);

    mainLayout->addWidget(toolbarBox);

    // 2. Main Content Splitter (Left: Live Stream Video Player | Right: Captured Snapshot Preview)
    QSplitter *splitter = new QSplitter(Qt::Horizontal, container);

    // Left Box: Real-Time Stream Video Screen
    QGroupBox *streamBox = new QGroupBox(tr("📺 摄像头实时视频传输画面 (Live Camera Stream)"), splitter);
    QVBoxLayout *streamLayout = new QVBoxLayout(streamBox);
    streamLayout->setContentsMargins(6, 6, 6, 6);

    m_liveStreamLabel = new QLabel(streamBox);
    m_liveStreamLabel->setAlignment(Qt::AlignCenter);
    m_liveStreamLabel->setMinimumSize(520, 380);
    m_liveStreamLabel->setStyleSheet("background-color: #0a0b0e; border: 1px solid #232635; border-radius: 4px;");
    m_liveStreamLabel->setText(tr("<font color='#555868'>请在左侧列表中手动选择设备以查看实时画面</font>"));

    streamLayout->addWidget(m_liveStreamLabel, 1);
    splitter->addWidget(streamBox);

    // Right Box: Captured Snapshot Preview & Details Panel
    QGroupBox *snapshotBox = new QGroupBox(tr("🖼️ 画面抓取快照与采样档案 (Captured Image Snapshot)"), splitter);
    QVBoxLayout *snapshotLayout = new QVBoxLayout(snapshotBox);
    snapshotLayout->setContentsMargins(6, 6, 6, 6);
    snapshotLayout->setSpacing(6);

    m_capturedSnapshotLabel = new QLabel(snapshotBox);
    m_capturedSnapshotLabel->setAlignment(Qt::AlignCenter);
    m_capturedSnapshotLabel->setMinimumSize(380, 280);
    m_capturedSnapshotLabel->setStyleSheet("background-color: #0a0b0e; border: 1px solid #232635; border-radius: 4px;");
    m_capturedSnapshotLabel->setText(tr("<font color='#555868'>尚未抓取快照图片<br>点击上方【📸 抓取当前帧快照】按钮生成帧快照</font>"));

    m_snapshotInfoLabel = new QLabel(tr("快照状态: 未抓取\n捕获时间: --\n分辨率: -- x --\n采样设备: --"), snapshotBox);
    m_snapshotInfoLabel->setStyleSheet("color: #69f0ae; background-color: #0c0d12; border: 1px solid #1e2130; border-radius: 4px; padding: 10px; font-family: 'Consolas', 'Courier New', monospace; font-size: 12px;");

    snapshotLayout->addWidget(m_capturedSnapshotLabel, 1);
    snapshotLayout->addWidget(m_snapshotInfoLabel);
    splitter->addWidget(snapshotBox);

    splitter->setStretchFactor(0, 6); // Stream 60%
    splitter->setStretchFactor(1, 4); // Snapshot 40%

    mainLayout->addWidget(splitter, 1);

    // Connections
    connect(m_toggleLiveBtn, &QPushButton::clicked, this, &MainWindow::onToggleLiveStream);
    connect(m_captureBtn, &QPushButton::clicked, this, &MainWindow::onCaptureCameraSnapshot);
    connect(m_saveSnapshotBtn, &QPushButton::clicked, this, &MainWindow::onSaveSnapshotImage);
    connect(m_useSnapshotForRunBtn, &QPushButton::clicked, this, &MainWindow::onUseSnapshotForRecognition);

    return container;
}

QWidget* MainWindow::createBottomLogConsole(QWidget *parent) {
    QGroupBox *consoleBox = new QGroupBox(tr("系统通信与算法运行日志控制台"), parent);
    consoleBox->setFixedHeight(160);
    QVBoxLayout *layout = new QVBoxLayout(consoleBox);
    layout->setContentsMargins(6, 6, 6, 6);

    m_logConsole = new QTextEdit(consoleBox);
    m_logConsole->setReadOnly(true);
    m_logConsole->setStyleSheet("QTextEdit { background-color: #0a0b0e; color: #69f0ae; font-family: 'Consolas', 'Courier New', monospace; font-size: 12px; border: 1px solid #1e2130; }");

    layout->addWidget(m_logConsole);
    return consoleBox;
}

void MainWindow::applyDarkStyle() {
    QString style = R"(
        QMainWindow { background-color: #101117; }
        QWidget { color: #d1d5db; font-family: 'Segoe UI', Microsoft YaHei, sans-serif; font-size: 13px; }
        QGroupBox { border: 1px solid #272a38; border-radius: 6px; margin-top: 10px; font-weight: bold; background-color: #161822; color: #00e5ff; }
        QGroupBox::title { subcontrol-origin: margin; subcontrol-position: top left; padding: 2px 8px; background-color: #161822; }
        QLineEdit, QComboBox, QSpinBox { background-color: #1f2230; border: 1px solid #33384c; border-radius: 4px; padding: 5px; color: #ffffff; }
        QLineEdit:focus, QComboBox:focus, QSpinBox:focus { border: 1px solid #00e5ff; }
        QPushButton { background-color: #272a3a; border: 1px solid #3a3f58; border-radius: 4px; padding: 5px 10px; color: #ffffff; }
        QPushButton:hover { background-color: #35394e; border-color: #00e5ff; }
        QPushButton:disabled { background-color: #15161f; color: #555868; border-color: #1e2130; }
    )";
    setStyleSheet(style);
}

void MainWindow::loadAndRestoreSavedDevices() {
    if (!m_deviceListWidget) return;
    if (DatabaseSettings::fromEnvironment().enabled && !m_registryLoaded) {
        if (m_registryWatcher) return;
        m_deviceListWidget->setEnabled(false);
        for (auto* button : {m_addDeviceBtn,m_removeDeviceBtn,m_captureBtn,m_snapAndRunBtn,m_startPatrolBtn,m_batchPatrolBtn})
            button->setEnabled(false);
        m_registryWatcher = new QFutureWatcher<bool>(this);
        connect(m_registryWatcher, &QFutureWatcher<bool>::finished, this, [this]() {
            const bool loaded = m_registryWatcher->result();
            m_registryWatcher->deleteLater(); m_registryWatcher = nullptr;
            m_registryLoaded = loaded;
            m_databaseStatusLabel->setText(loaded ? tr("MySQL 档案加载完成") : tr("MySQL 档案加载失败: %1").arg(DeviceConfigManager::instance().lastError()));
            if (!loaded) {
                onLogReceived(m_databaseStatusLabel->text());
                m_autoDiscoverBtn->setEnabled(true);
                return;
            }
            m_deviceListWidget->setEnabled(true);
            for (auto* button : {m_addDeviceBtn,m_removeDeviceBtn,m_autoDiscoverBtn,m_captureBtn,m_snapAndRunBtn,m_startPatrolBtn,m_batchPatrolBtn})
                button->setEnabled(true);
            loadAndRestoreSavedDevices();
        });
        m_registryWatcher->setFuture(QtConcurrent::run([]() { return DeviceConfigManager::instance().load(); }));
        return;
    }

    if (m_previewService) m_previewService->select(InspectionTask());
    m_cameraHasFrame=false;
    m_liveStreamLabel->clear();
    m_liveStreamLabel->setText(tr("请在左侧列表中手动选择设备以查看实时画面。"));
    m_deviceListWidget->clear();
    m_discoveredItemsMap.clear();

    if (!DatabaseSettings::fromEnvironment().enabled) DeviceConfigManager::instance().load();
    QList<DiscoveredCameraItem> devices = DeviceConfigManager::instance().getAllDevices();

    for (auto& dev : devices) {
        // Saved online flags describe a previous run, not the current network.
        dev.isOnline = false;
        DeviceConfigManager::instance().markDeviceOnline(dev.cameraId, false);
        m_discoveredItemsMap.insert(dev.cameraId, dev);
        QListWidgetItem *item = new QListWidgetItem(m_deviceListWidget);
        item->setData(Qt::UserRole, dev.cameraId);
        updateDeviceListItemWidget(item, dev);
    }

    if (m_statusCameraCountBadge) {
        m_statusCameraCountBadge->setText(tr("📷 在库设备: %1 台").arg(m_discoveredItemsMap.size()));
    }

    // Initialize or sync inspection matrix table with all saved devices
    if (m_inspectionTable && !m_isPatrolRunning) {
        m_inspectionTable->setRowCount(devices.size());
        m_inspectionRecords.clear();
        m_patrolCameraIdQueue.clear();

        QString snapshotDir = QCoreApplication::applicationDirPath() + "/inspection_snapshots";
        for (int i = 0; i < devices.size(); ++i) {
            const auto& dev = devices[i];
            m_patrolCameraIdQueue.append(dev.cameraId);

            InspectionRecord rec;
            rec.cameraId = dev.cameraId;
        rec.device = dev;
            rec.customName = dev.customName.isEmpty() ? dev.modelName : dev.customName;
            rec.locationInfo = QString("%1 | %2 | %3").arg(dev.substation).arg(dev.area).arg(dev.installationLocation);
            rec.unit = "MPa";
            rec.status = tr("未巡视");
            rec.timestamp = "-";

            // Check if cached snapshots exist from previous runs
            QString step2File = QString("%1/%2_step2_rectified.png").arg(snapshotDir).arg(dev.cameraId);
            QString step4File = QString("%1/%2_step4_recognition.png").arg(snapshotDir).arg(dev.cameraId);
            if (QFileInfo::exists(step2File)) rec.step2Path = step2File;
            if (QFileInfo::exists(step4File)) rec.step4Path = step4File;

            m_inspectionRecords.append(rec);

            m_inspectionTable->setItem(i, 0, new QTableWidgetItem(QString::number(i + 1)));
            m_inspectionTable->setItem(i, 1, new QTableWidgetItem(rec.cameraId));
            m_inspectionTable->setItem(i, 2, new QTableWidgetItem(rec.customName));
            m_inspectionTable->setItem(i, 3, new QTableWidgetItem(rec.locationInfo));
            m_inspectionTable->setItem(i, 4, new QTableWidgetItem("--"));
            m_inspectionTable->setItem(i, 5, new QTableWidgetItem("--"));
            m_inspectionTable->setItem(i, 6, new QTableWidgetItem("--"));
            m_inspectionTable->setItem(i, 7, new QTableWidgetItem(rec.status));
            m_inspectionTable->setItem(i, 8, new QTableWidgetItem(rec.timestamp));

            QPushButton *viewBtn = new QPushButton(tr("🖼️ 查看照片"), m_inspectionTable);
            viewBtn->setStyleSheet(
                "QPushButton { background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #1a365d, stop:1 #2b6cb0); "
                "color: #90cdf4; border: 1px solid #3182ce; border-radius: 4px; padding: 3px 8px; font-weight: bold; font-size: 12px; } "
                "QPushButton:hover { background: #3182ce; color: #ffffff; }"
            );
            connect(viewBtn, &QPushButton::clicked, this, [this, i]() {
                showInspectionPhotosDialog(i);
            });
            m_inspectionTable->setCellWidget(i, 9, viewBtn);
        }
        if (m_inspectionSummaryLabel) {
            m_inspectionSummaryLabel->setText(tr("📊 巡检状态: 就绪 | 监控节点总数: %1 台 | 点击【启动全站批量巡检】或选择设备单张抓拍").arg(devices.size()));
        }
    }
}

void MainWindow::updateDeviceListItemWidget(QListWidgetItem* item, const DiscoveredCameraItem& data) {
    if (!item) return;

    QString statusIcon = data.isOnline ? "🟢" : "🔴";
    QString nameText = data.customName.isEmpty() ? data.modelName : data.customName;
    const bool calibrated = data.hasCalibration &&
        hasSavedDeviceCalibration(deviceToJson(data)["calibration"].toObject());
    const QString calibrationStatus = calibrated ? tr("已标定") : tr("未标定");
    const QString calibrationBadge = calibrated ? tr("✅[已标定]") : tr("🟠[未标定]");

    QString line1 = QString("%1 %2 %3 (%4)")
                        .arg(statusIcon)
                        .arg(calibrationBadge)
                        .arg(data.cameraId)
                        .arg(nameText);

    QString line2 = QString("   地址/SN: %1 | %2")
                        .arg(data.ipOrSn)
                        .arg(data.protocolName);

    item->setText(line1 + "\n" + line2);
    item->setForeground(QBrush(QColor(calibrated ? "#e0e0e0" : "#ffb74d")));
    item->setBackground(QBrush(QColor(calibrated ? "#12131a" : "#302316")));
    auto font = item->font();
    font.setBold(!calibrated);
    item->setFont(font);
    item->setToolTip(tr("设备编号: %1\n名称备注: %2\n地址/SN: %3\n通信协议: %4\n端口: %5 (RTSP: %6)\n默认表计: %7\n在线状态: %8\n表计标定: %9\n\n💡 提示: 右键可编辑设备档案或进行表计标定")
                         .arg(data.cameraId)
                         .arg(nameText)
                         .arg(data.ipOrSn)
                         .arg(data.protocolName)
                         .arg(data.port)
                         .arg(data.rtspPort)
                         .arg(data.defaultProfileId)
                         .arg(data.isOnline ? tr("在线") : tr("离线"))
                         .arg(calibrated ? calibrationStatus : tr("未标定，请先完成标定后再识别或巡检")));
}

QListWidgetItem* MainWindow::findListItemByCameraId(const QString& camId) {
    if (!m_deviceListWidget) return nullptr;
    for (int i = 0; i < m_deviceListWidget->count(); ++i) {
        QListWidgetItem *it = m_deviceListWidget->item(i);
        if (it && it->data(Qt::UserRole).toString() == camId) {
            return it;
        }
    }
    return nullptr;
}

void MainWindow::onTriggerAutoDiscovery() {
    m_autoDiscoverBtn->setEnabled(false);
    onLogReceived(tr("[AUTO-DISCOVERY] 正在静默向局域网广播探针报文检测在线状态...（保留现有设备库，不中断当前流）"));

    // DO NOT CLEAR user list! Keeps existing connections untouched.
    m_discoveryService->triggerScanNow();
    QTimer::singleShot(2500, this, [this]() {
        m_autoDiscoverBtn->setEnabled(true);
        onLogReceived(tr("[AUTO-DISCOVERY] 局域网探针检测完成，在线状态已更新。"));
    });
}

void MainWindow::onDiscoveredCameraAdded(const DiscoveredCameraItem& item) {
    // Defer full registry snapshots until calibration commits and updates the cache.
    if (m_calibrationDialogOpen) {
        QTimer::singleShot(250, this, [this, item]() { onDiscoveredCameraAdded(item); });
        return;
    }
    if (DatabaseSettings::fromEnvironment().enabled && !m_registryLoaded) {
        // Do not submit an incomplete snapshot that could replace DB archives.
        m_discoveredItemsMap[item.cameraId] = item;
        auto* row = findListItemByCameraId(item.cameraId);
        if (!row) {
            row = new QListWidgetItem(m_deviceListWidget);
            row->setData(Qt::UserRole, item.cameraId);
        }
        updateDeviceListItemWidget(row, item);
        onLogReceived(tr("[AUTO-DISCOVERY] 已发现摄像头 %1；数据库档案未就绪，恢复后再登记。").arg(item.ipOrSn));
        return;
    }
    const int previousCount = DeviceConfigManager::instance().getAllDevices().size();
    const auto registered = DeviceConfigManager::instance().registerDiscoveredDevice(item, true);
    if (DeviceConfigManager::instance().getAllDevices().size() > previousCount) {
        onLogReceived(DatabaseSettings::fromEnvironment().enabled
            ? tr("[DEVICE] 新发现设备 %1 已自动登记并提交数据库同步，写入状态见 MySQL 状态栏。").arg(registered.cameraId)
            : tr("[DEVICE] 新发现设备 %1 已登记到本地档案；启用 MySQL 后可使用数据库设备管理。").arg(registered.cameraId));
    }
    m_discoveredItemsMap.insert(registered.cameraId, registered);
    auto* listItem = findListItemByCameraId(registered.cameraId);
    if (!listItem) {
        listItem = new QListWidgetItem(m_deviceListWidget);
        listItem->setData(Qt::UserRole, registered.cameraId);
    }
    updateDeviceListItemWidget(listItem, registered);


    if (m_deviceFilterEdit) {
        QString filterKey = m_deviceFilterEdit->text().trimmed().toLower();
        if (!filterKey.isEmpty() && !listItem->text().toLower().contains(filterKey)) {
            listItem->setHidden(true);
        }
    }

    if (m_statusCameraCountBadge) {
        m_statusCameraCountBadge->setText(tr("📷 在库设备: %1 台").arg(m_discoveredItemsMap.size()));
    }
}

void MainWindow::onAddNewDevice() {
    CameraInfoEditDialog dialog("", this, true);
    if (dialog.exec() == QDialog::Accepted) {
        DiscoveredCameraItem newItem = dialog.getUpdatedDeviceItem();
        onLogReceived(tr("[DEVICE] 已成功登记新摄像头节点: %1 (%2)").arg(newItem.cameraId).arg(newItem.ipOrSn));

        m_discoveredItemsMap.insert(newItem.cameraId, newItem);
        DeviceConfigManager::instance().addOrUpdateDevice(newItem, true);

        QListWidgetItem *it = findListItemByCameraId(newItem.cameraId);
        if (!it) {
            it = new QListWidgetItem(m_deviceListWidget);
            it->setData(Qt::UserRole, newItem.cameraId);
        }
        updateDeviceListItemWidget(it, newItem);
        onLogReceived(tr("[DEVICE] 请在设备列表中手动选择该设备以查看实时画面。"));

        if (m_statusCameraCountBadge) {
            m_statusCameraCountBadge->setText(tr("📷 在库设备: %1 台").arg(m_discoveredItemsMap.size()));
        }
    }
}

void MainWindow::onRemoveSelectedDevice() {
    QListWidgetItem *curr = m_deviceListWidget->currentItem();
    if (!curr) {
        QMessageBox::information(this, tr("提示"), tr("请先在列表中选中需要删除的摄像头！"));
        return;
    }

    QString camId = curr->data(Qt::UserRole).toString();
    auto reply = QMessageBox::question(this, tr("确认删除"),
                                      tr("确定要从持久化设备库中移除摄像头 [%1] 吗？\n此操作将同时更新 devices.json 配置文件。").arg(camId),
                                      QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (reply == QMessageBox::Yes) {
        DeviceConfigManager::instance().removeDevice(camId, true);
        m_discoveredItemsMap.remove(camId);
        delete curr;

        onLogReceived(tr("[DEVICE] 已从设备库中删除摄像头: %1").arg(camId));

        if (m_deviceListWidget->count() > 0) {
            m_deviceListWidget->clearSelection();
            m_deviceListWidget->setCurrentItem(nullptr);
            if (m_previewService) m_previewService->select(InspectionTask());
            m_cameraHasFrame=false;
            m_liveStreamLabel->clear();
            m_liveStreamLabel->setText(tr("请在左侧列表中手动选择设备以查看实时画面。"));
        } else {
            if (m_singleService) m_singleService->stop();
            if (m_previewService) m_previewService->select(InspectionTask());
            m_hasAnalysisInput = m_hasSnapshotInput = false;
            m_currentFrame.release();
            m_lastCapturedFrame.release();
        }

        if (m_statusCameraCountBadge) {
            m_statusCameraCountBadge->setText(tr("📷 在库设备: %1 台").arg(m_discoveredItemsMap.size()));
        }
    }
}

void MainWindow::onDeviceCustomContextMenu(const QPoint& pos) {
    QListWidgetItem *item = m_deviceListWidget->itemAt(pos);
    QMenu menu(this);

    if (item) {
        m_deviceListWidget->setCurrentItem(item);
        QString camId = item->data(Qt::UserRole).toString();
        menu.addAction(tr("⚡ 立即连接此设备"), this, [this, camId]() {
            switchActiveCameraSource(camId);
        });
        menu.addAction(tr("✏️ 编辑设备网络参数与表计档案..."), this, [this, item]() {
            onEditCameraInfo(item);
        });
        menu.addAction(tr("🎯 表计标定..."), this, &MainWindow::onCalibrateCardinalPoints);
        menu.addAction(tr("🗑️ 从设备库移除此设备"), this, &MainWindow::onRemoveSelectedDevice);
        menu.addSeparator();
    }

    menu.addAction(tr("➕ 添加新网络摄像头..."), this, &MainWindow::onAddNewDevice);
    menu.addAction(tr("🔄 刷新局域网探针设备状态"), this, &MainWindow::onTriggerAutoDiscovery);

    menu.exec(m_deviceListWidget->mapToGlobal(pos));
}

void MainWindow::onFilterDeviceList(const QString& filterText) {
    if (!m_deviceListWidget) return;
    QString key = filterText.trimmed().toLower();
    for (int i = 0; i < m_deviceListWidget->count(); ++i) {
        QListWidgetItem *item = m_deviceListWidget->item(i);
        if (!item) continue;
        if (key.isEmpty() || item->text().toLower().contains(key)) {
            item->setHidden(false);
        } else {
            item->setHidden(true);
        }
    }
}

MainWindow::~MainWindow() {
    if (m_registryWatcher) m_registryWatcher->waitForFinished();
    DeviceConfigManager::instance().waitForDatabaseWrites();
    delete m_previewService;
    delete m_singleService;
    delete m_inspectionService;
    delete m_discoveryService;
}

void MainWindow::closeEvent(QCloseEvent *event) {
    if (m_databaseSyncTimer) m_databaseSyncTimer->stop();
    if (m_liveTimer) m_liveTimer->stop();
    if (m_autoPatrolTimer) m_autoPatrolTimer->stop();
    if (m_inspectionService) m_inspectionService->stop();
    if (m_singleService) m_singleService->stop();
    if (m_previewService) m_previewService->stop();
    if (m_discoveryService) m_discoveryService->stopAutoScan();
    event->accept();
}

void MainWindow::onCameraSelectionChanged() {
    QListWidgetItem *curr = m_deviceListWidget->currentItem();
    if (!curr) return;

    QString camId = curr->data(Qt::UserRole).toString();
    switchActiveCameraSource(camId);
}

void MainWindow::switchActiveCameraSource(const QString& camId) {
    if (!m_discoveredItemsMap.contains(camId)) return;
    m_cameraHasFrame = false;
    ++m_cameraConnectionGeneration;
    m_calibrationChecked.remove(camId);
    m_calibrationQueries.clear();
    m_pendingCalibrationCamera.clear();
    if (m_singleService && m_singleService->isRunning()) m_singleService->stop();
    m_hasAnalysisInput = m_hasSnapshotInput = false;
    m_latestResult = GaugeResult();
    m_valLabel->setText(tr("读取数值: --"));
    m_statusLabel->setText(tr("已切换设备，请明确选择图像来源。"));
    updateImageCard(m_imgStage1, cv::Mat(), "Original");
    updateImageCard(m_imgStage2, cv::Mat(), "Rectified");
    updateImageCard(m_imgStage3, cv::Mat(), "Grayscale Input");
    updateImageCard(m_imgStage4, cv::Mat(), "Recognition");
    m_currentFrame.release();
    m_lastCapturedFrame.release();
    m_currentImagePath.clear();
    m_imagePathEdit->clear();
    m_runBtn->setEnabled(false);
    m_useSnapshotForRunBtn->setEnabled(false);
    m_saveSnapshotBtn->setEnabled(false);
    updateImageCard(m_capturedSnapshotLabel, cv::Mat(), "Snapshot");
    m_liveStreamLabel->clear();
    m_liveStreamLabel->setText(tr("正在连接所选设备..."));
    InspectionTask task;
    task.device = m_discoveredItemsMap.value(camId);
    m_previewService->select(task);
    applyCameraCalibrationAndProfile(camId);
}

void MainWindow::applyCameraCalibrationAndProfile(const QString& camId) {
    if (!m_discoveredItemsMap.contains(camId)) return;
    const auto generation = ++m_profileGeneration;
    const auto profileId = m_discoveredItemsMap[camId].defaultProfileId.isEmpty()
        ? QStringLiteral("PRESSURE_GAUGE_1.6MPA") : m_discoveredItemsMap[camId].defaultProfileId;
    m_profileReady = false;
    m_cfgRange->setText(tr("量程范围：正在读取数据库模板…"));
    using ProfileReply = QPair<QJsonObject, QString>;
    auto* watcher = new QFutureWatcher<ProfileReply>(this);
    connect(watcher, &QFutureWatcher<ProfileReply>::finished, this, [this, watcher, camId, generation]() {
        const auto reply = watcher->result(); watcher->deleteLater();
        if (generation != m_profileGeneration || makeSingleTask().device.cameraId != camId) return;
        if (!reply.second.isEmpty() || !m_profile.loadFromJson(reply.first)) {
            m_cfgRange->setText(tr("量程范围：数据库模板加载失败"));
            onLogReceived(tr("[TEMPLATE] %1").arg(reply.second));
            return;
        }
        const auto item = m_discoveredItemsMap.value(camId);
        // Overlay the selected device's identity and calibration.
        m_profile.gauge_id = camId.toStdString();
        m_profile.location.substation = item.substation.toStdString();
        m_profile.location.area = item.area.toStdString();
        m_profile.location.device_name = item.customName.toStdString();
        m_profile.location.device_tag = item.deviceTag.toStdString();
        m_profile.spec.gauge_type = item.gaugeType.toStdString();
        m_profile.spec.manufacturer = item.manufacturer.toStdString();
        m_profile.spec.model_name = item.modelName.toStdString();
        m_profile.spec.accuracy_class = item.accuracyClass.toStdString();

        // Apply the database device archive calibration.
        m_profile.perspective_correction_enabled = item.perspectiveEnabled;
        m_profile.perspective_pts = {
        cv::Point2f(static_cast<float>(item.pt0_x), static_cast<float>(item.pt0_y)),
        cv::Point2f(static_cast<float>(item.pt1_x), static_cast<float>(item.pt1_y)),
        cv::Point2f(static_cast<float>(item.pt2_x), static_cast<float>(item.pt2_y)),
        cv::Point2f(static_cast<float>(item.pt3_x), static_cast<float>(item.pt3_y))
        };
        m_profile.scalePoints = item.scalePoints;
        m_profile.center = cv::Point2f(static_cast<float>(item.centerX), static_cast<float>(item.centerY));
        m_profile.pointerBaseRadius = item.pointerBaseRadius;
        m_profile.calibrated_width = item.calibratedWidth;
        m_profile.calibrated_height = item.calibratedHeight;
        m_profile.auto_detect_roi = false;
        m_profileReady = true;
        m_cfgRange->setText(tr("量程范围：%1 ～ %2 %3").arg(m_profile.min_val).arg(m_profile.max_val).arg(QString::fromStdString(m_profile.unit)));
    });
    watcher->setFuture(QtConcurrent::run([profileId]() -> ProfileReply {
        try { return {MySqlRepository().loadTemplate(profileId).definition, QString()}; }
        catch (const std::exception& error) { return {QJsonObject(), QString::fromUtf8(error.what())}; }
    }));
}
void MainWindow::onToggleLiveStream() {
    m_isLiveStreaming = !m_isLiveStreaming;
    if (m_isLiveStreaming) {
        if (m_liveTimer && !m_liveTimer->isActive()) m_liveTimer->start(40);
        m_toggleLiveBtn->setText(tr("⏸ 暂停实时预览"));
        m_toggleLiveBtn->setStyleSheet("QPushButton { background-color: #f57c00; color: white; font-weight: bold; padding: 6px 14px; border-radius: 4px; }"
                                       "QPushButton:hover { background-color: #ff9800; }");
        onLogReceived(tr("[STREAM] 实时视频流预览已开启。"));
    } else {
        m_toggleLiveBtn->setText(tr("▶ 开启实时流"));
        m_toggleLiveBtn->setStyleSheet("QPushButton { background-color: #0091ea; color: white; font-weight: bold; padding: 6px 14px; border-radius: 4px; }"
                                       "QPushButton:hover { background-color: #00b0ff; }");
        onLogReceived(tr("[STREAM] 实时视频流预览已暂停。"));
    }
}

void MainWindow::onCaptureCameraSnapshot() {
    auto task = makeSingleTask();
    task.inputKind = InspectionInputKind::CameraSnapshot;
    task.captureOnly = true;
    const auto frame = m_previewService->snapshot(task.device.cameraId);
    if (!frame.ok()) {
        onLogReceived(tr("[CAPTURE] 当前没有有效的实时帧，请开启实时预览并等待画面更新后再抓拍。"));
        return;
    }
    task.previewSnapshot = frame.frame;
    task.previewCapturedAt = QDateTime::fromMSecsSinceEpoch(
        std::chrono::duration_cast<std::chrono::milliseconds>(frame.capturedAt.time_since_epoch()).count(), Qt::UTC);
    submitSingleTask(task);
}

void MainWindow::onSaveSnapshotImage() {
    if (m_lastCapturedFrame.empty()) {
        QMessageBox::warning(this, tr("保存失败"), tr("当前没有已抓取的快照图像！"));
        return;
    }

    QString defaultName = QString("snapshot_%1.png")
                              .arg(QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss"));

    QString filePath = QFileDialog::getSaveFileName(
        this,
        tr("保存快照图像"),
        defaultName,
        tr("PNG 图像 (*.png);;JPEG 图像 (*.jpg *.jpeg);;BMP 图像 (*.bmp);;所有文件 (*.*)"));

    if (filePath.isEmpty()) {
        return;
    }

    m_saveSnapshotBtn->setEnabled(false);
    m_singleService->exportImage(m_lastCapturedFrame, filePath);
}

void MainWindow::onUseSnapshotForRecognition() {
    if (!m_hasSnapshotInput) return;
    auto task = m_snapshotInput;
    task.captureOnly = false;
    submitSingleTask(task);
}

void MainWindow::onTabChanged(int index) {
    if (index == 0) {
        onLogReceived(tr("[STREAM] 画面切回实时流监控页 (Stage 0)，恢复画面实时重绘。"));
        QTimer::singleShot(30, this, &MainWindow::onUpdateLiveStreamFrame);
    } else {
        onLogReceived(tr("[STREAM] 切换至算法调试/识别结果页，已暂停 GUI 界面重绘（底层视频流保持实时接收）以降低 CPU/GPU 损耗。"));
    }
}

void MainWindow::onUpdateLiveStreamFrame() {
    if ((m_isLiveStreaming || !m_pendingCalibrationCamera.isEmpty()) && m_previewService) m_previewService->requestFrame();
}

void MainWindow::onLogReceived(const QString& logMsg) {
    if (!m_logConsole) return;
    QString timeStr = QDateTime::currentDateTime().toString("HH:mm:ss.zzz");
    m_logConsole->append(QString("[%1] %2").arg(timeStr).arg(logMsg));
}

void MainWindow::onSelectImage() {
    if (m_singleService->isRunning()) return;
    const QString fileName = QFileDialog::getOpenFileName(this, tr("选择本地图像"), "",
        tr("图像 (*.png *.jpg *.jpeg *.bmp)"));
    if (fileName.isEmpty()) return;
    auto task = makeSingleTask();
    task.inputKind = InspectionInputKind::LocalImage;
    task.localImagePath = QFileInfo(fileName).absoluteFilePath();
    task.captureOnly = true;
    submitSingleTask(task);
}

void MainWindow::onManageGaugeTemplates() {
    GaugeTemplateDialog dialog(this);
    connect(&dialog, &GaugeTemplateDialog::templatesChanged, this, [this]() {
        const auto id = makeSingleTask().device.cameraId;
        if (m_discoveredItemsMap.contains(id)) applyCameraCalibrationAndProfile(id);
        onLogReceived(tr("[TEMPLATE] 模板已保存到数据库，后续任务将读取更新后的参数。"));
    });
    dialog.exec();
}
void MainWindow::onCalibrateCardinalPoints() {
    const QString cameraId = makeSingleTask().device.cameraId;
    if (!m_discoveredItemsMap.contains(cameraId)) return;
    m_calibrationChecked.remove(cameraId);
    m_pendingCalibrationCamera = cameraId;
    m_statusLabel->setText(tr("正在获取设备 %1 的实时标定底图…").arg(cameraId));
    checkCameraCalibration(cameraId);
    tryOpenCameraCalibration();
    m_previewService->requestFrame();
}

void MainWindow::checkCameraCalibration(const QString& cameraId) {
    if (cameraId != makeSingleTask().device.cameraId || m_calibrationChecked.contains(cameraId) ||
        m_calibrationQueries.contains(cameraId) || !m_registryLoaded) return;
    m_calibrationQueries.insert(cameraId);
    const auto generation = m_cameraConnectionGeneration;
    using Reply = QPair<DiscoveredCameraItem, QString>;
    auto* watcher = new QFutureWatcher<Reply>(this);
    connect(watcher, &QFutureWatcher<Reply>::finished, this, [this, watcher, cameraId, generation]() {
        const auto reply = watcher->result();
        watcher->deleteLater();
        if (generation != m_cameraConnectionGeneration) return;
        m_calibrationQueries.remove(cameraId);
        m_calibrationChecked.insert(cameraId);
        if (!reply.second.isEmpty()) {
            onLogReceived(tr("[CALIBRATION] 设备 %1 标定查询失败: %2；请保存设备档案后从右键菜单重试。").arg(cameraId, reply.second));
            m_statusLabel->setText(tr("设备 %1 标定查询失败，请从右键菜单重试。").arg(cameraId));
            m_pendingCalibrationCamera.clear();
            return;
        }
        // Keep connection credentials from the in-memory archive.
        auto& item = m_discoveredItemsMap[cameraId];
        const auto& saved = reply.first;
        item.hasCalibration = saved.hasCalibration;
        item.perspectiveEnabled = saved.perspectiveEnabled;
        item.calibratedWidth = saved.calibratedWidth; item.calibratedHeight = saved.calibratedHeight;
        item.pt0_x = saved.pt0_x; item.pt0_y = saved.pt0_y;
        item.pt1_x = saved.pt1_x; item.pt1_y = saved.pt1_y;
        item.pt2_x = saved.pt2_x; item.pt2_y = saved.pt2_y;
        item.pt3_x = saved.pt3_x; item.pt3_y = saved.pt3_y;
        item.centerX = saved.centerX; item.centerY = saved.centerY;
        item.pointerBaseRadius = saved.pointerBaseRadius;
        item.scalePoints = saved.scalePoints;
        if (auto* row = findListItemByCameraId(cameraId)) updateDeviceListItemWidget(row, item);
        if (!saved.hasCalibration) {
            m_pendingCalibrationCamera = cameraId;
            onLogReceived(tr("[CALIBRATION] 设备 %1 未保存表计标定，请手动标定。").arg(cameraId));
        }
        tryOpenCameraCalibration();
    });
    watcher->setFuture(QtConcurrent::run([cameraId]() -> Reply {
        try {
            DeviceConfigManager::instance().waitForDatabaseWrites();
            return {MySqlRepository().loadDevice(cameraId), QString()};
        } catch (const std::exception& error) { return {{}, QString::fromUtf8(error.what())}; }
    }));
}

void MainWindow::tryOpenCameraCalibration() {
    const QString cameraId = m_pendingCalibrationCamera;
    if (cameraId.isEmpty() || cameraId != makeSingleTask().device.cameraId || m_calibrationDialogOpen ||
        !m_calibrationChecked.contains(cameraId) || m_calibrationQueries.contains(cameraId)) return;
    const auto frame = m_previewService->snapshot(cameraId);
    if (!frame.ok()) return;
    m_pendingCalibrationCamera.clear();
    m_calibrationDialogOpen = true;
    // Queue the modal window outside the frame callback. Freeze this camera's real frame.
    QTimer::singleShot(0, this, [this, cameraId, frame]() {
        if (cameraId != makeSingleTask().device.cameraId) {
            m_calibrationDialogOpen = false;
            return;
        }
        const auto device = m_discoveredItemsMap.value(cameraId);
        GaugeProfile profile = m_profile;
        profile.scalePoints = {};
        profile.perspective_correction_enabled = device.hasCalibration;
        if (device.hasCalibration) {
            const float sx = device.calibratedWidth > 0 ? float(frame.frame.cols) / device.calibratedWidth : 1;
            const float sy = device.calibratedHeight > 0 ? float(frame.frame.rows) / device.calibratedHeight : 1;
            profile.perspective_pts = {{float(device.pt0_x)*sx, float(device.pt0_y)*sy},
                {float(device.pt1_x)*sx, float(device.pt1_y)*sy},
                {float(device.pt2_x)*sx, float(device.pt2_y)*sy}, {float(device.pt3_x)*sx, float(device.pt3_y)*sy}};
            profile.center = {float(device.centerX)*sx, float(device.centerY)*sy};
            profile.scalePoints = ScaleCalibration::scaled(device.scalePoints,sx,sy);
            profile.pointerBaseRadius = device.pointerBaseRadius * (sx+sy)/2;
        } else {
            profile.perspective_pts.clear();
            profile.center = {-1, -1};
        }
        CardinalSelectorDialog dialog(frame.frame, profile, cameraId, this);
        if (!device.hasCalibration)
            dialog.setWindowTitle(tr("设备 %1 尚未标定 — 请完成表计标定并保存").arg(cameraId));
        if (dialog.exec() == QDialog::Accepted) {
            m_discoveredItemsMap[cameraId] = DeviceConfigManager::instance().getDevice(cameraId);
            if (auto* row = findListItemByCameraId(cameraId))
                updateDeviceListItemWidget(row, m_discoveredItemsMap.value(cameraId));
            if (m_analysisInput.device.cameraId == cameraId && !m_analysisInput.useHistoricalConfiguration)
                m_analysisInput.device = m_discoveredItemsMap.value(cameraId);
            if (m_snapshotInput.device.cameraId == cameraId)
                m_snapshotInput.device = m_discoveredItemsMap.value(cameraId);
            if (cameraId == makeSingleTask().device.cameraId) applyCameraCalibrationAndProfile(cameraId);
            onLogReceived(tr("[CALIBRATION] 设备 %1 的表计标定已保存到 MySQL。").arg(cameraId));
        }
        m_calibrationDialogOpen = false;
    });
}

void MainWindow::onEditCameraInfo(QListWidgetItem *item) {
    if (!item) return;

    QString camId = item->data(Qt::UserRole).toString();
    if (camId.isEmpty()) return;

    CameraInfoEditDialog dialog(camId, this, false);
    if (dialog.exec() == QDialog::Accepted) {
        QString newCamId = dialog.getUpdatedCameraId();
        onLogReceived(tr("[CAMERA] 已成功更新摄像头节点 [%1] 网络连接与属性档案。").arg(newCamId));

        loadAndRestoreSavedDevices();

        QListWidgetItem *it = findListItemByCameraId(newCamId);
        if (it) {
            m_deviceListWidget->clearSelection();
            m_deviceListWidget->setCurrentItem(nullptr);
        }
    }
}

InspectionTask MainWindow::makeSingleTask() const {
    InspectionTask task;
    if (m_deviceListWidget && m_deviceListWidget->currentItem())
        task.device = m_discoveredItemsMap.value(m_deviceListWidget->currentItem()->data(Qt::UserRole).toString());
    else {
        task.device.cameraId = "LOCAL_ANALYSIS";
        task.device.protocolType = CameraProtocolType::LOCAL_FILE;
    }
    return task;
}

void MainWindow::submitSingleTask(InspectionTask task) {
    if (m_singleService->isRunning()) return;
    if (task.inputKind != InspectionInputKind::HistoricalImage || !task.useHistoricalConfiguration) {
        if (m_discoveredItemsMap.contains(task.device.cameraId))
            task.device = m_discoveredItemsMap.value(task.device.cameraId);
    }
    if (requiresInspectionCalibration(task)) {
        const auto message = tr("设备 %1 尚未完成表计标定，不能进行识别。\n请在设备列表中右键该设备，选择“表计标定”并保存。").arg(task.device.cameraId);
        m_statusLabel->setText(tr("请先完成设备 %1 的表计标定。").arg(task.device.cameraId));
        onLogReceived(tr("[CALIBRATION] %1").arg(message));
        QMessageBox::warning(this, tr("请先完成表计标定"), message);
        return;
    }
    task.outputDirectory = QCoreApplication::applicationDirPath() + "/inspection_snapshots";
    task.runMode = static_cast<GaugeRunMode>(m_runModeCombo->currentData().toInt());
    task.returnImages = true;
    m_singleSubmittedTask = task;
    m_startTime = std::chrono::high_resolution_clock::now();
    if (!m_singleService->start({task})) return;
    m_runBtn->setEnabled(false);
    m_captureBtn->setEnabled(false);
    m_snapAndRunBtn->setEnabled(false);
    m_browseImgBtn->setEnabled(false);
    m_statusLabel->setText(tr("后台处理中: %1 | 设备: %2")
        .arg(inspectionInputName(task.inputKind)).arg(task.device.cameraId));
}

void MainWindow::onRunRecognition() {
    if (!m_hasAnalysisInput) {
        m_statusLabel->setText(tr("请先明确选择本地图像、实时抓拍或历史原图。"));
        return;
    }
    auto task = m_analysisInput;
    task.captureOnly = false;
    submitSingleTask(task);
}

void MainWindow::onRecognitionFinished() {
    auto tEnd = std::chrono::high_resolution_clock::now();
    double elapsedMs = std::chrono::duration<double, std::milli>(tEnd - m_startTime).count();

    m_runBtn->setEnabled(true);

    m_confidenceLabel->setToolTip(qualitySummary(m_latestResult.recognitionQuality) + "\n" + QString::fromUtf8(QJsonDocument(m_latestResult.recognitionQuality).toJson()));
    if (auto label = findChild<QLabel*>("recognitionQuality")) {
        label->setText(qualitySummary(m_latestResult.recognitionQuality));
        label->setToolTip(qualityDetails(m_latestResult.recognitionQuality));
    }
    onLogReceived(qualitySummary(m_latestResult.recognitionQuality));
    // Show the core images, including the actual grayscale detector input.
    updateImageCard(m_imgStage1, m_latestResult.step1_original, "1. Original Stream Frame");
    updateImageCard(m_imgStage2, m_latestResult.step2_rectified, "2. FOV Rectified Image");
    updateImageCard(m_imgStage3, m_latestResult.step3_gray, "3. Grayscale Input Image");
    updateImageCard(m_imgStage4, m_latestResult.step4_recognition, "4. Final Recognition Result");

    // Clear diagnostics from a preceding debug run when the current mode omits them.
    if (m_latestResult.runMode != GaugeRunMode::Debug && m_diagnosticImgLabel) {
        m_diagnosticImgLabel->clear();
        if (m_diagnosticTextEdit) m_diagnosticTextEdit->clear();
    }
    // If diagnostic window is open, refresh its content automatically
    if (m_diagnosticDialog && m_diagnosticDialog->isVisible()) {
        if (m_diagnosticImgLabel && !m_latestResult.diagnosticHudImage.empty()) {
            QImage qimg = cvMatToQImage(m_latestResult.diagnosticHudImage);
            m_diagnosticImgLabel->setPixmap(QPixmap::fromImage(qimg));
        }
    }


    m_timeLabel->setText(QString(tr("算法耗时: %.2f ms")).arg(elapsedMs));

    if (m_latestResult.success) {
        m_valLabel->setText(QString(tr("<b>读取数值:</b> <font color='#00e676' size='+2'>%.3f %1</font>"))
                                .arg(QString::fromStdString(m_latestResult.unit)).arg(m_latestResult.value));
        m_angleLabel->setText(QString(tr("指针角度: %.1f°")).arg(m_latestResult.pointer_angle_deg));
        m_centerLabel->setText(QString(tr("表盘中心: (%.1f, %.1f) px  R=%.1f")).arg(m_latestResult.center.x).arg(m_latestResult.center.y).arg(m_latestResult.radius));
        m_confidenceLabel->setText(QString(tr("识别置信度: %.1f %%")).arg(m_latestResult.confidence * 100.0));
        
        if (m_latestResult.centerInconsistent) {
            m_statusLabel->setText(QString(tr("<font color='#ffab00'>⚠️ 警告: %1</font>")).arg(QString::fromStdString(m_latestResult.warningMessage)));
            onLogReceived(QString::fromStdString(m_latestResult.warningMessage));
        } else if (!m_latestResult.warningMessage.empty()) {
            m_statusLabel->setText(QString(tr("<font color='#ffab00'>%1</font>")).arg(QString::fromStdString(m_latestResult.warningMessage)));
            onLogReceived(QString::fromStdString(m_latestResult.warningMessage));
        } else {
            m_statusLabel->setText(tr("<font color='#00e676'>系统状态: 指定任务识别完成，数据已输出</font>"));
        }

        onLogReceived(tr("[PIPELINE] 指定图像识别完成: 数值=%.3f %1, 耗时=%.2f ms。")
                          .arg(m_latestResult.value).arg(QString::fromStdString(m_latestResult.unit)).arg(elapsedMs));
    } else {
        QString errStep = QString::fromStdString(m_latestResult.errorStep);
        QString errMsg = QString::fromStdString(m_latestResult.errorMessage);
        if (errMsg.isEmpty()) errMsg = tr("未知识别错误 (置信度不足或未找到指针)");

        m_statusLabel->setText(QString(tr("<font color='#ff5252'>❌ 识别失败 [%1]: %2</font>")).arg(errStep.isEmpty() ? tr("检测阶段") : errStep).arg(errMsg));
        m_valLabel->setText(tr("<b>读取数值:</b> <font color='#ff5252'>-- (识别失败)</font>"));
        m_angleLabel->setText(QString(tr("指针角度: %.1f° (未通过校验)")).arg(m_latestResult.pointer_angle_deg));
        m_centerLabel->setText(QString(tr("表盘中心: (%.1f, %.1f) px  R=%.1f")).arg(m_latestResult.center.x).arg(m_latestResult.center.y).arg(m_latestResult.radius));
        m_confidenceLabel->setText(QString(tr("识别置信度: <font color='#ff5252'>%.1f %% (未达门限)</font>")).arg(m_latestResult.confidence * 100.0));

        onLogReceived(tr("--------------------------------------------------"));
        onLogReceived(tr("❌ [PIPELINE ERROR] 仪表识别失败！"));
        if (!errStep.isEmpty()) onLogReceived(tr("  • 失败环节: %1").arg(errStep));
        onLogReceived(tr("  • 错误原因: %1").arg(errMsg));
        onLogReceived(tr("  • 轴心与表盘: 中心=(%.1f, %.1f), 半径=%.1f px").arg(m_latestResult.center.x).arg(m_latestResult.center.y).arg(m_latestResult.radius));
        if (!m_latestResult.detailedDebugInfo.empty()) {
            onLogReceived(tr("  • 详细算法诊断信息:"));
            QStringList lines = QString::fromStdString(m_latestResult.detailedDebugInfo).split('\n', QString::SkipEmptyParts);
            for (const QString& line : lines) {
                onLogReceived(QString("    %1").arg(line));
            }
        }
        onLogReceived(tr("💡 提示: 请查看检测输入灰度图、识别结果和指针算法诊断信息排查问题"));
        onLogReceived(tr("--------------------------------------------------"));
    }

}

void MainWindow::updateImageCard(QLabel* label, const cv::Mat& mat, const QString& title) {
    Q_UNUSED(title);
    if (!label || mat.empty()) return;
    QImage img = cvMatToQImage(mat);
    QPixmap pixmap = QPixmap::fromImage(img);

    QSize targetSize = label->size();
    if (targetSize.width() <= 20 || targetSize.height() <= 20) {
        if (label->parentWidget() && label->parentWidget()->width() > 50 && label->parentWidget()->height() > 50) {
            targetSize = label->parentWidget()->size();
        } else {
            targetSize = QSize(640, 480);
        }
    }

    label->setPixmap(pixmap.scaled(targetSize, Qt::KeepAspectRatio, Qt::SmoothTransformation));
}

QImage MainWindow::cvMatToQImage(const cv::Mat& mat) {
    if (mat.empty()) return QImage();

    switch (mat.type()) {
        case CV_8UC1: {
            QImage img(mat.data, mat.cols, mat.rows, static_cast<int>(mat.step), QImage::Format_Grayscale8);
            return img.copy();
        }
        case CV_8UC3: {
            QImage img(mat.data, mat.cols, mat.rows, static_cast<int>(mat.step), QImage::Format_BGR888);
            return img.copy();
        }
        case CV_8UC4: {
            QImage img(mat.data, mat.cols, mat.rows, static_cast<int>(mat.step), QImage::Format_ARGB32);
            return img.copy();
        }
        default:
            return QImage();
    }
}

void MainWindow::onShowDiagnosticWindow() {
    if (!m_diagnosticDialog) {
        m_diagnosticDialog = new QDialog(this);
        m_diagnosticDialog->setWindowTitle(tr("指针识别算法诊断信息 (Diagnostic HUD)"));
        m_diagnosticDialog->resize(620, 520);
        m_diagnosticDialog->setStyleSheet("QDialog { background-color: #0d1117; color: #c9d1d9; }");

        QVBoxLayout *dlgLayout = new QVBoxLayout(m_diagnosticDialog);
        dlgLayout->setContentsMargins(12, 12, 12, 12);
        dlgLayout->setSpacing(10);

        QLabel *titleLabel = new QLabel(tr("📐 Pointer Edge & Line Fit Diagnostic HUD"), m_diagnosticDialog);
        titleLabel->setStyleSheet("color: #58a6ff; font-size: 14px; font-weight: bold; padding: 4px 0;");
        dlgLayout->addWidget(titleLabel);

        QScrollArea *scrollArea = new QScrollArea(m_diagnosticDialog);
        scrollArea->setWidgetResizable(true);
        scrollArea->setStyleSheet("QScrollArea { border: 1px solid #30363d; background-color: #0b0e14; border-radius: 4px; }");

        m_diagnosticImgLabel = new QLabel(scrollArea);
        m_diagnosticImgLabel->setAlignment(Qt::AlignCenter);
        scrollArea->setWidget(m_diagnosticImgLabel);
        dlgLayout->addWidget(scrollArea, 1);

        QHBoxLayout *btnLayout = new QHBoxLayout();
        QLabel *tipLabel = new QLabel(tr("💡 诊断信息随识别实时刷新，不遮挡被识别结果叠加图。"), m_diagnosticDialog);
        tipLabel->setStyleSheet("color: #8b949e; font-size: 11px;");
        btnLayout->addWidget(tipLabel);
        btnLayout->addStretch();

        QPushButton *closeBtn = new QPushButton(tr("关闭窗口"), m_diagnosticDialog);
        closeBtn->setStyleSheet("QPushButton { background-color: #21262d; color: #c9d1d9; border: 1px solid #30363d; padding: 6px 18px; border-radius: 4px; font-weight: bold; }"
                                "QPushButton:hover { background-color: #30363d; color: #ffffff; }");
        connect(closeBtn, &QPushButton::clicked, m_diagnosticDialog, &QDialog::hide);
        btnLayout->addWidget(closeBtn);

        dlgLayout->addLayout(btnLayout);
    }

    // Refresh content with latest result if available
    if (!m_latestResult.diagnosticHudImage.empty()) {
        QImage qimg = cvMatToQImage(m_latestResult.diagnosticHudImage);
        m_diagnosticImgLabel->setPixmap(QPixmap::fromImage(qimg));
    } else {
        m_diagnosticImgLabel->setText(tr("<div align='center' style='color:#8b949e; padding:40px; font-size:14px;'>"
                                         "暂无诊断信息<br><br>请点击【▶ 运行】完成一次仪表识别即可在此查看完整诊断数据。</div>"));
    }

    m_diagnosticDialog->show();
    m_diagnosticDialog->raise();
    m_diagnosticDialog->activateWindow();
}

QWidget* MainWindow::createInspectionStageArea(QWidget *parent) {
    QWidget *container = new QWidget(parent);
    QVBoxLayout *layout = new QVBoxLayout(container);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(10);

    // 1. Top Control & Summary Bar
    QGroupBox *ctrlBox = new QGroupBox(tr("🚀 全站表盘读数自动巡检控制台 (All Devices Batch Inspection Control)"), container);
    QHBoxLayout *ctrlLayout = new QHBoxLayout(ctrlBox);
    ctrlLayout->setContentsMargins(10, 8, 10, 8);

    m_startPatrolBtn = new QPushButton(tr("🚀 启动全站批量巡检"), ctrlBox);
    m_startPatrolBtn->setStyleSheet("QPushButton { background-color: #00897b; color: white; font-weight: bold; font-size: 13px; padding: 7px 16px; border-radius: 4px; }"
                                   "QPushButton:hover { background-color: #00bfa5; }");

    m_stopPatrolBtn = new QPushButton(tr("⏹ 停止巡检"), ctrlBox);
    m_stopPatrolBtn->setEnabled(false);
    m_stopPatrolBtn->setStyleSheet("QPushButton { background-color: #d32f2f; color: white; font-weight: bold; padding: 7px 14px; border-radius: 4px; }"
                                  "QPushButton:hover { background-color: #f44336; }");

    m_autoPatrolCheck = new QCheckBox(tr("⏱ 开启定时轮巡 (每10分钟自动执行)"), ctrlBox);
    m_autoPatrolCheck->setStyleSheet("QCheckBox { color: #80deea; font-size: 12px; font-weight: bold; }");
    connect(m_autoPatrolCheck, &QCheckBox::toggled, this, [this](bool checked) {
        if (checked) {
            m_autoPatrolTimer->start(10 * 60 * 1000);
            onLogReceived(tr("[PATROL] ⏱ 已启动定时自动巡视轮询，周期: 10 分钟。"));
        } else {
            m_autoPatrolTimer->stop();
            onLogReceived(tr("[PATROL] ⏱ 已关闭定时自动巡视轮询。"));
        }
    });

    m_exportReportBtn = new QPushButton(tr("📥 导出巡检数据报表 (CSV)"), ctrlBox);
    m_exportReportBtn->setStyleSheet("QPushButton { background-color: #37474f; color: #00e5ff; font-weight: bold; border: 1px solid #00acc1; padding: 7px 14px; border-radius: 4px; }"
                                    "QPushButton:hover { background-color: #455a64; color: #ffffff; }");

    ctrlLayout->addWidget(m_startPatrolBtn);
    ctrlLayout->addWidget(m_stopPatrolBtn);
    ctrlLayout->addWidget(m_autoPatrolCheck);
    ctrlLayout->addStretch();
    ctrlLayout->addWidget(m_exportReportBtn);

    layout->addWidget(ctrlBox);

    // 2. Summary Badge & Progress Bar
    QHBoxLayout *statusLayout = new QHBoxLayout();
    m_inspectionSummaryLabel = new QLabel(tr("📊 巡检状态: 待开始 | 监控节点总数: 0 台 | 正常: 0 | 待复核: 0"), container);
    m_inspectionSummaryLabel->setStyleSheet("color: #00e5ff; font-size: 13px; font-weight: bold; padding: 4px;");

    m_inspectionProgressBar = new QProgressBar(container);
    m_inspectionProgressBar->setRange(0, 100);
    m_inspectionProgressBar->setValue(0);
    m_inspectionProgressBar->setTextVisible(true);
    m_inspectionProgressBar->setStyleSheet(
        "QProgressBar { background-color: #1a1a24; border: 1px solid #2b2e3e; border-radius: 4px; text-align: center; color: white; height: 20px; font-weight: bold; }"
        "QProgressBar::chunk { background-color: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #00897b, stop:1 #00e5ff); border-radius: 3px; }"
    );

    statusLayout->addWidget(m_inspectionSummaryLabel);
    statusLayout->addWidget(m_inspectionProgressBar, 1);
    layout->addLayout(statusLayout);

    // 3. Inspection Data Table
    m_inspectionTable = new QTableWidget(container);
    m_inspectionTable->setColumnCount(10);
    QStringList headers = {
        tr("序号"), tr("设备编号"), tr("测点名称"), tr("安装区域/位置"),
        tr("最新读数"), tr("指针偏角"), tr("置信度"), tr("巡检状态"), tr("更新时间"),
        tr("📸 过程分析")
    };
    m_inspectionTable->setHorizontalHeaderLabels(headers);
    m_inspectionTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_inspectionTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_inspectionTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
    m_inspectionTable->horizontalHeader()->setSectionResizeMode(9, QHeaderView::ResizeToContents);
    m_inspectionTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_inspectionTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_inspectionTable->setAlternatingRowColors(true);
    m_inspectionTable->setStyleSheet(
        "QTableWidget { background-color: #14151c; alternate-background-color: #1a1b26; color: #e0e0e0; gridline-color: #272a38; border: 1px solid #272a38; border-radius: 4px; font-size: 13px; }"
        "QHeaderView::section { background-color: #1f2230; color: #00e5ff; font-weight: bold; padding: 6px; border: 1px solid #272a38; }"
        "QTableWidget::item:selected { background-color: #005b64; color: #ffffff; }"
    );

    layout->addWidget(m_inspectionTable, 1);

    // Connections
    connect(m_startPatrolBtn, &QPushButton::clicked, this, &MainWindow::onStartAllDevicesInspection);
    connect(m_stopPatrolBtn, &QPushButton::clicked, this, &MainWindow::onStopAllDevicesInspection);
    connect(m_exportReportBtn, &QPushButton::clicked, this, &MainWindow::onExportPatrolReport);
    connect(m_inspectionTable, &QTableWidget::cellDoubleClicked, this, &MainWindow::onInspectionTableItemDoubleClicked);

    return container;
}

void MainWindow::onCaptureAndRecognizeCurrent() {
    auto task = makeSingleTask();
    task.inputKind = InspectionInputKind::CameraSnapshot;
    submitSingleTask(task);
}

void MainWindow::onStartAllDevicesInspection() {
    if (m_isPatrolRunning) return;

    if (!DeviceConfigManager::instance().load()) {
        const auto message = tr("无法读取设备档案及标定数据，本次巡检未启动：%1").arg(DeviceConfigManager::instance().lastError());
        onLogReceived(tr("[PATROL] %1").arg(message));
        m_inspectionSummaryLabel->setText(message);
        if (sender() != m_autoPatrolTimer) QMessageBox::warning(this, tr("巡检未启动"), message);
        return;
    }
    QList<DiscoveredCameraItem> devices = DeviceConfigManager::instance().getAllDevices();
    if (devices.isEmpty()) {
        QMessageBox::information(this, tr("提示"), tr("当前设备库为空，请先添加摄像头设备！"));
        return;
    }

    QStringList uncalibrated;
    for (auto it = devices.begin(); it != devices.end();) {
        if (!canRecognizeDevice(*it)) {
            uncalibrated.append(it->cameraId);
            it = devices.erase(it);
        } else ++it;
    }
    if (!uncalibrated.isEmpty()) {
        const auto message = tr("以下设备尚未完成表计标定，已排除在本次全站批量巡检之外：\n%1\n请右键对应设备，完成“表计标定”并保存。").arg(uncalibrated.join("\n"));
        onLogReceived(tr("[CALIBRATION] %1").arg(message));
        m_inspectionSummaryLabel->setText(tr("未标定设备已跳过：%1 台；请先完成表计标定。").arg(uncalibrated.size()));
        if (sender() != m_autoPatrolTimer)
            QMessageBox::warning(this, tr("请先完成表计标定"), message);
        if (devices.isEmpty()) return;
    }

    m_isPatrolRunning = true;
    m_patrolCameraIdQueue.clear();
    m_inspectionRecords.clear();

    m_inspectionTable->setRowCount(devices.size());

    for (int i = 0; i < devices.size(); ++i) {
        const auto& dev = devices[i];
        m_patrolCameraIdQueue.append(dev.cameraId);

        InspectionRecord rec;
        rec.cameraId = dev.cameraId;
        rec.device = dev;
        rec.customName = dev.customName.isEmpty() ? dev.modelName : dev.customName;
        rec.locationInfo = QString("%1 | %2 | %3").arg(dev.substation).arg(dev.area).arg(dev.installationLocation);
        rec.unit = "MPa";
        rec.status = tr("⏳ 排队巡视中...");
        rec.timestamp = "-";
        m_inspectionRecords.append(rec);

        // Populate table row
        m_inspectionTable->setItem(i, 0, new QTableWidgetItem(QString::number(i + 1)));
        m_inspectionTable->setItem(i, 1, new QTableWidgetItem(rec.cameraId));
        m_inspectionTable->setItem(i, 2, new QTableWidgetItem(rec.customName));
        m_inspectionTable->setItem(i, 3, new QTableWidgetItem(rec.locationInfo));
        m_inspectionTable->setItem(i, 4, new QTableWidgetItem("--"));
        m_inspectionTable->setItem(i, 5, new QTableWidgetItem("--"));
        m_inspectionTable->setItem(i, 6, new QTableWidgetItem("--"));
        m_inspectionTable->setItem(i, 7, new QTableWidgetItem(rec.status));
        m_inspectionTable->setItem(i, 8, new QTableWidgetItem(rec.timestamp));

        QPushButton *viewBtn = new QPushButton(tr("🖼️ 查看照片"), m_inspectionTable);
        viewBtn->setStyleSheet(
            "QPushButton { background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #1a365d, stop:1 #2b6cb0); "
            "color: #90cdf4; border: 1px solid #3182ce; border-radius: 4px; padding: 3px 8px; font-weight: bold; font-size: 12px; } "
            "QPushButton:hover { background: #3182ce; color: #ffffff; }"
        );
        connect(viewBtn, &QPushButton::clicked, this, [this, i]() {
            showInspectionPhotosDialog(i);
        });
        m_inspectionTable->setCellWidget(i, 9, viewBtn);
    }

    m_patrolCurrentIndex = 0;
    m_startPatrolBtn->setEnabled(false);
    m_stopPatrolBtn->setEnabled(true);
    m_inspectionProgressBar->setValue(0);
    m_inspectionSummaryLabel->setText(tr("📊 正在全站巡检 (0/%1)...").arg(devices.size()));

    // Switch vision tab to inspection matrix
    for (int i = 0; i < m_visionTabWidget->count(); ++i) {
        if (m_visionTabWidget->tabText(i).contains(tr("巡检"))) {
            m_visionTabWidget->setCurrentIndex(i);
            break;
        }
    }

    onLogReceived(tr("[PATROL] 🚀 启动全站批量巡检流水线，待巡检设备总计: %1 台。").arg(devices.size()));

    QList<InspectionTask> tasks;
    for (const auto& device : devices) {
        InspectionTask task;
        task.device = device;
        task.preferOnvifSnapshot = true;
        if (device.protocolType == CameraProtocolType::LOCAL_FILE) task.inputKind = InspectionInputKind::LocalImage;
        task.runMode = static_cast<GaugeRunMode>(m_runModeCombo->currentData().toInt());
        task.localImagePath = m_hasAnalysisInput && m_analysisInput.inputKind == InspectionInputKind::LocalImage
            ? m_analysisInput.localImagePath : QString();
        task.outputDirectory = QCoreApplication::applicationDirPath() + "/inspection_snapshots";
        tasks.append(task);
    }
    m_batchPatrolBtn->setEnabled(false);
    m_inspectionService->start(tasks);
}

void MainWindow::onStopAllDevicesInspection() {
    if (!m_isPatrolRunning) return;
    m_inspectionService->stop();
    m_stopPatrolBtn->setEnabled(false);
    m_inspectionSummaryLabel->setText(tr("正在停止，等待在途任务退出..."));
}

void MainWindow::onInspectionOutcome(const InspectionOutcome& outcome) {
    if (!outcome.databaseError.isEmpty()) onLogReceived(tr("[MYSQL] 设备 %1 · 任务 %2 · %3 · 待同步: %4")
        .arg(outcome.cameraId,outcome.taskId,outcome.databaseError).arg(outcome.databasePending));
    const int row = m_patrolCameraIdQueue.indexOf(outcome.cameraId);
    if (row < 0 || row >= m_inspectionRecords.size()) return;
    auto& rec = m_inspectionRecords[row];
    rec.taskId = outcome.taskId;
    rec.capturedAt = outcome.capturedAt;
    const auto& res = outcome.result;
    rec.timestamp = outcome.capturedAt.isValid()
        ? outcome.capturedAt.toLocalTime().toString("yyyy-MM-dd HH:mm:ss")
        : QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
    rec.readingValue = res.value;
    rec.unit = res.unit;
    rec.pointerAngle = res.pointer_angle_deg;
    rec.confidence = res.confidence * 100.0;
    rec.status = !outcome.errorCode.isEmpty() ? tr("失败: %1").arg(outcome.errorCode)
        : (res.success && res.confidence >= 0.40 ? tr("识别成功") : tr("待复核"));
    if (outcome.errorCode == "NO_FRAME" && outcome.errorMessage.contains("uniform"))
        rec.status = tr("采集失败：视频仅返回灰色/空白帧");
    rec.step2Path = outcome.rectifiedPath;
    rec.originalPath = outcome.originalPath;
    rec.step4Path = outcome.recognitionPath;
    rec.diagnosticText = outcome.errorMessage.isEmpty()
        ? QString::fromUtf8(res.detailedDebugInfo.data(), static_cast<int>(res.detailedDebugInfo.size())) : outcome.errorMessage;
    const bool valid = res.success && outcome.errorCode.isEmpty();
    m_inspectionTable->item(row, 4)->setText(valid ? QString::asprintf("%.2f %s", rec.readingValue, rec.unit.c_str()) : "--");
    m_inspectionTable->item(row, 5)->setText(valid ? QString::asprintf("%.1f°", rec.pointerAngle) : "--");
    m_inspectionTable->item(row, 6)->setText(QString::asprintf("%.1f%%", rec.confidence));
    m_inspectionTable->item(row, 7)->setText(rec.status);
    onLogReceived(tr("[CAPTURE] 设备 %1 · 采集方式 %2").arg(outcome.cameraId, outcome.captureMethod));
    const auto& timings = outcome.timings;
    const auto engineSteps = timings["engine_steps"].toObject();
    QStringList details;
    for (auto it = engineSteps.begin(); it != engineSteps.end(); ++it)
        if (it.value().isDouble()) details.append(it.key() + "=" + QString::number(it.value().toDouble(), 'f', 1));
    onLogReceived(tr("[RECOGNITION TIMING] 设备 %1 · 模板读取 %2 ms · 配置准备 %3 ms · %4")
        .arg(outcome.cameraId).arg(timings["template_load_ms"].toDouble(), 0, 'f', 1)
        .arg(timings["profile_setup_ms"].toDouble(), 0, 'f', 1).arg(details.join("; ")));
    onLogReceived(tr("[POINTER TIMING] 设备 %1 · %2").arg(outcome.cameraId,
        QString::fromUtf8(QJsonDocument(engineSteps["pointer_details"].toObject()).toJson(QJsonDocument::Compact))));
    const auto timingText = tr("耗时(ms)：总计 %1；排队 %2；采集 %3；识别等待 %4；识别 %5；存储等待 %6；图片保存 %7；结果保存 %8；数据库 %9")
        .arg(timings["total_ms"].toDouble(), 0, 'f', 1)
        .arg(timings["queue_ms"].toDouble(), 0, 'f', 1)
        .arg(timings["capture_ms"].toDouble(), 0, 'f', 1)
        .arg(timings["recognition_queue_ms"].toDouble(), 0, 'f', 1)
        .arg(timings["recognition_ms"].toDouble(), 0, 'f', 1)
        .arg(timings["storage_queue_ms"].toDouble(), 0, 'f', 1)
        .arg(timings["image_save_ms"].toDouble(), 0, 'f', 1)
        .arg(timings["result_save_ms"].isDouble()
             ? QString::number(timings["result_save_ms"].toDouble(), 'f', 1) : tr("未完成"))
        .arg(timings["database_write_ms"].isDouble()
             ? QString::number(timings["database_write_ms"].toDouble(), 'f', 1) : tr("未执行"));
    if (timings["metrics_save_failed"].toBool())
        onLogReceived(tr("[TIMING] 设备 %1 · 任务 %2 · 耗时统计文件保存失败").arg(outcome.cameraId, outcome.taskId));
    rec.diagnosticText += "\n" + timingText;
    m_inspectionTable->item(row, 7)->setToolTip(rec.diagnosticText);
    onLogReceived(tr("[TIMING] 设备 %1 · 任务 %2 · %3").arg(outcome.cameraId, outcome.taskId, timingText));
    m_inspectionTable->item(row, 8)->setText(rec.timestamp);
    onLogReceived(tr("[PATROL] %1: %2").arg(outcome.cameraId, rec.status));
}

void MainWindow::onInspectionTableItemDoubleClicked(int row, int column) {
    Q_UNUSED(column);
    if (row < 0 || row >= m_inspectionRecords.size()) return;
    showInspectionPhotosDialog(row);
}

void MainWindow::onExportPatrolReport() {
    if (m_inspectionRecords.isEmpty()) {
        QMessageBox::information(this, tr("提示"), tr("当前无巡检记录可供导出！"));
        return;
    }

    QString defaultName = QString("Inspection_Report_%1.csv")
        .arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss"));
    QString savePath = QFileDialog::getSaveFileName(this, tr("导出巡检数据报表"), defaultName, tr("CSV 电子表格 (*.csv)"));
    if (savePath.isEmpty()) return;

    QFile file(savePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, tr("保存失败"), tr("无法创建并写入指定的导出文件！"));
        return;
    }

    QTextStream out(&file);
    out.setCodec("UTF-8");
    // UTF-8 BOM for Excel
    out << "\xEF\xBB\xBF";
    out << "序号,设备编号,测点名称,安装区域/位置,最新读数,单位,指针偏转角度,识别置信度,巡检状态,巡检时间戳\n";

    for (int i = 0; i < m_inspectionRecords.size(); ++i) {
        const auto& r = m_inspectionRecords[i];
        out << (i + 1) << ","
            << "\"" << r.cameraId << "\","
            << "\"" << r.customName << "\","
            << "\"" << r.locationInfo << "\","
            << r.readingValue << ","
            << "\"" << QString::fromStdString(r.unit) << "\","
            << r.pointerAngle << ","
            << r.confidence << "%,"
            << "\"" << r.status << "\","
            << "\"" << r.timestamp << "\"\n";
    }

    file.close();
    QMessageBox::information(this, tr("导出成功"), tr("已成功导出 %1 条巡检数据至:\n%2").arg(m_inspectionRecords.size()).arg(savePath));
    onLogReceived(tr("[EXPORT] 巡检报表已成功导出至: %1").arg(savePath));
}

void MainWindow::showInspectionPhotosDialog(int row) {
    if (row < 0 || row >= m_inspectionRecords.size()) return;
    const InspectionRecord rec = m_inspectionRecords[row]; // Freeze the reviewed task across timer updates.

    QDialog dlg(this);
    dlg.setWindowTitle(tr("📸 巡检识别过程图像深度分析 - %1 (%2)").arg(rec.cameraId).arg(rec.customName));
    dlg.resize(1180, 780);
    dlg.setMinimumSize(960, 640);
    dlg.setStyleSheet(R"(
        QDialog { background-color: #101117; color: #ffffff; }
        QLabel { color: #e0e0e0; font-family: 'Segoe UI', Microsoft YaHei, sans-serif; }
        QGroupBox { border: 1px solid #272a38; border-radius: 6px; margin-top: 10px; font-weight: bold; background-color: #151722; color: #00e5ff; }
        QGroupBox::title { subcontrol-origin: margin; subcontrol-position: top left; padding: 2px 8px; background-color: #151722; }
        QPushButton { background-color: #272a3a; border: 1px solid #3a3f58; border-radius: 4px; padding: 6px 14px; color: #ffffff; font-weight: bold; }
        QPushButton:hover { background-color: #35394e; border-color: #00e5ff; }
    )");

    QVBoxLayout *mainLayout = new QVBoxLayout(&dlg);
    mainLayout->setContentsMargins(14, 14, 14, 14);
    mainLayout->setSpacing(10);

    // 1. Top Header Card (Device Meta & Inspection Summary)
    QFrame *headerFrame = new QFrame(&dlg);
    headerFrame->setStyleSheet("background-color: #161824; border: 1px solid #282c3c; border-radius: 8px;");
    QVBoxLayout *headerVLayout = new QVBoxLayout(headerFrame);
    headerVLayout->setContentsMargins(12, 10, 12, 10);
    headerVLayout->setSpacing(8);

    QHBoxLayout *topInfoRow = new QHBoxLayout();
    QLabel *titleLabel = new QLabel(QString("📷 设备测点: <font color='#00e5ff'><b>%1</b></font> (%2)")
                                        .arg(rec.cameraId).arg(rec.customName), headerFrame);
    titleLabel->setStyleSheet("font-size: 16px; font-weight: bold;");
    topInfoRow->addWidget(titleLabel);

    topInfoRow->addStretch();

    QLabel *locBadge = new QLabel(QString("📍 %1").arg(rec.locationInfo), headerFrame);
    locBadge->setStyleSheet("background-color: #1a2233; color: #90caf9; padding: 4px 10px; border-radius: 4px; font-size: 12px;");
    topInfoRow->addWidget(locBadge);

    QLabel *statusBadge = new QLabel(QString("状态: %1").arg(rec.status), headerFrame);
    QString statusColor = (rec.status.contains("正常") || rec.status.contains("识别成功")) ? "#00e676" : (rec.status.contains("复核") ? "#ffb300" : "#f44336");
    statusBadge->setStyleSheet(QString("background-color: #1e293b; color: %1; font-weight: bold; padding: 4px 10px; border-radius: 4px; font-size: 12px;").arg(statusColor));
    topInfoRow->addWidget(statusBadge);

    headerVLayout->addLayout(topInfoRow);

    // Metrics Bar
    QHBoxLayout *metricsRow = new QHBoxLayout();
    metricsRow->setSpacing(12);

    auto makeMetricWidget = [&](const QString& label, const QString& val, const QString& color) -> QWidget* {
        QWidget *w = new QWidget(headerFrame);
        w->setStyleSheet("background-color: #111219; border: 1px solid #232635; border-radius: 4px; padding: 4px 8px;");
        QHBoxLayout *hl = new QHBoxLayout(w);
        hl->setContentsMargins(6, 4, 6, 4);
        QLabel *lbl = new QLabel(label, w);
        lbl->setStyleSheet("color: #8f95a5; font-size: 12px;");
        QLabel *vl = new QLabel(val, w);
        vl->setStyleSheet(QString("color: %1; font-weight: bold; font-size: 14px;").arg(color));
        hl->addWidget(lbl);
        hl->addWidget(vl);
        return w;
    };

    QString readingStr = (rec.readingValue == 0.0 && !(rec.status.contains("正常") || rec.status.contains("识别成功"))) ? "--" : QString::asprintf("%.2f %s", rec.readingValue, rec.unit.c_str());
    QString angleStr = (rec.pointerAngle == 0.0 && !(rec.status.contains("正常") || rec.status.contains("识别成功"))) ? "--" : QString::asprintf("%.1f°", rec.pointerAngle);
    QString confStr = (rec.confidence == 0.0 && !(rec.status.contains("正常") || rec.status.contains("识别成功"))) ? "--" : QString::asprintf("%.1f%%", rec.confidence);
    QString timeStr = rec.timestamp.isEmpty() ? "-" : rec.timestamp;

    metricsRow->addWidget(makeMetricWidget(tr("最新读数:"), readingStr, "#00e676"));
    metricsRow->addWidget(makeMetricWidget(tr("指针偏转角:"), angleStr, "#00e5ff"));
    metricsRow->addWidget(makeMetricWidget(tr("识别置信度:"), confStr, "#ffb300"));
    metricsRow->addWidget(makeMetricWidget(tr("巡检抓拍时间:"), timeStr, "#e0e0e0"));
    metricsRow->addStretch();

    headerVLayout->addLayout(metricsRow);
    mainLayout->addWidget(headerFrame);

    // 2. Load step2 and step4 Mats if not in memory but file exists on disk
    cv::Mat step2Mat = rec.step2_rectified;
    const bool showingOriginal = rec.step2Path.isEmpty() && !rec.originalPath.isEmpty();
    if (showingOriginal) step2Mat = cv::imread(rec.originalPath.toStdString());
    if (step2Mat.empty() && !rec.step2Path.isEmpty() && QFileInfo::exists(rec.step2Path)) {
        step2Mat = cv::imread(rec.step2Path.toStdString());
    }
    cv::Mat step4Mat = rec.step4_recognition;
    if (step4Mat.empty() && !rec.step4Path.isEmpty() && QFileInfo::exists(rec.step4Path)) {
        step4Mat = cv::imread(rec.step4Path.toStdString());
    }

    // 3. Center Two Images Side-by-Side Comparison
    QHBoxLayout *imagesLayout = new QHBoxLayout();
    imagesLayout->setSpacing(14);

    // Left Card: Step 2
    QGroupBox *step2Box = new QGroupBox(showingOriginal ? tr("原始采集图像") : tr("【步骤 2】切点视场透视校正正置图 (FOV Rectified Dial)"), &dlg);
    QVBoxLayout *step2Layout = new QVBoxLayout(step2Box);
    step2Layout->setContentsMargins(10, 10, 10, 10);

    QLabel *step2Desc = new QLabel(tr("📐 基于表盘4边缘切点透视展开，消除俯仰/侧倾视角畸变，展开为标准正面正置表盘"), step2Box);
    if (showingOriginal) step2Desc->setText(tr("本次读数对应的未经处理的原始抓拍图像"));
    step2Desc->setStyleSheet("color: #8c93a8; font-size: 11px; margin-bottom: 4px;");
    step2Layout->addWidget(step2Desc);

    QLabel *step2Label = new QLabel(step2Box);
    step2Label->setAlignment(Qt::AlignCenter);
    step2Label->setStyleSheet("background-color: #0b0c10; border: 1px solid #232635; border-radius: 6px;");
    step2Label->setMinimumSize(460, 400);

    if (!step2Mat.empty()) {
        QImage qimg = cvMatToQImage(step2Mat);
        QPixmap pix = QPixmap::fromImage(qimg);
        step2Label->setPixmap(pix.scaled(520, 440, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    } else {
        step2Label->setText(tr("<div align='center' style='color:#757575;'><br><br><br>📷 暂无步骤 2 校正图<br><br><span style='font-size:12px;'>该设备尚未完成巡检抓拍识别</span></div>"));
    }
    step2Layout->addWidget(step2Label, 1);
    imagesLayout->addWidget(step2Box, 1);

    // Right Card: Step 4
    QGroupBox *step4Box = new QGroupBox(tr("【步骤 4】最终仪表读数与指针识别图 (Recognition Result Overlay)"), &dlg);
    QVBoxLayout *step4Layout = new QVBoxLayout(step4Box);
    step4Layout->setContentsMargins(10, 10, 10, 10);

    QLabel *step4Desc = new QLabel(tr("🎯 表盘回转中心圆盘定位、指针直线拟合、零点与满量程刻度跨度映射与读数叠加"), step4Box);
    step4Desc->setStyleSheet("color: #8c93a8; font-size: 11px; margin-bottom: 4px;");
    step4Layout->addWidget(step4Desc);

    QLabel *step4Label = new QLabel(step4Box);
    step4Label->setAlignment(Qt::AlignCenter);
    step4Label->setStyleSheet("background-color: #0b0c10; border: 1px solid #232635; border-radius: 6px;");
    step4Label->setMinimumSize(460, 400);

    if (!step4Mat.empty()) {
        QImage qimg = cvMatToQImage(step4Mat);
        QPixmap pix = QPixmap::fromImage(qimg);
        step4Label->setPixmap(pix.scaled(520, 440, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    } else {
        step4Label->setText(tr("暂无识别标注图。\n生产模式仅保留原图，可打开原图按原始分辨率复核。"));
    }
    step4Layout->addWidget(step4Label, 1);
    imagesLayout->addWidget(step4Box, 1);

    mainLayout->addLayout(imagesLayout, 1);

    // 4. Diagnostic Info Section (if available)
    if (!rec.diagnosticText.isEmpty()) {
        QLabel *diagLabel = new QLabel(tr("ℹ️ 算法诊断与特征提取详情: %1").arg(rec.diagnosticText.trimmed().replace("\n", "  |  ")), &dlg);
        diagLabel->setStyleSheet("background-color: #12141d; color: #81d4fa; border: 1px solid #1e2230; border-radius: 4px; padding: 6px 10px; font-size: 11px;");
        diagLabel->setTextFormat(Qt::PlainText);
        diagLabel->setWordWrap(true);
        diagLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        mainLayout->addWidget(diagLabel);
    }

    // 5. Bottom Action Bar
    QHBoxLayout *actionLayout = new QHBoxLayout();
    actionLayout->setSpacing(10);

    QPushButton *originalBtn = new QPushButton(tr("查看原图（原始分辨率）"), &dlg);
    originalBtn->setEnabled(!rec.originalPath.isEmpty());
    connect(originalBtn, &QPushButton::clicked, &dlg, [this, rec, &dlg]() {
        const cv::Mat original = cv::imread(rec.originalPath.toStdString());
        if (original.empty()) {
            QMessageBox::warning(&dlg, tr("原图不可用"), tr("本次抓拍原图不存在或无法读取。"));
            return;
        }
        QDialog viewer(&dlg);
        viewer.setWindowTitle(tr("原图复核 | %1 | %2 | %3").arg(rec.cameraId, rec.timestamp, rec.taskId));
        viewer.resize(1000, 760);
        auto* layout = new QVBoxLayout(&viewer);
        auto* scroll = new QScrollArea(&viewer);
        auto* image = new QLabel;
        image->setPixmap(QPixmap::fromImage(cvMatToQImage(original)));
        scroll->setWidget(image);
        layout->addWidget(scroll);
        viewer.exec();
    });
    actionLayout->addWidget(originalBtn);

    QPushButton *exportBtn = new QPushButton(tr("导出复核图片..."), &dlg);
    exportBtn->setStyleSheet("QPushButton { background-color: #00695c; color: #80cbc4; border: 1px solid #00897b; } "
                            "QPushButton:hover { background-color: #00897b; color: #ffffff; }");
    connect(exportBtn, &QPushButton::clicked, &dlg, [this, &rec, step2Mat, step4Mat, showingOriginal]() {
        if (step2Mat.empty() && step4Mat.empty()) {
            QMessageBox::warning(this, tr("提示"), tr("当前记录中无有效图片可供导出！"));
            return;
        }
        QString dir = QFileDialog::getExistingDirectory(this, tr("选择保存目录"), "");
        if (!dir.isEmpty()) {
            bool ok2 = false, ok4 = false;
            QString s2 = QString("%1/%2_step2_rectified.png").arg(dir).arg(rec.cameraId);
            if (showingOriginal) s2 = QString("%1/%2_original.png").arg(dir).arg(rec.cameraId);
            QString s4 = QString("%1/%2_step4_recognition.png").arg(dir).arg(rec.cameraId);
            if (!step2Mat.empty()) ok2 = cv::imwrite(s2.toStdString(), step2Mat);
            if (!step4Mat.empty()) ok4 = cv::imwrite(s4.toStdString(), step4Mat);
            QMessageBox::information(this, tr("导出成功"), tr("已保存过程照片至目录:\n%1\n• 步骤2 (视场校正图): %2\n• 步骤4 (最终识别图): %3")
                                     .arg(dir).arg(ok2 ? "成功" : "无").arg(ok4 ? "成功" : "无"));
        }
    });
    actionLayout->addWidget(exportBtn);

    QPushButton *switchDebugBtn = new QPushButton(tr("使用本次原图重新分析"), &dlg);
    switchDebugBtn->setEnabled(!rec.originalPath.isEmpty());
    switchDebugBtn->setStyleSheet("QPushButton { background-color: #1a365d; color: #90cdf4; border: 1px solid #2b6cb0; } "
                                  "QPushButton:hover { background-color: #2b6cb0; color: #ffffff; }");
    connect(switchDebugBtn, &QPushButton::clicked, &dlg, [this, rec, &dlg]() {
        if (m_singleService->isRunning()) {
            QMessageBox::information(&dlg, tr("正在处理"), tr("请等待当前单设备任务完成。"));
            return;
        }
        InspectionTask task;
        task.device = rec.device;
        task.inputKind = InspectionInputKind::HistoricalImage;
        task.localImagePath = rec.originalPath;
        task.parentTaskId = rec.taskId;
        task.originalCapturedAt = rec.capturedAt;
        m_runModeCombo->setCurrentIndex(2);
        submitSingleTask(task);
        dlg.accept();
        m_visionTabWidget->setCurrentIndex(1);
    });
    actionLayout->addWidget(switchDebugBtn);

    actionLayout->addStretch();

    QPushButton *closeBtn = new QPushButton(tr("关闭 (Close)"), &dlg);
    closeBtn->setStyleSheet("QPushButton { background-color: #0091ea; color: #ffffff; padding: 6px 26px; font-weight: bold; border-radius: 4px; } "
                            "QPushButton:hover { background-color: #00b0ff; }");
    connect(closeBtn, &QPushButton::clicked, &dlg, &QDialog::accept);
    actionLayout->addWidget(closeBtn);

    mainLayout->addLayout(actionLayout);

    dlg.exec();
}


