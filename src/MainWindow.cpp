#include "MainWindow.hpp"
#include "CardinalSelectorDialog.hpp"
#include "CameraInfoEditDialog.hpp"
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

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    // Instantiate Core Backend Services
    m_gbManager = std::make_shared<GB28181StreamManager>();
    m_onvifManager = std::make_shared<ONVIFDeviceManager>();
    m_discoveryService = new AutoCameraDiscoveryService(this);

    // Auto register GB28181 & ONVIF in background
    m_gbManager->open();
    m_onvifManager->open();

    // Setup Log Callbacks to stream backend protocol events into system console
    m_gbManager->setLogCallback([this](const std::string& msg) {
        onLogReceived(QString::fromStdString(msg));
    });
    m_onvifManager->setLogCallback([this](const std::string& msg) {
        onLogReceived(QString::fromStdString(msg));
    });
    connect(m_discoveryService, &AutoCameraDiscoveryService::logGenerated, this, &MainWindow::onLogReceived);
    connect(m_discoveryService, &AutoCameraDiscoveryService::cameraDiscovered, this, &MainWindow::onDiscoveredCameraAdded);

    setupUi();
    applyDarkStyle();

    // Connect watcher signal for background algorithm processing
    connect(&m_watcher, &QFutureWatcher<GaugeResult>::finished, this, &MainWindow::onRecognitionFinished);

    m_autoPatrolTimer = new QTimer(this);
    connect(m_autoPatrolTimer, &QTimer::timeout, this, &MainWindow::onStartAllDevicesInspection);

    // Default paths setup: prioritize local deployed config in the application directory
    QString appLocalConfig = QCoreApplication::applicationDirPath() + "/config/gauge_profile.json";
    if (QFileInfo::exists(appLocalConfig)) {
        m_currentConfigPath = appLocalConfig;
    } else if (QFileInfo::exists("config/gauge_profile.json")) {
        m_currentConfigPath = "config/gauge_profile.json";
    } else if (QFileInfo::exists("../config/gauge_profile.json")) {
        m_currentConfigPath = "../config/gauge_profile.json";
    } else {
        m_currentConfigPath = appLocalConfig;
    }

    QString appLocalImage = QCoreApplication::applicationDirPath() + "/test_images/5-1-2.png";
    if (QFileInfo::exists(appLocalImage)) {
        m_currentImagePath = appLocalImage;
    } else if (QFileInfo::exists("test_images/5-1-2.png")) {
        m_currentImagePath = "test_images/5-1-2.png";
    } else if (QFileInfo::exists("../test_images/5-1-2.png")) {
        m_currentImagePath = "../test_images/5-1-2.png";
    } else {
        m_currentImagePath = "test_images/5-1-2.png";
    }

    m_imagePathEdit->setText(m_currentImagePath);
    m_configPathEdit->setText(m_currentConfigPath);

    // Load initial profile from config
    m_profile.loadFromFile(m_currentConfigPath.toStdString());

    // Populate Camera List from Persistent Device Registry & Auto-Connect Previous Device
    loadAndRestoreSavedDevices();

    // Start background periodic network discovery without clearing user list
    m_discoveryService->startAutoScan(30000);

    onLogReceived(tr("[SYSTEM] 工业指针仪表智能感知识别与检测平台已就绪。"));
    onLogReceived(tr("[SYSTEM] 后台国标 GB28181 传输引擎与 ONVIF 探针服务已静默启动并常驻后台。"));

    // Load initial frame if file exists
    if (QFileInfo::exists(m_currentImagePath) || QFileInfo::exists("../" + m_currentImagePath)) {
        m_currentFrame = cv::imread(m_currentImagePath.toStdString());
        if (m_currentFrame.empty()) {
            m_currentFrame = cv::imread(("../" + m_currentImagePath).toStdString());
        }
        if (!m_currentFrame.empty()) {
            m_gbManager->setSampleFrame(m_currentFrame);
            m_onvifManager->setSampleFrame(m_currentFrame);
            updateImageCard(m_imgStage1, m_currentFrame, "1. Original Stream Frame");
            m_statusLabel->setText(tr("<font color='#00e5ff'>图像已就绪，请点击【▶ 运行】按钮进行识别</font>"));
        }
    }

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
    QGroupBox *devBox = new QGroupBox(tr("摄像头设备管理库 (开机自动连接 🟢 已就绪)"), sidebar);
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

    fileLayout->addWidget(new QLabel(tr("参数配置文件:")));
    QHBoxLayout *cfgFileLayout = new QHBoxLayout();
    m_configPathEdit = new QLineEdit(fileBox);
    m_browseCfgBtn = new QPushButton(tr("浏览..."), fileBox);
    cfgFileLayout->addWidget(m_configPathEdit);
    cfgFileLayout->addWidget(m_browseCfgBtn);
    fileLayout->addLayout(cfgFileLayout);

    m_snapAndRunBtn = new QPushButton(tr("📷 拍摄并识别当前表计"), fileBox);
    m_snapAndRunBtn->setStyleSheet("background-color: #00897b; color: white; font-weight: bold; font-size: 13px; padding: 9px; border-radius: 6px;");
    fileLayout->addWidget(m_snapAndRunBtn);

    m_batchPatrolBtn = new QPushButton(tr("🚀 启动全站批量巡检"), fileBox);
    m_batchPatrolBtn->setStyleSheet("background-color: #0288d1; color: white; font-weight: bold; font-size: 13px; padding: 8px; border-radius: 6px;");
    fileLayout->addWidget(m_batchPatrolBtn);

    m_calibrateBtn = new QPushButton(tr("🎯 视场切点校准"), fileBox);
    m_calibrateBtn->setStyleSheet("background-color: #37474f; color: #00e5ff; border: 1px solid #00acc1; font-weight: bold; padding: 6px; border-radius: 4px;");
    fileLayout->addWidget(m_calibrateBtn);

    m_enableSteps4To9Check = new QCheckBox(tr("⚡ 执行步骤 4-9 (抗反光多通道预处理)"), fileBox);
    m_enableSteps4To9Check->setChecked(false); // 默认不进行，以节省运行时间
    m_enableSteps4To9Check->setToolTip(tr("默认取消勾选以节省运行时间。勾选后将生成步骤4-9（CLAHE对比度增强、高斯平滑降噪、Otsu二值化、黑帽滤波与阴影鉴别等）。"));
    m_enableSteps4To9Check->setStyleSheet("QCheckBox { color: #80deea; font-size: 11px; padding: 3px 0px; } QCheckBox::indicator:checked { background-color: #00bcd4; }");
    fileLayout->addWidget(m_enableSteps4To9Check);

    m_runBtn = new QPushButton(tr("▶ 重新分析当前图像"), fileBox);
    m_runBtn->setStyleSheet("background-color: #2e7d32; color: white; font-weight: bold; font-size: 12px; padding: 6px; border-radius: 4px;");
    fileLayout->addWidget(m_runBtn);

    m_exportStepsBtn = new QPushButton(tr("💾 导出 12 步中间图像"), fileBox);
    m_exportStepsBtn->setStyleSheet("background-color: #455a64; color: white; font-weight: bold; padding: 6px; border-radius: 4px;");
    fileLayout->addWidget(m_exportStepsBtn);

    sideLayout->addWidget(fileBox);

    // 3. Profile Info Card
    QGroupBox *profileBox = new QGroupBox(tr("摄像头与仪表规格档案 (GIS/位置/表头)"), sidebar);
    QVBoxLayout *profLayout = new QVBoxLayout(profileBox);
    m_cfgGaugeId = new QLabel(tr("设备/节点编号: -"), profileBox);
    m_cfgLocationInfo = new QLabel(tr("安装位置: -"), profileBox);
    m_cfgGisInfo = new QLabel(tr("GIS 坐标: -"), profileBox);
    m_cfgGaugeSpec = new QLabel(tr("表头规格: -"), profileBox);
    m_cfgRange = new QLabel(tr("量程范围: -"), profileBox);
    m_cfgAngleRange = new QLabel(tr("刻度角度: -"), profileBox);

    profLayout->addWidget(m_cfgGaugeId);
    profLayout->addWidget(m_cfgLocationInfo);
    profLayout->addWidget(m_cfgGisInfo);
    profLayout->addWidget(m_cfgGaugeSpec);
    profLayout->addWidget(m_cfgRange);
    profLayout->addWidget(m_cfgAngleRange);
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
    resLayout->addWidget(m_timeLabel);
    resLayout->addWidget(m_statusLabel);
    sideLayout->addWidget(resultBox);

    // Connections
    connect(m_autoDiscoverBtn, &QPushButton::clicked, this, &MainWindow::onTriggerAutoDiscovery);
    connect(m_addDeviceBtn, &QPushButton::clicked, this, &MainWindow::onAddNewDevice);
    connect(m_removeDeviceBtn, &QPushButton::clicked, this, &MainWindow::onRemoveSelectedDevice);
    connect(m_deviceFilterEdit, &QLineEdit::textChanged, this, &MainWindow::onFilterDeviceList);
    connect(m_deviceListWidget, &QListWidget::itemSelectionChanged, this, &MainWindow::onCameraSelectionChanged);
    connect(m_deviceListWidget, &QListWidget::itemDoubleClicked, this, &MainWindow::onEditCameraInfo);
    connect(m_deviceListWidget, &QListWidget::customContextMenuRequested, this, &MainWindow::onDeviceCustomContextMenu);
    connect(m_browseImgBtn, &QPushButton::clicked, this, &MainWindow::onSelectImage);
    connect(m_browseCfgBtn, &QPushButton::clicked, this, &MainWindow::onLoadConfig);
    connect(m_snapAndRunBtn, &QPushButton::clicked, this, &MainWindow::onCaptureAndRecognizeCurrent);
    connect(m_batchPatrolBtn, &QPushButton::clicked, this, &MainWindow::onStartAllDevicesInspection);
    connect(m_calibrateBtn, &QPushButton::clicked, this, &MainWindow::onCalibrateCardinalPoints);
    connect(m_runBtn, &QPushButton::clicked, this, &MainWindow::onRunRecognition);
    connect(m_exportStepsBtn, &QPushButton::clicked, this, &MainWindow::onExportAllSteps);

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
    gridLayout->addWidget(createStageCard(tr("步骤 3: 图像预处理图 (Preprocessed Image)"), m_imgStage3), 1, 0);

    QGroupBox *stage4Card = createStageCard(tr("步骤 4: 最终仪表读数识别 (Final Recognition Result)"), m_imgStage4);
    QPushButton *stage4DiagBtn = new QPushButton(tr("📊 独立窗口查看步骤12算法诊断信息 (HUD)"), stage4Card);
    stage4DiagBtn->setStyleSheet("QPushButton { background-color: #1a2a3a; color: #00e5ff; border: 1px solid #00a0e9; padding: 5px; border-radius: 4px; font-size: 12px; font-weight: bold; margin-top: 4px; }"
                                 "QPushButton:hover { background-color: #243b55; border-color: #00e5ff; }");
    connect(stage4DiagBtn, &QPushButton::clicked, this, &MainWindow::onShowDiagnosticWindow);
    stage4Card->layout()->addWidget(stage4DiagBtn);
    gridLayout->addWidget(stage4Card, 1, 1);

    tab1Scroll->setWidget(gridContainer);
    m_visionTabWidget->addTab(tab1Scroll, tr("⚡ 核心 4 步主流程 (Core 4 Stages)"));

    // --- Tab 2: 全流程 12 步中间图像调试与优化 ---
    QScrollArea *tab2Scroll = new QScrollArea(m_visionTabWidget);
    tab2Scroll->setWidgetResizable(true);
    tab2Scroll->setStyleSheet("QScrollArea { border: none; background-color: #121216; }");

    QWidget *allStepsWidget = new QWidget(tab2Scroll);
    QVBoxLayout *allStepsMainLayout = new QVBoxLayout(allStepsWidget);
    allStepsMainLayout->setContentsMargins(8, 8, 8, 8);
    allStepsMainLayout->setSpacing(8);

    QHBoxLayout *topCtrlLayout = new QHBoxLayout();
    QLabel *tipLabel = new QLabel(tr("🔬 图像处理全流程 12 个中间步骤拆解（点击卡片图像或【🔍 放大查看】观察高清特征与参数的效果）"), allStepsWidget);
    tipLabel->setStyleSheet("color: #00e5ff; font-weight: bold; font-size: 13px;");
    topCtrlLayout->addWidget(tipLabel);
    topCtrlLayout->addStretch();

    QPushButton *tabExportBtn = new QPushButton(tr("💾 一键导出所有中间步骤图像"), allStepsWidget);
    tabExportBtn->setStyleSheet("QPushButton { background-color: #00897b; color: white; font-weight: bold; padding: 6px 12px; border-radius: 4px; }"
                                 "QPushButton:hover { background-color: #00bfa5; }"
                                 "QPushButton:pressed { background-color: #00695c; }");
    topCtrlLayout->addWidget(tabExportBtn);
    connect(tabExportBtn, &QPushButton::clicked, this, &MainWindow::onExportAllSteps);

    allStepsMainLayout->addLayout(topCtrlLayout);

    m_allStepsGridContainer = new QWidget(allStepsWidget);
    m_allStepsGridLayout = new QGridLayout(m_allStepsGridContainer);
    m_allStepsGridLayout->setSpacing(10);
    allStepsMainLayout->addWidget(m_allStepsGridContainer);

    tab2Scroll->setWidget(allStepsWidget);
    m_visionTabWidget->addTab(tab2Scroll, tr("🔬 全流程 12 步中间图像调试与优化 (Step-by-Step Optimization)"));

    // --- Tab 3: 全站设备巡检读数总览 (Batch Patrol Inspection Matrix) ---
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

    m_liveCamInfoBadge = new QLabel(tr("📷 当前通道: CAM_5-3-1 | 协议: ONVIF (RTSP) | 地址: 192.168.1.101 | 状态: 🟢 实时传输中"), toolbarBox);
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

    m_useSnapshotForRunBtn = new QPushButton(tr("🚀 使用此抓图运行仪表识别"), toolbarBox);
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
    m_liveStreamLabel->setText(tr("<font color='#555868'>正在初始化摄像头视频流...请在左侧列表中点击选择设备</font>"));

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

    m_deviceListWidget->clear();
    m_discoveredItemsMap.clear();

    DeviceConfigManager::instance().load();
    QList<DiscoveredCameraItem> devices = DeviceConfigManager::instance().getAllDevices();

    for (const auto& dev : devices) {
        m_discoveredItemsMap.insert(dev.cameraId, dev);
        QListWidgetItem *item = new QListWidgetItem(m_deviceListWidget);
        item->setData(Qt::UserRole, dev.cameraId);
        updateDeviceListItemWidget(item, dev);
    }

    if (m_statusCameraCountBadge) {
        m_statusCameraCountBadge->setText(tr("📷 在库设备: %1 台").arg(m_discoveredItemsMap.size()));
    }

    // Determine target device to auto-connect on startup
    QString targetId = DeviceConfigManager::instance().getLastActiveDeviceId();
    QListWidgetItem *targetItem = findListItemByCameraId(targetId);

    if (!targetItem && m_deviceListWidget->count() > 0) {
        // Fallback to first auto-connect device or row 0
        for (int i = 0; i < m_deviceListWidget->count(); ++i) {
            QListWidgetItem *it = m_deviceListWidget->item(i);
            QString cid = it->data(Qt::UserRole).toString();
            if (m_discoveredItemsMap.value(cid).isAutoConnect) {
                targetItem = it;
                targetId = cid;
                break;
            }
        }
        if (!targetItem) {
            targetItem = m_deviceListWidget->item(0);
            targetId = targetItem->data(Qt::UserRole).toString();
        }
    }

    if (targetItem) {
        m_deviceListWidget->setCurrentItem(targetItem);
        switchActiveCameraSource(targetId);
        QString devName = m_discoveredItemsMap.value(targetId).customName;
        if (devName.isEmpty()) devName = m_discoveredItemsMap.value(targetId).modelName;
        onLogReceived(tr("[SYSTEM] 🚀 开机已直接自动连接至上次设备: %1 (%2) [免手动刷新]").arg(targetId).arg(devName));
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
    QString starIcon = data.isAutoConnect ? " ⭐[自连]" : "";
    QString nameText = data.customName.isEmpty() ? data.modelName : data.customName;

    QString line1 = QString("%1 %2 (%3)%4")
                        .arg(statusIcon)
                        .arg(data.cameraId)
                        .arg(nameText)
                        .arg(starIcon);

    QString line2 = QString("   地址/SN: %1 | %2")
                        .arg(data.ipOrSn)
                        .arg(data.protocolName);

    item->setText(line1 + "\n" + line2);
    item->setToolTip(tr("设备编号: %1\n名称备注: %2\n地址/SN: %3\n通信协议: %4\n端口: %5 (RTSP: %6)\n默认表计: %7\n开机自连: %8\n在线状态: %9\n\n💡 提示: 双击或右键可编辑网络连接参数与表计档案")
                         .arg(data.cameraId)
                         .arg(nameText)
                         .arg(data.ipOrSn)
                         .arg(data.protocolName)
                         .arg(data.port)
                         .arg(data.rtspPort)
                         .arg(data.defaultProfileId)
                         .arg(data.isAutoConnect ? tr("是 (默认启动自连)") : tr("否"))
                         .arg(data.isOnline ? tr("在线") : tr("离线")));
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

void MainWindow::updateCameraListFromConfig(const QString& configPath) {
    Q_UNUSED(configPath);
    loadAndRestoreSavedDevices();
}

QStringList MainWindow::loadCameraIdsFromConfig(const QString& configPath) {
    QStringList ids;
    QFile file(configPath);

    if (!file.open(QIODevice::ReadOnly)) {
        QStringList candidates = {
            configPath,
            "../" + configPath,
            "config/" + QFileInfo(configPath).fileName()
        };
        for (const auto& cand : candidates) {
            file.setFileName(cand);
            if (file.open(QIODevice::ReadOnly)) break;
        }
    }

    if (file.isOpen()) {
        QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
        file.close();
        if (doc.isObject()) {
            QJsonObject root = doc.object();
            QJsonArray camArr = root.contains("cameras") ? root["cameras"].toArray() : root["profiles"].toArray();

            for (const auto& val : camArr) {
                if (val.isObject()) {
                    QJsonObject obj = val.toObject();
                    if (obj.contains("camera_id")) ids.append(obj["camera_id"].toString());
                    else if (obj.contains("gauge_id")) ids.append(obj["gauge_id"].toString());
                }
            }
        }
    }
    return ids;
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
    if (m_discoveredItemsMap.contains(item.cameraId)) {
        // Upgrade / update existing entry without destroying user custom name and credentials
        auto existing = m_discoveredItemsMap.value(item.cameraId);
        existing.isOnline = true;
        if (!item.ipOrSn.isEmpty()) existing.ipOrSn = item.ipOrSn;
        if (existing.protocolType == CameraProtocolType::LOCAL_FILE && item.protocolType != CameraProtocolType::LOCAL_FILE) {
            existing.protocolType = item.protocolType;
            existing.protocolName = item.protocolName;
            existing.modelName = item.modelName;
            existing.port = item.port;
            existing.rtspPort = item.rtspPort;
            if (!item.rtspUrl.isEmpty()) existing.rtspUrl = item.rtspUrl;
        }
        m_discoveredItemsMap[item.cameraId] = existing;
        DeviceConfigManager::instance().addOrUpdateDevice(existing, true);

        QListWidgetItem *it = findListItemByCameraId(item.cameraId);
        if (it) {
            updateDeviceListItemWidget(it, existing);
        }
        return;
    }

    // New device discovered from local network
    DiscoveredCameraItem newItem = item;
    newItem.isOnline = true;
    m_discoveredItemsMap.insert(newItem.cameraId, newItem);
    DeviceConfigManager::instance().addOrUpdateDevice(newItem, true);

    QListWidgetItem *listItem = new QListWidgetItem(m_deviceListWidget);
    listItem->setData(Qt::UserRole, newItem.cameraId);
    updateDeviceListItemWidget(listItem, newItem);

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
    CameraInfoEditDialog dialog(m_currentConfigPath, "", this, true);
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
        m_deviceListWidget->setCurrentItem(it);
        switchActiveCameraSource(newItem.cameraId);

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
            m_deviceListWidget->setCurrentRow(0);
        } else {
            if (m_activeCameraSource) {
                m_activeCameraSource->close();
                m_activeCameraSource.reset();
            }
        }

        if (m_statusCameraCountBadge) {
            m_statusCameraCountBadge->setText(tr("📷 在库设备: %1 台").arg(m_discoveredItemsMap.size()));
        }
    }
}

