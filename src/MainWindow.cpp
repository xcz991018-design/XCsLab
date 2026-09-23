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
#include <chrono>
#include "SDKCameraSourceStub.hpp"
#include "FileCameraSource.hpp"

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

    // Default paths setup
    m_currentImagePath = "test_images/5-1-2.png";
    m_currentConfigPath = "config/cameras.json";

    if (!QFileInfo::exists(m_currentConfigPath) && QFileInfo::exists("C:/Users/95254/code/GaugeReaderTest/config/cameras.json")) {
        m_currentConfigPath = "C:/Users/95254/code/GaugeReaderTest/config/cameras.json";
    }

    m_imagePathEdit->setText(m_currentImagePath);
    m_configPathEdit->setText(m_currentConfigPath);

    // Populate Camera List
    updateCameraListFromConfig(m_currentConfigPath);

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
    QGroupBox *devBox = new QGroupBox(tr("摄像头接入控制 (后台探针 🟢 运行中)"), sidebar);
    QVBoxLayout *devLayout = new QVBoxLayout(devBox);

    m_autoDiscoverBtn = new QPushButton(tr("🔄 立即刷新设备 / 手动探针扫描"), devBox);
    m_autoDiscoverBtn->setToolTip(tr("点击立即向局域网广播 ONVIF/GB28181/JLink 探针报文刷新在线列表"));
    m_autoDiscoverBtn->setStyleSheet("QPushButton { background-color: #0091ea; color: white; font-weight: bold; padding: 8px; border-radius: 4px; }"
                                     "QPushButton:hover { background-color: #00b0ff; }"
                                     "QPushButton:pressed { background-color: #007cbb; }");
    devLayout->addWidget(m_autoDiscoverBtn);

    m_deviceFilterEdit = new QLineEdit(devBox);
    m_deviceFilterEdit->setPlaceholderText(tr("🔍 输入 IP 或名称快速筛选设备..."));
    m_deviceFilterEdit->setClearButtonEnabled(true);
    m_deviceFilterEdit->setStyleSheet("QLineEdit { background-color: #1a1b24; border: 1px solid #00e5ff; border-radius: 4px; padding: 5px; color: #00e5ff; font-weight: bold; }");
    devLayout->addWidget(m_deviceFilterEdit);

    m_deviceListWidget = new QListWidget(devBox);
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

    m_calibrateBtn = new QPushButton(tr("🎯 视场切点校准)"), fileBox);
    m_calibrateBtn->setStyleSheet("background-color: #0288d1; color: white; font-weight: bold; padding: 6px; border-radius: 4px;");
    fileLayout->addWidget(m_calibrateBtn);

    m_runBtn = new QPushButton(tr("▶ 运行"), fileBox);
    m_runBtn->setStyleSheet("background-color: #00897b; color: white; font-weight: bold; font-size: 13px; padding: 9px; border-radius: 6px;");
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
    connect(m_deviceFilterEdit, &QLineEdit::textChanged, this, &MainWindow::onFilterDeviceList);
    connect(m_deviceListWidget, &QListWidget::itemSelectionChanged, this, &MainWindow::onCameraSelectionChanged);
    connect(m_deviceListWidget, &QListWidget::itemDoubleClicked, this, &MainWindow::onEditCameraInfo);
    connect(m_browseImgBtn, &QPushButton::clicked, this, &MainWindow::onSelectImage);
    connect(m_browseCfgBtn, &QPushButton::clicked, this, &MainWindow::onLoadConfig);
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

void MainWindow::updateCameraListFromConfig(const QString& configPath) {
    if (!m_deviceListWidget) return;

    m_deviceListWidget->clear();
    m_discoveredItemsMap.clear();

    QStringList ids = loadCameraIdsFromConfig(configPath);
    if (ids.isEmpty()) {
        ids << "CAM_5-3-1" << "CAM_5-1-2" << "CAM_5-3-4" << "CAM_5-4-1" << "CAM_20";
    }

    for (const auto& id : ids) {
        DiscoveredCameraItem item;
        item.cameraId = id;
        if (id.startsWith("CAM_IP_")) {
            QString ipStr = id.mid(7);
            ipStr.replace('_', '.');
            item.ipOrSn = ipStr;
            item.protocolType = CameraProtocolType::SDK_JLINK;
            item.protocolName = "Xiongmai JLink Camera";
            item.modelName = "JFTech Xiongmai Camera";
        } else {
            item.ipOrSn = "192.168.1." + QString::number(100 + m_discoveredItemsMap.size());
            item.protocolType = CameraProtocolType::LOCAL_FILE;
            item.protocolName = "智能图像采集终端";
            item.modelName = "Pressure Gauge Sensor Node";
        }
        item.isOnline = true;
        item.defaultProfileId = "PRESSURE_GAUGE_1.6MPA";

        onDiscoveredCameraAdded(item);
    }
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

    if (m_deviceListWidget) {
        m_deviceListWidget->clear();
        m_discoveredItemsMap.clear();
        if (m_statusCameraCountBadge) {
            m_statusCameraCountBadge->setText(tr("📷 在线设备: 0 台"));
        }
    }

    m_discoveryService->triggerScanNow();
    QTimer::singleShot(2500, this, [this]() {
        m_autoDiscoverBtn->setEnabled(true);
    });
}