void MainWindow::onSetDeviceAsDefaultAutoConnect() {
    QListWidgetItem *curr = m_deviceListWidget->currentItem();
    if (!curr) return;

    QString camId = curr->data(Qt::UserRole).toString();
    DeviceConfigManager::instance().setDeviceAutoConnect(camId, true, true);

    // Refresh UI item states
    for (int i = 0; i < m_deviceListWidget->count(); ++i) {
        QListWidgetItem *it = m_deviceListWidget->item(i);
        QString cid = it->data(Qt::UserRole).toString();
        DiscoveredCameraItem item = DeviceConfigManager::instance().getDevice(cid);
        m_discoveredItemsMap[cid] = item;
        updateDeviceListItemWidget(it, item);
    }

    onLogReceived(tr("[DEVICE] ⭐ 已将摄像头 [%1] 设为开机默认自动连接设备。").arg(camId));
    QMessageBox::information(this, tr("设置成功"), tr("已将摄像头 [%1] 设为开机默认自动连接设备！\n下次打开软件将直接连接此摄像头。").arg(camId));
}

void MainWindow::onDeviceCustomContextMenu(const QPoint& pos) {
    QListWidgetItem *item = m_deviceListWidget->itemAt(pos);
    QMenu menu(this);

    if (item) {
        QString camId = item->data(Qt::UserRole).toString();
        menu.addAction(tr("⚡ 立即连接此设备"), this, [this, camId]() {
            switchActiveCameraSource(camId);
        });
        menu.addAction(tr("⭐ 设为开机默认自动连接设备"), this, &MainWindow::onSetDeviceAsDefaultAutoConnect);
        menu.addAction(tr("✏️ 编辑设备网络参数与表计档案..."), this, [this, item]() {
            onEditCameraInfo(item);
        });
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

void MainWindow::closeEvent(QCloseEvent *event) {
    onLogReceived(tr("[SYSTEM] 正在安全关闭系统... 释放雄迈/ONVIF/GB28181设备网络连接句柄。"));

    if (m_liveTimer && m_liveTimer->isActive()) {
        m_liveTimer->stop();
    }
    if (m_discoveryService) {
        m_discoveryService->stopAutoScan();
    }
    if (m_activeCameraSource) {
        m_activeCameraSource->close();
        m_activeCameraSource.reset();
    }
    if (m_gbManager) {
        m_gbManager->close();
    }
    if (m_onvifManager) {
        m_onvifManager->close();
    }

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

    DiscoveredCameraItem item = m_discoveredItemsMap.value(camId);
    DeviceConfigManager::instance().setLastActiveDeviceId(camId, true);

    onLogReceived(tr("[CAMERA] 切换当前活动视频源节点至: %1 (%2 | %3)...").arg(camId).arg(item.protocolName).arg(item.ipOrSn));

    if (m_activeCameraSource) {
        m_activeCameraSource->close();
    }

    if (item.protocolType == CameraProtocolType::GB28181_PS_STREAM) {
        m_activeCameraSource = m_gbManager;
    } else if (item.protocolType == CameraProtocolType::ONVIF_RTSP_STREAM) {
        m_onvifManager->setSampleFrame(cv::Mat()); // Clear sample frame so network device doesn't display 5-1-2.png
        m_onvifManager->setDeviceIp(item.ipOrSn.toStdString());
        ONVIFDeviceInfo devInfo = m_onvifManager->getDeviceInfo();
        devInfo.ipAddress = item.ipOrSn.toStdString();
        if (item.port > 0) devInfo.port = item.port;
        devInfo.username = "";
        devInfo.password = "";
        if (!item.rtspUrl.isEmpty()) {
            devInfo.rtspUri = item.rtspUrl.toStdString();
        } else {
            devInfo.rtspUri = "rtsp://admin:@" + item.ipOrSn.toStdString() + ":" + std::to_string(item.rtspPort > 0 ? item.rtspPort : 554) + "/h264/ch1/main/av_stream";
        }
        m_onvifManager->setDeviceInfo(devInfo);
        m_activeCameraSource = m_onvifManager;
    } else if (item.protocolType == CameraProtocolType::SDK_JLINK) {
        auto jlinkSource = std::make_shared<SDKCameraSourceStub>(item.ipOrSn.toStdString(), 0, CameraProtocolType::SDK_JLINK);
        jlinkSource->setJLinkConfig("DEMO_UUID", "DEMO_KEY", "DEMO_SECRET", "DEMO_MOVECARD", "", "");
        jlinkSource->setLogCallback([this](const std::string& msg) {
            onLogReceived(QString::fromStdString(msg));
        });
        m_activeCameraSource = jlinkSource;
    } else {
        m_activeCameraSource = std::make_shared<FileCameraSource>(m_currentImagePath.toStdString());
    }

    if (m_activeCameraSource) {
        m_activeCameraSource->open();
    }

    if (m_liveCamInfoBadge) {
        m_liveCamInfoBadge->setText(QString(tr("📷 当前通道: %1 | 协议: %2 | 地址/SN: %3 | 状态: 🟢 实时传输中"))
                                      .arg(item.cameraId)
                                      .arg(item.protocolName)
                                      .arg(item.ipOrSn));
    }

    applyCameraCalibrationAndProfile(camId);

    // Instantly update stream view without blocking main thread
    onUpdateLiveStreamFrame();
}

void MainWindow::applyCameraCalibrationAndProfile(const QString& camId) {
    if (!m_discoveredItemsMap.contains(camId)) return;
    const DiscoveredCameraItem& item = m_discoveredItemsMap[camId];

    // 1. Load meter profile algorithm specs from gauge_profile.json
    QString profileId = item.defaultProfileId.isEmpty() ? "PRESSURE_GAUGE_1.6MPA" : item.defaultProfileId;
    m_profile.loadFromFile(m_currentConfigPath.toStdString(), profileId.toStdString());

    // 2. Set node identity and asset metadata from camera item
    m_profile.gauge_id = camId.toStdString();
    m_profile.location.substation = item.substation.toStdString();
    m_profile.location.area = item.area.toStdString();
    m_profile.location.device_name = item.customName.toStdString();
    m_profile.location.device_tag = item.deviceTag.toStdString();
    m_profile.spec.gauge_type = item.gaugeType.toStdString();
    m_profile.spec.manufacturer = item.manufacturer.toStdString();
    m_profile.spec.model_name = item.modelName.toStdString();
    m_profile.spec.accuracy_class = item.accuracyClass.toStdString();

    // 3. Overlay camera's physical perspective calibration from devices.json
    m_profile.perspective_correction_enabled = item.perspectiveEnabled;
    m_profile.perspective_pts = {
        cv::Point2f(static_cast<float>(item.pt0_x), static_cast<float>(item.pt0_y)),
        cv::Point2f(static_cast<float>(item.pt1_x), static_cast<float>(item.pt1_y)),
        cv::Point2f(static_cast<float>(item.pt2_x), static_cast<float>(item.pt2_y)),
        cv::Point2f(static_cast<float>(item.pt3_x), static_cast<float>(item.pt3_y))
    };
    m_profile.center = cv::Point2f(static_cast<float>(item.centerX), static_cast<float>(item.centerY));
    m_profile.pointerBaseRadius = item.pointerBaseRadius;
    m_profile.calibrated_width = item.calibratedWidth;
    m_profile.calibrated_height = item.calibratedHeight;
    m_profile.auto_detect_roi = false;
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
    cv::Mat grabbed;
    if (m_activeCameraSource && m_activeCameraSource->isOpened()) {
        m_activeCameraSource->grabFrame(grabbed);
    }
    if (grabbed.empty()) {
        grabbed = m_currentFrame;
    }

    if (grabbed.empty()) {
        QMessageBox::warning(this, tr("抓图失败"), tr("当前摄像头未抓取到有效图像帧！"));
        return;
    }

    m_lastCapturedFrame = grabbed.clone();
    updateImageCard(m_capturedSnapshotLabel, m_lastCapturedFrame, "Captured Snapshot");

    QString activeCamId = "CAM_5-3-1";
    if (m_deviceListWidget && m_deviceListWidget->currentItem()) {
        activeCamId = m_deviceListWidget->currentItem()->data(Qt::UserRole).toString();
    }

    QString timeStr = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz");
    QString infoText = QString(tr("快照状态: 🟢 抓取成功\n捕获时间: %1\n分辨率: %2 x %3 px\n采样设备: %4"))
                           .arg(timeStr)
                           .arg(m_lastCapturedFrame.cols)
                           .arg(m_lastCapturedFrame.rows)
                           .arg(activeCamId);
    m_snapshotInfoLabel->setText(infoText);
    m_useSnapshotForRunBtn->setEnabled(true);
    m_saveSnapshotBtn->setEnabled(true);

    onLogReceived(tr("[CAPTURE] 成功抓取摄像头 [%1] 的画面快照 (%2x%3)。")
                      .arg(activeCamId).arg(m_lastCapturedFrame.cols).arg(m_lastCapturedFrame.rows));
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

    std::string stdPath = filePath.toLocal8Bit().constData();
    bool ok = cv::imwrite(stdPath, m_lastCapturedFrame);
    if (!ok) {
        std::vector<uchar> buf;
        std::string ext = QFileInfo(filePath).suffix().toLower().toStdString();
        if (ext.empty()) ext = "png";
        if (cv::imencode("." + ext, m_lastCapturedFrame, buf)) {
            QFile file(filePath);
            if (file.open(QIODevice::WriteOnly)) {
                file.write(reinterpret_cast<const char*>(buf.data()), buf.size());
                file.close();
                ok = true;
            }
        }
    }

    if (ok) {
        onLogReceived(tr("[CAPTURE] 快照图像已成功保存至: %1").arg(filePath));
        QMessageBox::information(this, tr("保存成功"), tr("快照图像已成功保存至:\n%1").arg(filePath));
    } else {
        onLogReceived(tr("[ERROR] 保存快照图像失败: %1").arg(filePath));
        QMessageBox::critical(this, tr("保存失败"), tr("无法将快照图像写入指定路径:\n%1").arg(filePath));
    }
}

void MainWindow::onUseSnapshotForRecognition() {
    if (m_lastCapturedFrame.empty()) return;

    m_currentFrame = m_lastCapturedFrame.clone();
    m_gbManager->setSampleFrame(m_currentFrame);
    m_onvifManager->setSampleFrame(m_currentFrame);

    updateImageCard(m_imgStage1, m_currentFrame, "1. Original Stream Frame");
    if (m_visionTabWidget) {
        m_visionTabWidget->setCurrentIndex(1); // Switch to Core 4-step pipeline tab
    }
    onRunRecognition();

    onLogReceived(tr("[CAPTURE] 已将画面快照载入核心识别引擎进行仪表读数分析。"));
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
    if (!m_isLiveStreaming) return;

    // 1. Grab fresh frame from camera source (keeps RTSP TCP socket buffer drained & active)
    cv::Mat frame;
    if (m_activeCameraSource && m_activeCameraSource->isOpened()) {
        m_activeCameraSource->grabFrame(frame);
    }

    // 2. Only perform Qt QImage/QPixmap rendering when Stage 0 (Live Stream & Capture) Tab is visible
    if (m_visionTabWidget && m_visionTabWidget->currentIndex() == 0) {
        if (!frame.empty() && m_liveStreamLabel) {
            updateImageCard(m_liveStreamLabel, frame, "Live Stream Video Player");
        }
    }
}

void MainWindow::onLogReceived(const QString& logMsg) {
    if (!m_logConsole) return;
    QString timeStr = QDateTime::currentDateTime().toString("HH:mm:ss.zzz");
    m_logConsole->append(QString("[%1] %2").arg(timeStr).arg(logMsg));
}

void MainWindow::onSelectImage() {
    QString fileName = QFileDialog::getOpenFileName(this, tr("选择仪表图像"), "", tr("Image Files (*.png *.jpg *.bmp *.jpeg *.tif)"));
    if (!fileName.isEmpty()) {
        m_currentImagePath = fileName;
        m_imagePathEdit->setText(m_currentImagePath);
        m_currentFrame = cv::imread(m_currentImagePath.toStdString());
        if (m_currentFrame.empty()) {
            QMessageBox::warning(this, tr("读取错误"), tr("无法读取选中的图像文件！"));
            return;
        }
        m_gbManager->setSampleFrame(m_currentFrame);
        m_onvifManager->setSampleFrame(m_currentFrame);

        // Display loaded image on Stage 1 preview card without auto-running recognition
        updateImageCard(m_imgStage1, m_currentFrame, "1. Original Stream Frame");
        m_statusLabel->setText(tr("<font color='#00e5ff'>图像已成功载入，请点击【▶ 运行】按钮开始识别</font>"));
        onLogReceived(tr("[IMAGE] 已载入新照片: %1，请点击【运行】开始识别。").arg(m_currentImagePath));
    }
}

void MainWindow::onLoadConfig() {
    QString fileName = QFileDialog::getOpenFileName(this, tr("选择配置文件"), "", tr("JSON Files (*.json)"));
    if (!fileName.isEmpty()) {
        m_currentConfigPath = fileName;
        m_configPathEdit->setText(m_currentConfigPath);
        updateCameraListFromConfig(m_currentConfigPath);
        onRunRecognition();
    }
}

void MainWindow::onCalibrateCardinalPoints() {
    if (m_currentFrame.empty() && QFileInfo::exists(m_currentImagePath)) {
        m_currentFrame = cv::imread(m_currentImagePath.toStdString());
    }

    if (m_currentFrame.empty()) {
        QMessageBox::warning(this, tr("错误"), tr("未载入有效仪表原图，请先选择一幅仪表图像！"));
        return;
    }

    QString activeCam = "CAM_5-3-1";
    if (m_deviceListWidget && m_deviceListWidget->currentItem()) {
        activeCam = m_deviceListWidget->currentItem()->data(Qt::UserRole).toString();
    }

    // Refresh profile and apply camera perspective calibration
    applyCameraCalibrationAndProfile(activeCam);

    CardinalSelectorDialog dialog(m_currentFrame, m_profile, m_currentConfigPath, activeCam, this);
    if (dialog.exec() == QDialog::Accepted) {
        DeviceConfigManager::instance().load();
        if (m_discoveredItemsMap.contains(activeCam)) {
            m_discoveredItemsMap[activeCam] = DeviceConfigManager::instance().getDevice(activeCam);
        }
        applyCameraCalibrationAndProfile(activeCam);
        onRunRecognition();
    }
}

void MainWindow::onEditCameraInfo(QListWidgetItem *item) {
    if (!item) return;

    QString camId = item->data(Qt::UserRole).toString();
    if (camId.isEmpty()) return;

    CameraInfoEditDialog dialog(m_currentConfigPath, camId, this, false);
    if (dialog.exec() == QDialog::Accepted) {
        QString newCamId = dialog.getUpdatedCameraId();
        onLogReceived(tr("[CAMERA] 已成功更新摄像头节点 [%1] 网络连接与属性档案。").arg(newCamId));

        loadAndRestoreSavedDevices();

        QListWidgetItem *it = findListItemByCameraId(newCamId);
        if (it) {
            m_deviceListWidget->setCurrentItem(it);
            switchActiveCameraSource(newCamId);
        }
    }
}

void MainWindow::onRunRecognition() {
    if (m_watcher.isRunning()) return;

    QString activeCam = "CAM_5-3-1";
    if (m_deviceListWidget && m_deviceListWidget->currentItem()) {
        activeCam = m_deviceListWidget->currentItem()->data(Qt::UserRole).toString();
    }

    applyCameraCalibrationAndProfile(activeCam);

    m_cfgGaugeId->setText(QString(tr("设备/节点编号: %1 [%2]")).arg(QString::fromStdString(m_profile.gauge_id)).arg(QString::fromStdString(m_profile.location.device_tag)));
    m_cfgLocationInfo->setText(QString(tr("安装位置: %1 | %2")).arg(QString::fromStdString(m_profile.location.substation)).arg(QString::fromStdString(m_profile.location.area)));
    m_cfgGisInfo->setText(QString::asprintf("GIS 坐标: E %.4f°, N %.4f° (H %.1fm)", m_profile.gis.longitude, m_profile.gis.latitude, m_profile.gis.altitude));
    m_cfgGaugeSpec->setText(QString(tr("表头规格: %1 | %2 | %3")).arg(QString::fromStdString(m_profile.spec.gauge_type)).arg(QString::fromStdString(m_profile.spec.manufacturer)).arg(QString::fromStdString(m_profile.spec.accuracy_class)));
    m_cfgRange->setText(QString(tr("量程范围: %1 ~ %2 %3")).arg(m_profile.min_val).arg(m_profile.max_val).arg(QString::fromStdString(m_profile.unit)));
    m_cfgAngleRange->setText(QString(tr("刻度角度: %1° ~ %2°")).arg(m_profile.min_angle_deg).arg(m_profile.max_angle_deg));

    m_statusLabel->setText(tr("<font color='#00e5ff'>系统状态: 正在后台4步流程预处理与识别...</font>"));
    m_runBtn->setEnabled(false);

    m_startTime = std::chrono::high_resolution_clock::now();

    auto preScaleMat = [](const cv::Mat& src, int maxDim = 800) -> cv::Mat {
        if (src.empty()) return cv::Mat();
        if (src.cols <= maxDim && src.rows <= maxDim) return src.clone();
        double scale = static_cast<double>(maxDim) / std::max(src.cols, src.rows);
        cv::Mat dst;
        cv::resize(src, dst, cv::Size(), scale, scale, cv::INTER_AREA);
        return dst;
    };

    CameraSourcePtr activeSource = m_activeCameraSource;
    cv::Mat fallbackFrame = m_currentFrame.clone();
    std::string currentImagePath = m_currentImagePath.toStdString();
    GaugeProfile profileToUse = m_profile;
    if (m_enableSteps4To9Check) {
        profileToUse.enable_steps_4_to_9 = m_enableSteps4To9Check->isChecked();
    }

    QFuture<GaugeResult> future = QtConcurrent::run([this, activeSource, fallbackFrame, currentImagePath, profileToUse, preScaleMat]() -> GaugeResult {
        cv::Mat grabFrame;
        if (!fallbackFrame.empty()) {
            grabFrame = fallbackFrame.clone();
        } else if (activeSource && activeSource->isOpened()) {
            activeSource->grabFrame(grabFrame);
        }

        if (grabFrame.empty() && QFileInfo::exists(QString::fromStdString(currentImagePath))) {
            grabFrame = cv::imread(currentImagePath);
        }

        if (grabFrame.empty()) {
            GaugeResult errRes;
            errRes.success = false;
            errRes.errorMessage = "错误: 未加载有效的仪表输入图像帧";
            return errRes;
        }

        GaugeProfile prof = profileToUse;
        GaugeResult res = m_engine.process(grabFrame, prof);
        if (res.success) {
            res.step1_original = preScaleMat(res.step1_original);
            res.step2_rectified = preScaleMat(res.step2_rectified);
            res.step3_preprocessed = preScaleMat(res.step3_preprocessed);
            res.step4_recognition = preScaleMat(res.step4_recognition);
        }
        return res;
    });

    m_watcher.setFuture(future);
}

void MainWindow::onRecognitionFinished() {
    auto tEnd = std::chrono::high_resolution_clock::now();
    double elapsedMs = std::chrono::duration<double, std::milli>(tEnd - m_startTime).count();

    m_latestResult = m_watcher.result();
    m_runBtn->setEnabled(true);

    // Always update 4 pipeline stage cards and intermediate steps so user can inspect intermediate results
    updateImageCard(m_imgStage1, m_latestResult.step1_original, "1. Original Stream Frame");
    updateImageCard(m_imgStage2, m_latestResult.step2_rectified, "2. FOV Rectified Image");
    updateImageCard(m_imgStage3, m_latestResult.step3_preprocessed, "3. Preprocessed Image");
    updateImageCard(m_imgStage4, m_latestResult.step4_recognition, "4. Final Recognition Result");

    // If diagnostic window is open, refresh its content automatically
    if (m_diagnosticDialog && m_diagnosticDialog->isVisible()) {
        if (m_diagnosticImgLabel && !m_latestResult.diagnosticHudImage.empty()) {
            QImage qimg = cvMatToQImage(m_latestResult.diagnosticHudImage);
            m_diagnosticImgLabel->setPixmap(QPixmap::fromImage(qimg));
        }
    }

    // Render full 12 intermediate steps UI
    renderIntermediateStepsUI(m_latestResult.intermediateSteps);

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
            m_statusLabel->setText(tr("<font color='#00e676'>系统状态: 4步/12步全流程识别完成，数据已输出</font>"));
        }

        onLogReceived(tr("[PIPELINE] 仪表识别完成: 数值=%.3f %1, 耗时=%.2f ms, 已生成全套 12 个中间处理步骤图。")
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
        onLogReceived(tr("💡 提示: 请点击卡片图像或【放大调试视图】查看全套 12 步中间图像排查反光/阈值问题"));
        onLogReceived(tr("--------------------------------------------------"));
    }

    // Sync recognition outcome and stored images into inspection matrix records
    QString inspectedGaugeId = QString::fromStdString(m_profile.gauge_id);
    for (int i = 0; i < m_inspectionRecords.size(); ++i) {
        if (m_inspectionRecords[i].cameraId == inspectedGaugeId) {
            auto& rec = m_inspectionRecords[i];
            rec.readingValue = m_latestResult.value;
            rec.unit = m_latestResult.unit;
            rec.pointerAngle = m_latestResult.pointer_angle_deg;
            rec.confidence = m_latestResult.confidence * 100.0;
            rec.status = (m_latestResult.success && m_latestResult.confidence >= 0.40) ? tr("🟢 正常") : tr("⚠️ 待复核");
            rec.timestamp = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
            rec.step1_original = m_latestResult.step1_original.clone();
            rec.step2_rectified = m_latestResult.step2_rectified.clone();
            rec.step4_recognition = m_latestResult.step4_recognition.clone();
            rec.diagnosticText = QString::fromStdString(m_latestResult.detailedDebugInfo);

            QString snapshotDir = QCoreApplication::applicationDirPath() + "/inspection_snapshots";
            QDir().mkpath(snapshotDir);
            QString step2File = QString("%1/%2_step2_rectified.png").arg(snapshotDir).arg(rec.cameraId);
            QString step4File = QString("%1/%2_step4_recognition.png").arg(snapshotDir).arg(rec.cameraId);
            if (!rec.step2_rectified.empty()) {
                cv::imwrite(step2File.toStdString(), rec.step2_rectified);
                rec.step2Path = step2File;
            }
            if (!rec.step4_recognition.empty()) {
                cv::imwrite(step4File.toStdString(), rec.step4_recognition);
                rec.step4Path = step4File;
            }

            if (m_inspectionTable && i < m_inspectionTable->rowCount()) {
                if (m_inspectionTable->item(i, 4)) m_inspectionTable->item(i, 4)->setText(QString::asprintf("%.2f %s", rec.readingValue, rec.unit.c_str()));
                if (m_inspectionTable->item(i, 5)) m_inspectionTable->item(i, 5)->setText(QString::asprintf("%.1f°", rec.pointerAngle));
                if (m_inspectionTable->item(i, 6)) m_inspectionTable->item(i, 6)->setText(QString::asprintf("%.1f%%", rec.confidence));
                if (m_inspectionTable->item(i, 7)) {
                    m_inspectionTable->item(i, 7)->setText(rec.status);
                    m_inspectionTable->item(i, 7)->setForeground(rec.status.contains("正常") ? QBrush(QColor("#00e676")) : QBrush(QColor("#ffb300")));
                }
                if (m_inspectionTable->item(i, 8)) m_inspectionTable->item(i, 8)->setText(rec.timestamp);

                QWidget* w = m_inspectionTable->cellWidget(i, 9);
                if (w) {
                    QPushButton* btn = qobject_cast<QPushButton*>(w);
                    if (btn) {
                        btn->setText(tr("📸 查看照片"));
                        btn->setStyleSheet(
                            "QPushButton { background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #00897b, stop:1 #00bcd4); "
                            "color: #ffffff; border: 1px solid #4dd0e1; border-radius: 4px; padding: 3px 8px; font-weight: bold; font-size: 12px; } "
                            "QPushButton:hover { background: #00e5ff; color: #0a0b0e; }"
                        );
                    }
                }
            }
            break;
        }
    }
}

void MainWindow::renderIntermediateStepsUI(const std::vector<ProcessingStepImage>& steps) {
    if (!m_allStepsGridLayout) return;

    // Clear previous widgets
    QLayoutItem *item;
    while ((item = m_allStepsGridLayout->takeAt(0)) != nullptr) {
        if (item->widget()) delete item->widget();
        delete item;
    }

    m_imgStepLabels.clear();
    m_titleStepLabels.clear();
    m_descStepLabels.clear();

    int cols = 3;
    for (size_t i = 0; i < steps.size(); ++i) {
        const auto& step = steps[i];
        int row = static_cast<int>(i) / cols;
        int col = static_cast<int>(i) % cols;

        QGroupBox *cardBox = new QGroupBox(m_allStepsGridContainer);
        cardBox->setStyleSheet(
            "QGroupBox { background-color: #171822; border: 1px solid #2b2e3e; border-radius: 6px; margin-top: 6px; font-weight: bold; color: #00e5ff; }"
            "QGroupBox::title { subcontrol-origin: margin; subcontrol-position: top left; padding: 2px 8px; background-color: #171822; }"
        );

        QVBoxLayout *cardLayout = new QVBoxLayout(cardBox);
        cardLayout->setContentsMargins(8, 8, 8, 8);
        cardLayout->setSpacing(6);

        // Header Title Badge
        QHBoxLayout *titleLayout = new QHBoxLayout();
        QLabel *numBadge = new QLabel(QString("Step %1").arg(step.stepNumber, 2, 10, QChar('0')), cardBox);
        numBadge->setStyleSheet("background-color: #005c99; color: #ffffff; font-weight: bold; padding: 2px 6px; border-radius: 3px; font-size: 11px;");
        
        QLabel *titleLabel = new QLabel(QString::fromStdString(step.title), cardBox);
        titleLabel->setStyleSheet("color: #00e5ff; font-weight: bold; font-size: 12px;");
        titleLayout->addWidget(numBadge);
        titleLayout->addWidget(titleLabel, 1);
        cardLayout->addLayout(titleLayout);

        // Image Card Label
        QLabel *imgLabel = new QLabel(cardBox);
        imgLabel->setAlignment(Qt::AlignCenter);
        imgLabel->setMinimumSize(320, 240);
        imgLabel->setCursor(Qt::PointingHandCursor);
        imgLabel->setStyleSheet("background-color: #0c0d12; border: 1px solid #232635; border-radius: 4px;");

        if (!step.image.empty()) {
            QImage qimg = cvMatToQImage(step.image);
            QPixmap pix = QPixmap::fromImage(qimg);
            imgLabel->setPixmap(pix.scaled(320, 240, Qt::KeepAspectRatio, Qt::FastTransformation));
        }

        cardLayout->addWidget(imgLabel);

        // Description Label
        QLabel *descLabel = new QLabel(QString::fromStdString(step.description), cardBox);
        descLabel->setWordWrap(true);
        descLabel->setStyleSheet("color: #a0a5b5; font-size: 11px; padding: 2px;");
        cardLayout->addWidget(descLabel);

        // Zoom & Diagnostic Buttons
        QHBoxLayout *btnBox = new QHBoxLayout();
        QPushButton *zoomBtn = new QPushButton(tr("🔍 放大查看细节"), cardBox);
        zoomBtn->setStyleSheet("QPushButton { background-color: #232736; color: #00e5ff; border: 1px solid #363b50; padding: 5px; border-radius: 4px; font-size: 12px; font-weight: bold; }"
                               "QPushButton:hover { background-color: #2f3448; border-color: #00e5ff; }");
        btnBox->addWidget(zoomBtn);

        int stepNum = step.stepNumber;
        if (stepNum == 12) {
            QPushButton *diagBtn = new QPushButton(tr("📊 独立窗口查看诊断信息"), cardBox);
            diagBtn->setStyleSheet("QPushButton { background-color: #1a365d; color: #63b3ed; border: 1px solid #2b6cb0; padding: 5px; border-radius: 4px; font-size: 12px; font-weight: bold; }"
                                   "QPushButton:hover { background-color: #2b6cb0; color: #ffffff; }");
            connect(diagBtn, &QPushButton::clicked, this, &MainWindow::onShowDiagnosticWindow);
            btnBox->addWidget(diagBtn);
        }
        cardLayout->addLayout(btnBox);

        connect(zoomBtn, &QPushButton::clicked, this, [this, stepNum]() {
            onShowEnlargedStepImage(stepNum);
        });

        m_allStepsGridLayout->addWidget(cardBox, row, col);
    }
}

void MainWindow::onShowEnlargedStepImage(int stepNumber) {
    if (m_latestResult.intermediateSteps.empty()) return;

    const ProcessingStepImage* targetStep = nullptr;
    for (const auto& step : m_latestResult.intermediateSteps) {
        if (step.stepNumber == stepNumber) {
            targetStep = &step;
            break;
        }
    }

    if (!targetStep || targetStep->image.empty()) return;

    QDialog dlg(this);
    dlg.setWindowTitle(QString("步骤 %1 放大调试视图 - %2").arg(stepNumber).arg(QString::fromStdString(targetStep->title)));
    dlg.resize(980, 750);
    dlg.setStyleSheet("QDialog { background-color: #101117; color: #ffffff; }");

    QVBoxLayout *layout = new QVBoxLayout(&dlg);

    QLabel *headLabel = new QLabel(QString("[%1] %2").arg(stepNumber).arg(QString::fromStdString(targetStep->title)), &dlg);
    headLabel->setStyleSheet("font-size: 16px; font-weight: bold; color: #00e5ff; padding: 4px;");
    layout->addWidget(headLabel);

    QLabel *descLabel = new QLabel(QString::fromStdString(targetStep->description), &dlg);
    descLabel->setStyleSheet("font-size: 13px; color: #b0b5c5; padding-bottom: 6px;");
    layout->addWidget(descLabel);

    QScrollArea *scrollArea = new QScrollArea(&dlg);
    scrollArea->setWidgetResizable(true);
    scrollArea->setStyleSheet("QScrollArea { border: 1px solid #2d3142; background-color: #0a0b0e; }");

    QLabel *imgLabel = new QLabel(scrollArea);
    imgLabel->setAlignment(Qt::AlignCenter);

    QImage qimg = cvMatToQImage(targetStep->image);
    QPixmap pix = QPixmap::fromImage(qimg);
    imgLabel->setPixmap(pix);
    scrollArea->setWidget(imgLabel);

    layout->addWidget(scrollArea, 1);

    QHBoxLayout *btnLayout = new QHBoxLayout();
    if (stepNumber == 12) {
        QPushButton *diagBtn = new QPushButton(tr("📊 打开指针算法诊断信息独立窗口 (HUD)"), &dlg);
        diagBtn->setStyleSheet("QPushButton { background-color: #1a365d; color: #63b3ed; padding: 6px 16px; font-weight: bold; border-radius: 4px; border: 1px solid #2b6cb0; }"
                               "QPushButton:hover { background-color: #2b6cb0; color: #ffffff; }");
        connect(diagBtn, &QPushButton::clicked, this, &MainWindow::onShowDiagnosticWindow);
        btnLayout->addWidget(diagBtn);
    }
    btnLayout->addStretch();
    QPushButton *closeBtn = new QPushButton(tr("关闭 (Close)"), &dlg);
    closeBtn->setStyleSheet("QPushButton { background-color: #0091ea; color: white; padding: 6px 22px; font-weight: bold; border-radius: 4px; }");
    connect(closeBtn, &QPushButton::clicked, &dlg, &QDialog::accept);
    btnLayout->addWidget(closeBtn);

    layout->addLayout(btnLayout);

    dlg.exec();
}

void MainWindow::onExportAllSteps() {
    if (m_latestResult.intermediateSteps.empty()) {
        QMessageBox::warning(this, tr("提示"), tr("当前尚无有效的中间处理步骤图像，请先点击【▶ 运行】完成一次识别处理！"));
        return;
    }

    QString dirPath = QFileDialog::getExistingDirectory(this, tr("选择导出目标文件夹"), "debug_steps");
    if (dirPath.isEmpty()) return;

    int exportedCount = 0;
    for (const auto& stepItem : m_latestResult.intermediateSteps) {
        if (!stepItem.image.empty()) {
            char filename[256];
            snprintf(filename, sizeof(filename), "/step%02d_%s.png", stepItem.stepNumber,
                     (stepItem.stepNumber == 1 ? "original" :
                      stepItem.stepNumber == 2 ? "rectified" :
                      stepItem.stepNumber == 3 ? "gray" :
                      stepItem.stepNumber == 4 ? "clahe" :
                      stepItem.stepNumber == 5 ? "gaussian_blur" :
                      stepItem.stepNumber == 6 ? "otsu_threshold" :
                      stepItem.stepNumber == 7 ? "blackhat_filter" :
                      stepItem.stepNumber == 8 ? "shadow_mask" :
                      stepItem.stepNumber == 9 ? "fused_binary" :
                      stepItem.stepNumber == 10 ? "hub_location" :
                      stepItem.stepNumber == 11 ? "trajectory_binary" : "recognition_result"));
            QString fullPath = dirPath + QString(filename);
            if (cv::imwrite(fullPath.toStdString(), stepItem.image)) {
                exportedCount++;
            }
        }
    }

    onLogReceived(tr("[EXPORT] 成功导出全套 %1 张中间处理步骤图像到: %2").arg(exportedCount).arg(dirPath));
    QMessageBox::information(this, tr("导出成功"), tr("成功将全套 %1 张中间处理步骤图像保存至:\n%2").arg(exportedCount).arg(dirPath));
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
        m_diagnosticDialog->setWindowTitle(tr("步骤 12 指针识别算法诊断信息 (Diagnostic HUD)"));
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
    QString activeCam = "CAM_5-3-1";
    if (m_deviceListWidget && m_deviceListWidget->currentItem()) {
        activeCam = m_deviceListWidget->currentItem()->data(Qt::UserRole).toString();
    }

    onLogReceived(tr("[INSPECT] 📷 正在向设备 [%1] 请求单张高清抓拍照并执行表盘读数识别...").arg(activeCam));

    // 1. Prepare profile and camera calibration
    applyCameraCalibrationAndProfile(activeCam);

    // 2. Fetch standalone snapshot from active source without locking continuous video stream
    cv::Mat grabbed;
    if (m_activeCameraSource) {
        m_activeCameraSource->captureSingleSnapshot(grabbed);
    }

    if (grabbed.empty() && !m_currentFrame.empty()) {
        grabbed = m_currentFrame.clone();
    }

    if (grabbed.empty() && QFileInfo::exists(m_currentImagePath)) {
        grabbed = cv::imread(m_currentImagePath.toStdString());
    }

    if (grabbed.empty()) {
        QMessageBox::warning(this, tr("抓拍失败"), tr("未能从摄像头 [%1] 获取到有效图像帧，请检查相机网络连接！").arg(activeCam));
        onLogReceived(tr("[INSPECT] ❌ 抓拍失败: 摄像头 [%1] 未响应图像数据。").arg(activeCam));
        return;
    }

    m_currentFrame = grabbed.clone();
    m_lastCapturedFrame = grabbed.clone();

    // Update display cards
    updateImageCard(m_imgStage1, m_currentFrame, "1. Original Snapshot Frame");
    if (m_capturedSnapshotLabel) {
        updateImageCard(m_capturedSnapshotLabel, m_lastCapturedFrame, "Captured Snapshot");
    }

    // 3. Immediately trigger recognition
    onRunRecognition();
    onLogReceived(tr("[INSPECT] 🟢 摄像头 [%1] 拍摄成功，正在运行机器视觉识别流水线...").arg(activeCam));
}

void MainWindow::onStartAllDevicesInspection() {
    if (m_isPatrolRunning) return;

    DeviceConfigManager::instance().load();
    QList<DiscoveredCameraItem> devices = DeviceConfigManager::instance().getAllDevices();
    if (devices.isEmpty()) {
        QMessageBox::information(this, tr("提示"), tr("当前设备库为空，请先添加摄像头设备！"));
        return;
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

    QTimer::singleShot(50, this, &MainWindow::onPatrolNextDevice);
}

void MainWindow::onStopAllDevicesInspection() {
    if (!m_isPatrolRunning) return;
    m_isPatrolRunning = false;
    m_startPatrolBtn->setEnabled(true);
    m_stopPatrolBtn->setEnabled(false);
    onLogReceived(tr("[PATROL] ⏹ 用户已手动中止全站批量巡检。"));
    m_inspectionSummaryLabel->setText(tr("📊 巡检已中止 (已完成 %1/%2 台)")
        .arg(m_patrolCurrentIndex).arg(m_patrolCameraIdQueue.size()));
}

void MainWindow::onPatrolNextDevice() {
    if (!m_isPatrolRunning) return;

    if (m_patrolCurrentIndex >= m_patrolCameraIdQueue.size()) {
        m_isPatrolRunning = false;
        m_startPatrolBtn->setEnabled(true);
        m_stopPatrolBtn->setEnabled(false);
        m_inspectionProgressBar->setValue(100);

        int normalCount = 0;
        int warnCount = 0;
        for (const auto& r : m_inspectionRecords) {
            if (r.status.contains("正常")) normalCount++;
            else warnCount++;
        }

        m_inspectionSummaryLabel->setText(
            tr("📊 全站巡检已全部完成 | 总计: %1 台 | 🟢 正常: %2 | ⚠️ 待核查/离线: %3")
                .arg(m_patrolCameraIdQueue.size()).arg(normalCount).arg(warnCount));

        onLogReceived(tr("[PATROL] 🎉 全站巡检顺利完成！总设备: %1 台，正常: %2 台，异常/待核查: %3 台。")
            .arg(m_patrolCameraIdQueue.size()).arg(normalCount).arg(warnCount));
        return;
    }

    int row = m_patrolCurrentIndex;
    QString camId = m_patrolCameraIdQueue[row];
    m_inspectionTable->item(row, 7)->setText(tr("📸 抓拍识别中..."));

    DiscoveredCameraItem dev = DeviceConfigManager::instance().getDevice(camId);

    // 1. Snapshot capture
    cv::Mat frame;
    if (dev.protocolType == CameraProtocolType::LOCAL_FILE) {
        if (QFileInfo::exists(m_currentImagePath)) {
            frame = cv::imread(m_currentImagePath.toStdString());
        } else if (QFileInfo::exists("test_images/5-1-2.png")) {
            frame = cv::imread("test_images/5-1-2.png");
        }
    } else if (dev.protocolType == CameraProtocolType::ONVIF_RTSP_STREAM) {
        ONVIFDeviceInfo devInfo;
        devInfo.ipAddress = dev.ipOrSn.toStdString();
        devInfo.port = dev.port > 0 ? dev.port : 80;
        devInfo.rtspUri = dev.rtspUrl.isEmpty() ?
            ("rtsp://admin:@" + dev.ipOrSn.toStdString() + ":554/h264/ch1/main/av_stream") : dev.rtspUrl.toStdString();
        ONVIFDeviceManager mgr(devInfo);
        mgr.captureSingleSnapshot(frame);
    }

    if (frame.empty()) {
        if (!m_currentFrame.empty()) frame = m_currentFrame.clone();
    }

    // 2. Algorithm recognition
    InspectionRecord& rec = m_inspectionRecords[row];
    rec.timestamp = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");

    if (frame.empty()) {
        rec.status = tr("🔴 抓拍失败/离线");
        m_inspectionTable->item(row, 7)->setText(rec.status);
        m_inspectionTable->item(row, 7)->setForeground(QBrush(QColor("#f44336")));
        m_inspectionTable->item(row, 8)->setText(rec.timestamp);
    } else {
        GaugeProfile prof;
        QString profId = dev.defaultProfileId.isEmpty() ? "PRESSURE_GAUGE_1.6MPA" : dev.defaultProfileId;
        prof.loadFromFile(m_currentConfigPath.toStdString(), profId.toStdString());

        // Overlay camera's calibration
        prof.gauge_id = dev.cameraId.toStdString();
        prof.perspective_correction_enabled = dev.perspectiveEnabled;
        prof.perspective_pts = {
            cv::Point2f(static_cast<float>(dev.pt0_x), static_cast<float>(dev.pt0_y)),
            cv::Point2f(static_cast<float>(dev.pt1_x), static_cast<float>(dev.pt1_y)),
            cv::Point2f(static_cast<float>(dev.pt2_x), static_cast<float>(dev.pt2_y)),
            cv::Point2f(static_cast<float>(dev.pt3_x), static_cast<float>(dev.pt3_y))
        };
        prof.center = cv::Point2f(static_cast<float>(dev.centerX), static_cast<float>(dev.centerY));
        prof.pointerBaseRadius = dev.pointerBaseRadius;
        prof.calibrated_width = dev.calibratedWidth;
        prof.calibrated_height = dev.calibratedHeight;
        prof.auto_detect_roi = false;

        GaugeResult res = m_engine.process(frame, prof);

        rec.readingValue = res.value;
        rec.unit = prof.unit;
        rec.pointerAngle = res.pointer_angle_deg;
        rec.confidence = res.confidence * 100.0;
        rec.status = (res.success && res.confidence >= 0.40) ? tr("🟢 正常") : tr("⚠️ 待复核");

        // Store intermediate images and diagnostics in record
        rec.step1_original = res.step1_original.clone();
        rec.step2_rectified = res.step2_rectified.clone();
        rec.step4_recognition = res.step4_recognition.clone();
        rec.diagnosticText = QString::fromStdString(res.detailedDebugInfo);

        // Persist snapshots to disk
        QString snapshotDir = QCoreApplication::applicationDirPath() + "/inspection_snapshots";
        QDir().mkpath(snapshotDir);
        QString step2File = QString("%1/%2_step2_rectified.png").arg(snapshotDir).arg(camId);
        QString step4File = QString("%1/%2_step4_recognition.png").arg(snapshotDir).arg(camId);
        if (!rec.step2_rectified.empty()) {
            cv::imwrite(step2File.toStdString(), rec.step2_rectified);
            rec.step2Path = step2File;
        }
        if (!rec.step4_recognition.empty()) {
            cv::imwrite(step4File.toStdString(), rec.step4_recognition);
            rec.step4Path = step4File;
        }

        m_inspectionTable->item(row, 4)->setText(QString::asprintf("%.2f %s", rec.readingValue, rec.unit.c_str()));
        m_inspectionTable->item(row, 5)->setText(QString::asprintf("%.1f°", rec.pointerAngle));
        m_inspectionTable->item(row, 6)->setText(QString::asprintf("%.1f%%", rec.confidence));
        m_inspectionTable->item(row, 7)->setText(rec.status);
        m_inspectionTable->item(row, 7)->setForeground(rec.status.contains("正常") ? QBrush(QColor("#00e676")) : QBrush(QColor("#ffb300")));
        m_inspectionTable->item(row, 8)->setText(rec.timestamp);

        QWidget* w = m_inspectionTable->cellWidget(row, 9);
        if (w) {
            QPushButton* btn = qobject_cast<QPushButton*>(w);
            if (btn) {
                btn->setText(tr("📸 查看照片"));
                btn->setStyleSheet(
                    "QPushButton { background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #00897b, stop:1 #00bcd4); "
                    "color: #ffffff; border: 1px solid #4dd0e1; border-radius: 4px; padding: 3px 8px; font-weight: bold; font-size: 12px; } "
                    "QPushButton:hover { background: #00e5ff; color: #0a0b0e; }"
                );
            }
        }

        onLogReceived(tr("[PATROL] (%1/%2) 测点 [%3] 读数: %4 %5 (置信度: %6%) -> %7")
            .arg(row + 1).arg(m_patrolCameraIdQueue.size())
            .arg(camId).arg(rec.readingValue).arg(QString::fromStdString(rec.unit))
            .arg(rec.confidence, 0, 'f', 1).arg(rec.status));
    }

    m_patrolCurrentIndex++;
    m_inspectionProgressBar->setValue(m_patrolCurrentIndex * 100 / m_patrolCameraIdQueue.size());

    // Schedule next
    QTimer::singleShot(40, this, &MainWindow::onPatrolNextDevice);
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
    const InspectionRecord& rec = m_inspectionRecords[row];

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
    QString statusColor = rec.status.contains("正常") ? "#00e676" : (rec.status.contains("复核") ? "#ffb300" : "#f44336");
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

    QString readingStr = (rec.readingValue == 0.0 && !rec.status.contains("正常")) ? "--" : QString::asprintf("%.2f %s", rec.readingValue, rec.unit.c_str());
    QString angleStr = (rec.pointerAngle == 0.0 && !rec.status.contains("正常")) ? "--" : QString::asprintf("%.1f°", rec.pointerAngle);
    QString confStr = (rec.confidence == 0.0 && !rec.status.contains("正常")) ? "--" : QString::asprintf("%.1f%%", rec.confidence);
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
    QGroupBox *step2Box = new QGroupBox(tr("【步骤 2】切点视场透视校正正置图 (FOV Rectified Dial)"), &dlg);
    QVBoxLayout *step2Layout = new QVBoxLayout(step2Box);
    step2Layout->setContentsMargins(10, 10, 10, 10);

    QLabel *step2Desc = new QLabel(tr("📐 基于表盘4边缘切点透视展开，消除俯仰/侧倾视角畸变，展开为标准正面正置表盘"), step2Box);
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
        step4Label->setText(tr("<div align='center' style='color:#757575;'><br><br><br>🎯 暂无步骤 4 识别图<br><br><span style='font-size:12px;'>该设备尚未完成巡检抓拍识别</span></div>"));
    }
    step4Layout->addWidget(step4Label, 1);
    imagesLayout->addWidget(step4Box, 1);

    mainLayout->addLayout(imagesLayout, 1);

    // 4. Diagnostic Info Section (if available)
    if (!rec.diagnosticText.isEmpty()) {
        QLabel *diagLabel = new QLabel(tr("ℹ️ 算法诊断与特征提取详情: %1").arg(rec.diagnosticText.trimmed().replace("\n", "  |  ")), &dlg);
        diagLabel->setStyleSheet("background-color: #12141d; color: #81d4fa; border: 1px solid #1e2230; border-radius: 4px; padding: 6px 10px; font-size: 11px;");
        diagLabel->setWordWrap(true);
        mainLayout->addWidget(diagLabel);
    }

    // 5. Bottom Action Bar
    QHBoxLayout *actionLayout = new QHBoxLayout();
    actionLayout->setSpacing(10);

    QPushButton *exportBtn = new QPushButton(tr("💾 导出两张过程照片到本地..."), &dlg);
    exportBtn->setStyleSheet("QPushButton { background-color: #00695c; color: #80cbc4; border: 1px solid #00897b; } "
                            "QPushButton:hover { background-color: #00897b; color: #ffffff; }");
    connect(exportBtn, &QPushButton::clicked, &dlg, [this, &rec, step2Mat, step4Mat]() {
        if (step2Mat.empty() && step4Mat.empty()) {
            QMessageBox::warning(this, tr("提示"), tr("当前记录中无有效图片可供导出！"));
            return;
        }
        QString dir = QFileDialog::getExistingDirectory(this, tr("选择保存目录"), "");
        if (!dir.isEmpty()) {
            bool ok2 = false, ok4 = false;
            QString s2 = QString("%1/%2_step2_rectified.png").arg(dir).arg(rec.cameraId);
            QString s4 = QString("%1/%2_step4_recognition.png").arg(dir).arg(rec.cameraId);
            if (!step2Mat.empty()) ok2 = cv::imwrite(s2.toStdString(), step2Mat);
            if (!step4Mat.empty()) ok4 = cv::imwrite(s4.toStdString(), step4Mat);
            QMessageBox::information(this, tr("导出成功"), tr("已保存过程照片至目录:\n%1\n• 步骤2 (视场校正图): %2\n• 步骤4 (最终识别图): %3")
                                     .arg(dir).arg(ok2 ? "成功" : "无").arg(ok4 ? "成功" : "无"));
        }
    });
    actionLayout->addWidget(exportBtn);

    QPushButton *switchDebugBtn = new QPushButton(tr("🔍 载入此设备并跳转至算法调试页"), &dlg);
    switchDebugBtn->setStyleSheet("QPushButton { background-color: #1a365d; color: #90cdf4; border: 1px solid #2b6cb0; } "
                                  "QPushButton:hover { background-color: #2b6cb0; color: #ffffff; }");
    connect(switchDebugBtn, &QPushButton::clicked, &dlg, [this, &rec, &dlg]() {
        dlg.accept();
        QListWidgetItem *it = findListItemByCameraId(rec.cameraId);
        if (it) {
            m_deviceListWidget->setCurrentItem(it);
        }
        switchActiveCameraSource(rec.cameraId);
        if (m_visionTabWidget) {
            m_visionTabWidget->setCurrentIndex(1);
        }
        onRunRecognition();
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