void MainWindow::onDiscoveredCameraAdded(const DiscoveredCameraItem& item) {
    if (m_discoveredItemsMap.contains(item.cameraId)) {
        // Upgrade existing local file entry if a real network camera is discovered
        auto existing = m_discoveredItemsMap.value(item.cameraId);
        if (existing.protocolType == CameraProtocolType::LOCAL_FILE && item.protocolType != CameraProtocolType::LOCAL_FILE) {
            m_discoveredItemsMap[item.cameraId] = item;
            for (int i = 0; i < m_deviceListWidget->count(); ++i) {
                QListWidgetItem *it = m_deviceListWidget->item(i);
                if (it && it->data(Qt::UserRole).toString() == item.cameraId) {
                    QString text = QString("📷 %1\n   地址/SN: %2 | %3")
                                       .arg(item.cameraId)
                                       .arg(item.ipOrSn)
                                       .arg(item.modelName);
                    it->setText(text);
                    break;
                }
            }
        }
        return;
    }

    m_discoveredItemsMap.insert(item.cameraId, item);

    QString text = QString("📷 %1\n   地址/SN: %2 | %3")
                       .arg(item.cameraId)
                       .arg(item.ipOrSn)
                       .arg(item.modelName);

    QListWidgetItem *listItem = new QListWidgetItem(text, m_deviceListWidget);
    listItem->setData(Qt::UserRole, item.cameraId);

    if (m_deviceFilterEdit) {
        QString filterKey = m_deviceFilterEdit->text().trimmed().toLower();
        if (!filterKey.isEmpty() && !text.toLower().contains(filterKey)) {
            listItem->setHidden(true);
        }
    }

    m_statusCameraCountBadge->setText(tr("📷 在线设备: %1 台").arg(m_discoveredItemsMap.size()));
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
    onLogReceived(tr("[CAMERA] Switching active source node to %1 (%2)...").arg(camId).arg(item.protocolName));

    if (m_activeCameraSource) {
        m_activeCameraSource->close();
    }

    if (item.protocolType == CameraProtocolType::GB28181_PS_STREAM) {
        m_activeCameraSource = m_gbManager;
    } else if (item.protocolType == CameraProtocolType::ONVIF_RTSP_STREAM) {
        m_onvifManager->setSampleFrame(cv::Mat()); // Clear sample frame so network device doesn't display 5-1-2.png
        m_onvifManager->setDeviceIp(item.ipOrSn.toStdString());
        m_activeCameraSource = m_onvifManager;
    } else if (item.protocolType == CameraProtocolType::SDK_JLINK) {
        auto jlinkSource = std::make_shared<SDKCameraSourceStub>(item.ipOrSn.toStdString(), 0, CameraProtocolType::SDK_JLINK);
        jlinkSource->setJLinkConfig("DEMO_UUID", "DEMO_KEY", "DEMO_SECRET", "DEMO_MOVECARD");
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

    m_profile.loadFromFile(m_currentConfigPath.toStdString(), camId.toStdString());

    // Instantly update stream view without blocking main thread
    onUpdateLiveStreamFrame();
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

    onLogReceived(tr("[CAPTURE] 成功抓取摄像头 [%1] 的画面快照 (%2x%3)。")
                      .arg(activeCamId).arg(m_lastCapturedFrame.cols).arg(m_lastCapturedFrame.rows));
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

    CardinalSelectorDialog dialog(m_currentFrame, m_profile, m_currentConfigPath, activeCam, this);
    if (dialog.exec() == QDialog::Accepted) {
        onRunRecognition();
    }
}

void MainWindow::onEditCameraInfo(QListWidgetItem *item) {
    if (!item) return;

    QString camId = item->data(Qt::UserRole).toString();
    if (camId.isEmpty()) return;

    CameraInfoEditDialog dialog(m_currentConfigPath, camId, this);
    if (dialog.exec() == QDialog::Accepted) {
        QString newCamId = dialog.getUpdatedCameraId();
        onLogReceived(tr("[CAMERA] 已成功更新摄像头节点 [%1] 属性参数档案到 %2。").arg(newCamId).arg(m_currentConfigPath));

        updateCameraListFromConfig(m_currentConfigPath);

        for (int i = 0; i < m_deviceListWidget->count(); ++i) {
            QListWidgetItem *it = m_deviceListWidget->item(i);
            if (it && it->data(Qt::UserRole).toString() == newCamId) {
                m_deviceListWidget->setCurrentItem(it);
                break;
            }
        }
    }
}

void MainWindow::onRunRecognition() {
    if (m_watcher.isRunning()) return;

    QString activeCam = "CAM_5-3-1";
    if (m_deviceListWidget && m_deviceListWidget->currentItem()) {
        activeCam = m_deviceListWidget->currentItem()->data(Qt::UserRole).toString();
    }

    m_profile.loadFromFile(m_currentConfigPath.toStdString(), activeCam.toStdString());

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

    if (m_latestResult.success) {
        m_valLabel->setText(QString(tr("<b>读取数值:</b> <font color='#00e676' size='+2'>%.3f %1</font>"))
                                .arg(QString::fromStdString(m_latestResult.unit)).arg(m_latestResult.value));
        m_angleLabel->setText(QString(tr("指针角度: %.1f°")).arg(m_latestResult.pointer_angle_deg));
        m_centerLabel->setText(QString(tr("表盘中心: (%.1f, %.1f) px  R=%.1f")).arg(m_latestResult.center.x).arg(m_latestResult.center.y).arg(m_latestResult.radius));
        m_confidenceLabel->setText(QString(tr("识别置信度: %.1f %%")).arg(m_latestResult.confidence * 100.0));
        m_timeLabel->setText(QString(tr("算法耗时: %.2f ms")).arg(elapsedMs));
        if (m_latestResult.centerInconsistent) {
            m_statusLabel->setText(QString(tr("<font color='#ffab00'>⚠️ 警告: %1</font>")).arg(QString::fromStdString(m_latestResult.warningMessage)));
            onLogReceived(QString::fromStdString(m_latestResult.warningMessage));
        } else {
            m_statusLabel->setText(tr("<font color='#00e676'>系统状态: 4步/12步全流程识别完成，数据已输出</font>"));
        }

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

        onLogReceived(tr("[PIPELINE] 仪表识别完成: 数值=%.3f %1, 耗时=%.2f ms, 已生成全套 12 个中间处理步骤图。")
                          .arg(m_latestResult.value).arg(QString::fromStdString(m_latestResult.unit)).arg(elapsedMs));
    } else {
        m_statusLabel->setText(QString(tr("<font color='#ff5252'>错误: %1</font>")).arg(QString::fromStdString(m_latestResult.errorMessage)));
        onLogReceived(tr("[PIPELINE ERROR] %1").arg(QString::fromStdString(m_latestResult.errorMessage)));
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

