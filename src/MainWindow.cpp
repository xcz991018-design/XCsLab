#include "DeviceConnection.hpp"
#include "UiTheme.hpp"
#include "AccessControl.hpp"
#include "MainWindow.hpp"
#include "OnvifSoap.hpp"
#include <QBrush>
#include <QColor>
#include "InspectionCalibration.hpp"
#include "DeviceCodec.hpp"
#include "DeviceArchiveSummary.hpp"
#include "RecognitionQuality.hpp"
#include "CardinalSelectorDialog.hpp"
#include "ScaleCalibration.hpp"
#include "IsolatedCapture.hpp"
#include "SnapshotFrameReader.hpp"
#include "CameraInfoEditDialog.hpp"
#include "GaugeTemplateDialog.hpp"
#include <QFileDialog>
#include <QMessageBox>
#include <QFileInfo>
#include <QFile>
#include <QSaveFile>
#include "CsvReport.hpp"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDateTime>
#include <QDebug>
#include <QMenu>
#include <QSettings>
#include <QAction>
#include <QApplication>
#include <QPainter>
#include <QTabBar>
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
    connect(m_discoveryService, &AutoCameraDiscoveryService::discoveryCompleted, this, [this](int) {
        if (m_autoDiscoverBtn) m_autoDiscoverBtn->setEnabled(true);
    });
    connect(m_discoveryService, &AutoCameraDiscoveryService::cameraOnlineStateChanged, this,
            [this](const QString& cameraId, bool online) {
        if (!m_discoveredItemsMap.contains(cameraId)) return;
        DeviceConfigManager::instance().markDeviceOnline(cameraId, online);
        auto& device = m_discoveredItemsMap[cameraId];
        device.isOnline = online;
        if (online) m_offlineCameraIds.remove(cameraId);
        else {
            m_offlineCameraIds.insert(cameraId);
            m_lastClockCheck.remove(cameraId);
            if (cameraId == m_previewCameraId) m_cameraHasFrame = false;
        }
        if (online) device.lastSeenTime = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");
        if (auto* row = findListItemByCameraId(cameraId)) updateDeviceListItemWidget(row, device);
        if (m_deviceListWidget->currentItem() &&
            m_deviceListWidget->currentItem()->data(Qt::UserRole).toString() == cameraId)
            updateSelectedDeviceArchive(cameraId);
        if (online) checkAndSynchronizeCameraClock(cameraId);
    });

    setupUi();
    applyDarkStyle();
    setupSystemTray();

    m_inspectionService = new InspectionService(this);
    connect(m_inspectionService, &InspectionService::evidenceMaintenanceFinished, this,
        [this](const QString& report) { onLogReceived("[STORAGE] "+report); });
    connect(m_inspectionService, &InspectionService::taskFinished, this, &MainWindow::onInspectionOutcome);
    connect(m_inspectionService, &InspectionService::taskStarted, this, [this](const QString&, const QString& cameraId) {
        m_discoveryService->deferHealthForCapture(cameraId);
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
        updatePatrolControls();
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
    connect(m_singleService, &InspectionService::taskStarted, this, [this](const QString&, const QString& cameraId) {
        if (m_singleSubmittedTask.inputKind == InspectionInputKind::CameraSnapshot)
            m_discoveryService->deferHealthForCapture(cameraId);
    });
    m_previewService = new CameraPreviewService(this);
    connect(m_singleService, &InspectionService::taskFinished, this, [this](const InspectionOutcome& out) {
        const auto submitted = m_singleSubmittedTask;
        if (submitted.inputKind == InspectionInputKind::CameraSnapshot)
            m_discoveryService->recordInspectionState(out.cameraId, out.errorCode, out.capturedAt.isValid());
        if (!out.databaseError.isEmpty()) onLogReceived(tr("[数据库] %1 · 待同步: %2").arg(out.databaseError).arg(out.databasePending));
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
            updateImageCard(m_originalImage, m_currentFrame, "Task original");
        } else {
            m_currentFrame.release();
            m_currentImagePath.clear();
            m_hasAnalysisInput = false;
        }
        if (!submitted.captureOnly || !out.errorCode.isEmpty()) {
            m_latestResult = out.result;
            if (!out.errorCode.isEmpty()) {
                m_latestResult.success = false;
                m_latestResult.errorMessage = (out.errorCode + ": " + out.errorMessage).toStdString();
            }
            onRecognitionFinished();
            if (!out.capturedFrame.empty()) updateImageCard(m_originalImage, out.capturedFrame, "Task original");
        } else m_statusLabel->setText(tr("图片已就绪；分析将仅使用此任务对应的指定图片。"));
    });
    connect(m_singleService, &InspectionService::finished, this, [this](bool cancelled) {
        if (cancelled) m_statusLabel->setText(tr("单设备任务已取消。"));
    });
    connect(m_previewService, &CameraPreviewService::frameReady, this,
            [this](const QString& cameraId, const FrameCaptureResult& result) {
        if (cameraId != m_previewCameraId) return;
        const bool hadFrame = m_cameraHasFrame;
        if (!result.ok()) {
            m_cameraHasFrame = false;
        }
        if (result.ok()) {
            if (!m_cameraHasFrame) m_calibrationChecked.remove(cameraId);
            m_cameraHasFrame = true;
            checkCameraCalibration(cameraId);
            tryOpenCameraCalibration();
        }
        m_discoveryService->recordInspectionState(cameraId,
            QString::fromLatin1(captureFailureName(result.failure)), result.ok());
        if (hadFrame != m_cameraHasFrame) {
            if (auto* row = findListItemByCameraId(cameraId))
                updateDeviceListItemWidget(row, m_discoveredItemsMap.value(cameraId));
        }
        if (!m_isLiveStreaming) return;
        m_liveCamInfoBadge->setText(tr("设备: %1 | 采集状态: %2").arg(cameraId, captureFailureName(result.failure)));
        if (m_visionTabWidget->currentWidget() == m_livePage) {
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
    m_clockCheckTimer = new QTimer(this);
    m_clockCheckTimer->setInterval(5 * 60 * 1000);
    connect(m_clockCheckTimer, &QTimer::timeout, this, [this]() {
        const auto devices = m_discoveredItemsMap.values();
        for (const auto& device : devices)
            if (device.isOnline) checkAndSynchronizeCameraClock(device.cameraId);
    });
    m_clockCheckTimer->start();

    m_currentImagePath.clear();
    connect(&DeviceConfigManager::instance(), &DeviceConfigManager::archiveSaveFailed, this,
            [this](const QString& stage, const QString& path, const QString& reason) {
        const bool queue = stage == "REGISTRY_QUEUE_FAILED";
        const auto message = tr("[%1] %2 | 阶段: %3 | 文件: %4 | 原因: %5")
            .arg(queue ? "REGISTRY-QUEUE" : "LOCAL-ARCHIVE",
                 queue ? tr("同步队列提交失败") : tr("本地档案保存失败"), stage, path, reason);
        m_databaseStatusLabel->setText(queue ? tr("档案待提交同步队列") : tr("本地档案未保存"));
        onLogReceived(message);
    });
    connect(&DeviceConfigManager::instance(), &DeviceConfigManager::archiveSaveRecovered, this,
            [this](const QString& failedPath, const QString& savedPath) {
        onLogReceived(tr("[LOCAL-ARCHIVE] 保存已恢复 | 原路径: %1 | 已改用用户数据目录: %2").arg(failedPath, savedPath));
    });

    // Load archives without opening a stream; connection is an explicit context-menu action.
    loadAndRestoreSavedDevices();

    if (DatabaseSettings::fromEnvironment().enabled) {
        connect(&DeviceConfigManager::instance(), &DeviceConfigManager::databaseStatus, this, [this](const QString& error) {
            if (error == "REGISTRY_SYNC_IN_PROGRESS") {
                m_databaseStatusLabel->setText(tr("数据库 档案正在同步…")); return;
            }
            m_databaseStatusLabel->setText(error.isEmpty() ? tr("数据库 档案已同步") : tr("数据库 待同步: %1").arg(error));
            if (!error.isEmpty()) {
                const bool queue = error.contains("OUTBOX_");
                onLogReceived(queue ? tr("[REGISTRY-QUEUE] 同步队列处理失败: %1").arg(error)
                                    : tr("[数据库] 档案同步失败: %1").arg(error));
            }
        });
        connect(m_inspectionService, &InspectionService::databaseSyncFinished, this, [this](const QString& error) {
            if (error == "REGISTRY_SYNC_IN_PROGRESS") {
                m_databaseStatusLabel->setText(tr("数据库 档案正在同步…")); return;
            }
            m_databaseStatusLabel->setText(error.isEmpty() ? tr("数据库 同步检查完成") : tr("数据库 待同步: %1").arg(error));
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
}

void MainWindow::setupUi() {
    setWindowTitle(tr("工业指针仪表智能感知识别与检测平台"));
    resize(1550, 960);

    QWidget *centralWidget = new QWidget(this);
    setCentralWidget(centralWidget);

    QVBoxLayout *rootLayout = new QVBoxLayout(centralWidget);
    rootLayout->setContentsMargins(10, 10, 10, 10);
    rootLayout->setSpacing(8);

    // 1. Top Banner
    createTopBanner(rootLayout);
    auto* databaseBar = new QHBoxLayout;
    auto* connectionButton = new QPushButton(tr("数据库连接配置"), centralWidget);
    connectionButton->setObjectName("mysqlConnectionConfigurationButton");
    auto* historyButton = new QPushButton(tr("历史数据"), centralWidget);
    historyButton->setEnabled(DatabaseSettings::fromEnvironment().enabled);
    m_databaseStatusLabel = new QLabel(DatabaseSettings::fromEnvironment().enabled ? tr("数据库正在加载档案…")
        : DatabaseSettings::fromEnvironment().configurationError.isEmpty() ? tr("数据库未配置；请点击数据库连接配置")
        : DatabaseSettings::fromEnvironment().configurationError, centralWidget);
    connectionButton->setVisible(AccessControl::administrator());
    historyButton->setVisible(AccessControl::administrator());
    auto* accountsButton = new QPushButton(tr("角色与账号管理"), centralWidget);
    accountsButton->setObjectName("roleManagementButton");
    accountsButton->setVisible(true);
    connect(accountsButton, &QPushButton::clicked, this, [this] {
        if (!manageApplicationAccounts(this)) return;
        setEnabled(false);
        if (m_databaseSyncTimer) m_databaseSyncTimer->stop();
        if (m_liveTimer) m_liveTimer->stop();
        if (m_autoPatrolTimer) m_autoPatrolTimer->stop();
        if (m_clockCheckTimer) m_clockCheckTimer->stop();
        if (m_inspectionService) m_inspectionService->stop();
        if (m_singleService) m_singleService->stop();
        if (m_previewService) m_previewService->stop();
        if (m_discoveryService) m_discoveryService->stopHealthMonitoring();
        if (m_trayIcon) m_trayIcon->hide();
        hide();
        emit logoutRequested();
    });
    databaseBar->addWidget(connectionButton);
    auto* templateButton = new QPushButton(tr("表计模板管理"), centralWidget);
    templateButton->setVisible(AccessControl::administrator());
    templateButton->setObjectName("gaugeTemplateManagementButton");
    databaseBar->addWidget(templateButton);
    connect(templateButton, &QPushButton::clicked, this, &MainWindow::onManageGaugeTemplates);
    databaseBar->addWidget(historyButton);
    databaseBar->addWidget(m_databaseStatusLabel, 1);
    databaseBar->addWidget(accountsButton);
    const auto currentRole = AccessControl::administrator() ? tr("管理员") : tr("操作员");
    auto* accountLabel = new QLabel(centralWidget);
    accountLabel->setTextFormat(Qt::PlainText);
    accountLabel->setText(tr("当前角色：%1  |  用户名：%2").arg(currentRole, AccessControl::username()));
    accountLabel->setObjectName("currentAccountLabel");
    databaseBar->addWidget(accountLabel);
    rootLayout->addLayout(databaseBar);
    connect(connectionButton, &QPushButton::clicked, this, [this]() {
        if (!AccessControl::administrator()) return;
        DatabaseConnectionDialog dialog(this);
        dialog.exec();
    });
    connect(historyButton, &QPushButton::clicked, this, [this]() {
        if (!AccessControl::administrator()) return;
        DatabaseHistoryDialog dialog(this, [this](InspectionTask task) {
            if (m_singleService->isRunning()) {
                onLogReceived(tr("[数据库] 单设备任务正在运行，请完成后重新分析历史原图。"));
                return;
            }
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




    m_statusCameraCountBadge = new QLabel(tr("📷 在线设备: 5 台"), bannerBox);
    m_statusCameraCountBadge->setStyleSheet("background-color: #311b92; color: #e040fb; border: 1px solid #d500f9; border-radius: 12px; padding: 4px 10px; font-weight: bold;");

    bannerLayout->addWidget(m_statusCameraCountBadge);

    rootLayout->addWidget(bannerBox);
}

QWidget* MainWindow::createLeftSidebar(QWidget *parent) {
    QWidget *sidebar = new QWidget(parent);
    QVBoxLayout *sideLayout = new QVBoxLayout(sidebar);
    sideLayout->setContentsMargins(0, 0, 0, 0);
    sideLayout->setSpacing(8);
    auto* sidebarSplitter = new QSplitter(Qt::Vertical, sidebar);
    sidebarSplitter->setChildrenCollapsible(false);
    sidebarSplitter->setHandleWidth(6);

    // 1. Auto-Discovery & Devices Box
    QGroupBox *devBox = new QGroupBox(tr("摄像头设备管理库（左键查看档案，右键连接）"), sidebar);
    QVBoxLayout *devLayout = new QVBoxLayout(devBox);

    QHBoxLayout *devBtnLayout = new QHBoxLayout();
    devBtnLayout->setSpacing(6);

    m_autoDiscoverBtn = new QPushButton(tr("🔄 刷新设备"), devBox);
    m_autoDiscoverBtn->setToolTip(tr("点击后搜索局域网新设备；不会清空已知设备或断开当前连接。后台不自动搜索新设备。"));
    m_autoDiscoverBtn->setStyleSheet("QPushButton { background-color: #0091ea; color: white; font-weight: bold; padding: 7px; border-radius: 4px; }"
                                     "QPushButton:hover { background-color: #00b0ff; }"
                                     "QPushButton:pressed { background-color: #007cbb; }");

    m_addDeviceBtn = new QPushButton(tr("➕ 添加"), devBox);
    m_addDeviceBtn->setToolTip(tr("手动登记并添加局域网或指定IP网络摄像头到持久化设备库"));
    m_addDeviceBtn->setStyleSheet("QPushButton { background-color: #00897b; color: white; font-weight: bold; padding: 7px; border-radius: 4px; }"
                                  "QPushButton:hover { background-color: #00bfa5; }");

    m_autoDiscoverBtn->setVisible(AccessControl::administrator());
    m_addDeviceBtn->setVisible(AccessControl::administrator());
    m_removeDeviceBtn = new QPushButton(tr("🗑️ 删除"), devBox);
    m_removeDeviceBtn->setToolTip(tr("从持久化设备库中移除当前选中的设备"));
    m_removeDeviceBtn->setVisible(AccessControl::administrator());
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
    devBox->setMinimumHeight(260);
    sidebarSplitter->addWidget(devBox);

    // 3. Profile Info Card
    QGroupBox *profileBox = new QGroupBox(tr("摄像头与仪表规格档案"), sidebar);
    QVBoxLayout *profLayout = new QVBoxLayout(profileBox);
    m_cfgGaugeId = new QLabel(tr("设备/节点编号: -"), profileBox);
    m_cfgLocationInfo = new QLabel(tr("安装位置: -"), profileBox);
    m_cfgHardwareId = new QLabel(tr("硬件 UUID: -"), profileBox);
    m_cfgGaugeSpec = new QLabel(tr("表头规格: -"), profileBox);
    m_cfgRange = new QLabel(tr("量程范围: -"), profileBox);

    m_cfgNetworkInfo = new QLabel(tr("连接信息: -"), profileBox);
    m_cfgSafeRange = new QLabel(tr("安全工作范围: -"), profileBox);
    m_cfgCalibration = new QLabel(tr("表计标定: -"), profileBox);
    auto* profileContents = new QWidget(profileBox);
    profileContents->setObjectName("deviceArchiveContents");
    profileContents->setStyleSheet("QWidget#deviceArchiveContents { background-color: #161822; }");
    auto* profileFields = new QVBoxLayout(profileContents);
    profileFields->setContentsMargins(4, 4, 4, 4);
    profileFields->setSpacing(10);
    for (auto* label : {m_cfgGaugeId, m_cfgHardwareId, m_cfgNetworkInfo, m_cfgLocationInfo,
                       m_cfgGaugeSpec, m_cfgRange, m_cfgSafeRange, m_cfgCalibration}) {
        label->setTextFormat(Qt::PlainText);
        label->setWordWrap(true);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        profileFields->addWidget(label);
    }
    auto* profileScroll = new QScrollArea(profileBox);
    profileScroll->setWidgetResizable(true);
    profileScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    profileScroll->setFrameShape(QFrame::NoFrame);
    profileScroll->setMinimumHeight(120);
    profileScroll->setWidget(profileContents);
    profLayout->addWidget(profileScroll);
    sidebarSplitter->addWidget(profileBox);
    sidebarSplitter->setStretchFactor(0, 3);
    sidebarSplitter->setStretchFactor(1, 1);
    sidebarSplitter->setSizes({560, 200});
    sideLayout->addWidget(sidebarSplitter, 1);

    // Internal status targets remain available to the recognition workflow.
    // The sidebar no longer presents a metrics/results panel.
    QWidget *resultBox = new QWidget(sidebar);
    resultBox->hide();
    QVBoxLayout *resLayout = new QVBoxLayout(resultBox);

    m_valLabel = new QLabel(tr("<b>读取数值:</b> <font color='#00e676' size='+2'>--</font>"), resultBox);
    m_centerLabel = new QLabel(tr("表盘中心: (-- , --) px"), resultBox);
    m_confidenceLabel = new QLabel(tr("识别置信度: -- %"), resultBox);
    m_timeLabel = new QLabel(tr("算法耗时: -- ms"), resultBox);
    m_statusLabel = new QLabel(tr("系统状态: 就绪"), resultBox);

    resLayout->addWidget(m_valLabel);
    resLayout->addWidget(m_centerLabel);
    resLayout->addWidget(m_confidenceLabel);
    auto qualityLabel = new QLabel(tr("旁路质量：未评估"), resultBox);
    qualityLabel->setObjectName("recognitionQuality");
    qualityLabel->setWordWrap(true);
    resLayout->addWidget(qualityLabel);
    resLayout->addWidget(m_timeLabel);
    resLayout->addWidget(m_statusLabel);

    // Connections
    connect(m_autoDiscoverBtn, &QPushButton::clicked, this, &MainWindow::onTriggerAutoDiscovery);
    connect(m_addDeviceBtn, &QPushButton::clicked, this, &MainWindow::onAddNewDevice);
    connect(m_removeDeviceBtn, &QPushButton::clicked, this, &MainWindow::onRemoveSelectedDevice);
    connect(m_deviceFilterEdit, &QLineEdit::textChanged, this, &MainWindow::onFilterDeviceList);
    connect(m_deviceListWidget, &QListWidget::currentItemChanged, this, &MainWindow::onCameraSelectionChanged);
    connect(m_deviceListWidget, &QListWidget::customContextMenuRequested, this, &MainWindow::onDeviceCustomContextMenu);

    return sidebar;
}

QWidget* MainWindow::createCenterVisionArea(QWidget *parent) {
    m_visionTabWidget = new QTabWidget(parent);
    // Keep complete tab titles when multiple vision pages are open.
    m_visionTabWidget->setElideMode(Qt::ElideNone);
    m_visionTabWidget->setUsesScrollButtons(true);
    m_visionTabWidget->setTabsClosable(true);
    m_visionTabWidget->tabBar()->setExpanding(false);
    m_visionTabWidget->setStyleSheet(
        "QTabWidget::pane { border: 1px solid #272a38; background-color: #121216; border-radius: 4px; }"
        "QTabBar::tab { background-color: #1a1c26; color: #9fa6b2; padding: 8px 16px; font-size: 13px; font-weight: bold; border: 1px solid #272a38; border-top-left-radius: 4px; border-top-right-radius: 4px; margin-right: 2px; }"
        "QTabBar::tab:selected { background-color: #272a3e; color: #00e5ff; border-bottom-color: #00e5ff; }"
        "QTabBar::tab:hover { color: #ffffff; background-color: #202332; }"
    );

    // --- Tab 0: Stage 0 摄像头实时预览 ---
    m_livePage = createLiveStreamStageArea(m_visionTabWidget);
    m_livePage->hide();

    // --- Tab 1: 采集原图与最终识别结果 ---
    QScrollArea *tab1Scroll = new QScrollArea(m_visionTabWidget);
    tab1Scroll->setWidgetResizable(true);
    tab1Scroll->setStyleSheet("QScrollArea { border: none; background-color: #121216; }");

    QWidget *gridContainer = new QWidget(tab1Scroll);
    QGridLayout *gridLayout = new QGridLayout(gridContainer);
    gridLayout->setSpacing(10);

    auto createImageCard = [this, gridContainer](const QString& title, QLabel*& outLabel) {
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

    gridLayout->addWidget(createImageCard(tr("采集原图"), m_originalImage), 0, 0);

    gridLayout->addWidget(createImageCard(tr("最终识别结果"), m_recognitionImage), 0, 1);
    gridLayout->setColumnStretch(0, 1);
    gridLayout->setColumnStretch(1, 1);
    gridLayout->setRowStretch(0, 1);

    tab1Scroll->setWidget(gridContainer);
    m_recognitionPage = tab1Scroll;
    m_recognitionPage->hide();

    // --- Tab 2: 全站设备巡检读数总览 (Batch Patrol Inspection Matrix) ---
    m_inspectionPage = createInspectionStageArea(m_visionTabWidget);
    const int inspectionIndex = m_visionTabWidget->addTab(m_inspectionPage, tr("自动巡检"));
    // The permanent patrol page must never offer a close button on either side.
    m_visionTabWidget->tabBar()->setTabButton(inspectionIndex, QTabBar::LeftSide, nullptr);
    m_visionTabWidget->tabBar()->setTabButton(inspectionIndex, QTabBar::RightSide, nullptr);
    m_visionTabWidget->setCurrentWidget(m_inspectionPage);

    connect(m_visionTabWidget, &QTabWidget::currentChanged, this, &MainWindow::onTabChanged);
    connect(m_visionTabWidget, &QTabWidget::tabCloseRequested, this, [this](int index) {
        auto* page = m_visionTabWidget->widget(index);
        if (!page || page == m_inspectionPage) return;
        if (page == m_livePage) {
            disconnectCameraPreview();
        } else if (page == m_recognitionPage) {
            m_visionTabWidget->removeTab(index);
            page->hide(); // Keep the page and its result widgets available for reopening.
        }
    });

    return m_visionTabWidget;
}

void MainWindow::showVisionPage(QWidget* page, const QString& title) {
    if (!m_visionTabWidget || !page) return;
    if (m_visionTabWidget->indexOf(page) < 0) m_visionTabWidget->addTab(page, title);
    m_visionTabWidget->setCurrentWidget(page);
}

QWidget* MainWindow::createLiveStreamStageArea(QWidget *parent) {
    QWidget *container = new QWidget(parent);
    QVBoxLayout *mainLayout = new QVBoxLayout(container);
    mainLayout->setContentsMargins(8, 8, 8, 8);
    mainLayout->setSpacing(8);

    // 1. Top Header & Control Toolbar
    QGroupBox *toolbarBox = new QGroupBox(tr("📷 摄像头实时预览"), container);
    QHBoxLayout *tbLayout = new QHBoxLayout(toolbarBox);
    tbLayout->setContentsMargins(10, 6, 10, 6);

    m_liveCamInfoBadge = new QLabel(tr("📷 当前通道: 未选择摄像头"), toolbarBox);
    m_liveCamInfoBadge->setStyleSheet("color: #00e5ff; font-weight: bold; font-size: 13px;");
    tbLayout->addWidget(m_liveCamInfoBadge);

    tbLayout->addStretch();

    m_toggleLiveBtn = new QPushButton(tr("实时预览（未连接）"), toolbarBox);
    m_toggleLiveBtn->setEnabled(false);
    m_toggleLiveBtn->setStyleSheet("QPushButton { background-color: #f57c00; color: white; font-weight: bold; padding: 6px 14px; border-radius: 4px; }"
                                   "QPushButton:hover { background-color: #ff9800; }");
    tbLayout->addWidget(m_toggleLiveBtn);

    mainLayout->addWidget(toolbarBox);

    // Real-time preview fills the available content area.
    QGroupBox *streamBox = new QGroupBox(tr("📺 摄像头实时视频传输画面 (Live Camera Stream)"), container);
    QVBoxLayout *streamLayout = new QVBoxLayout(streamBox);
    streamLayout->setContentsMargins(6, 6, 6, 6);

    m_liveStreamLabel = new QLabel(streamBox);
    m_liveStreamLabel->setAlignment(Qt::AlignCenter);
    m_liveStreamLabel->setMinimumSize(520, 380);
    m_liveStreamLabel->setStyleSheet("background-color: #0a0b0e; border: 1px solid #232635; border-radius: 4px;");
    m_liveStreamLabel->setText(tr("<font color='#555868'>请右键点击设备并选择“连接此设备”以查看实时画面</font>"));

    streamLayout->addWidget(m_liveStreamLabel, 1);
    mainLayout->addWidget(streamBox, 1);

    // Connections
    connect(m_toggleLiveBtn, &QPushButton::clicked, this, &MainWindow::onToggleLiveStream);

    return container;
}

QWidget* MainWindow::createBottomLogConsole(QWidget *parent) {
    QGroupBox *consoleBox = new QGroupBox(tr("运行日志"), parent);
    consoleBox->setFixedHeight(160);
    QVBoxLayout *layout = new QVBoxLayout(consoleBox);
    layout->setContentsMargins(6, 6, 6, 6);

    m_logConsole = new QTextEdit(consoleBox);
    m_logConsole->setReadOnly(true);
    m_logConsole->setStyleSheet("QTextEdit { background-color: #0a0b0e; color: #69f0ae; font-family: 'Consolas', 'Courier New', monospace; font-size: 12px; border: 1px solid #1e2130; }");

    auto* actions = new QHBoxLayout;
    actions->addStretch();
    auto* clearButton = new QPushButton(tr("清空日志"), consoleBox);
    clearButton->setObjectName("clearRuntimeLogButton");
    clearButton->setToolTip(tr("清空当前窗口中的运行日志。"));
    auto* exportButton = new QPushButton(tr("导出日志"), consoleBox);
    exportButton->setObjectName("exportRuntimeLogButton");
    exportButton->setToolTip(tr("将当前窗口中的运行日志导出为 UTF-8 文本文件。"));
    actions->addWidget(clearButton);
    actions->addWidget(exportButton);
    layout->addLayout(actions);
    layout->addWidget(m_logConsole, 1);
    connect(clearButton, &QPushButton::clicked, m_logConsole, &QTextEdit::clear);
    connect(exportButton, &QPushButton::clicked, this, [this] {
        const auto log = m_logConsole->toPlainText();
        if (log.isEmpty()) return;
        const auto defaultName = tr("运行日志_%1.txt")
            .arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss"));
        const auto path = QFileDialog::getSaveFileName(this, tr("导出运行日志"),
            defaultName, tr("文本文件 (*.txt);;所有文件 (*)"));
        if (path.isEmpty()) return;
        QSaveFile file(path);
        const auto bytes = log.toUtf8();
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
            QMessageBox::warning(this, tr("导出运行日志"),
                tr("日志导出失败：%1").arg(file.errorString()));
            return;
        }
        QMessageBox::information(this, tr("导出运行日志"), tr("日志已导出至：%1").arg(path));
    });
    const auto updateActions = [this, clearButton, exportButton] {
        const bool hasLog = !m_logConsole->document()->isEmpty();
        clearButton->setEnabled(hasLog);
        exportButton->setEnabled(hasLog);
    };
    connect(m_logConsole, &QTextEdit::textChanged, this, updateActions);
    updateActions();
    return consoleBox;
}

void MainWindow::applyDarkStyle() {
    QString style = R"(
        QMainWindow { background-color: #101117; }
        QSplitter::handle { background-color: #092B56; }
        QSplitter::handle:hover { background-color: #42a5f5; }
        QSplitter::handle:pressed { background-color: #0d47a1; }
        QWidget { color: #d1d5db; font-family: 'Segoe UI', Microsoft YaHei, sans-serif; font-size: 13px; }
        QMenu { background-color: #1f2230; color: #f5f7fa; border: 1px solid #4b526b; padding: 5px; }
        QMenu::item { background-color: transparent; color: #f5f7fa; padding: 8px 24px 8px 12px; }
        QMenu::item:selected { background-color: #005b64; color: #ffffff; }
        QMenu::item:disabled { color: #9299ab; }
        QMenu::separator { height: 1px; background-color: #4b526b; margin: 5px 8px; }
        QGroupBox { border: 1px solid #272a38; border-radius: 6px; margin-top: 10px; font-weight: bold; background-color: #161822; color: #00e5ff; }
        QGroupBox::title { subcontrol-origin: margin; subcontrol-position: top left; padding: 2px 8px; background-color: #161822; }
        QLineEdit, QComboBox, QSpinBox { background-color: #1f2230; border: 1px solid #33384c; border-radius: 4px; padding: 5px; color: #ffffff; }
        QLineEdit:focus, QComboBox:focus, QSpinBox:focus { border: 1px solid #00e5ff; }
        QPushButton { background-color: #272a3a; border: 1px solid #3a3f58; border-radius: 4px; padding: 5px 10px; color: #ffffff; }
        QPushButton:hover { background-color: #35394e; border-color: #00e5ff; }
        QPushButton:disabled { background-color: #15161f; color: #555868; border-color: #1e2130; }
    )";
    setStyleSheet(style + UiTheme::scrollAndTableStyle());
}

void MainWindow::loadAndRestoreSavedDevices() {
    if (!m_deviceListWidget) return;
    if (DatabaseSettings::fromEnvironment().enabled && !m_registryLoaded) {
        if (m_registryWatcher) return;
        m_deviceListWidget->setEnabled(false);
        for (auto* button : {m_addDeviceBtn,m_removeDeviceBtn,m_startPatrolBtn})
            button->setEnabled(false);
        m_registryWatcher = new QFutureWatcher<bool>(this);
        connect(m_registryWatcher, &QFutureWatcher<bool>::finished, this, [this]() {
            const bool loaded = m_registryWatcher->result();
            m_registryWatcher->deleteLater(); m_registryWatcher = nullptr;
            m_registryLoaded = loaded;
            m_databaseStatusLabel->setText(loaded ? tr("数据库档案加载完成") : tr("数据库档案加载失败: %1").arg(DeviceConfigManager::instance().lastError()));
            if (!loaded) {
                onLogReceived(m_databaseStatusLabel->text());
                m_autoDiscoverBtn->setEnabled(true);
                return;
            }
            m_deviceListWidget->setEnabled(true);
            for (auto* button : {m_addDeviceBtn,m_removeDeviceBtn,m_autoDiscoverBtn,m_startPatrolBtn})
                button->setEnabled(true);
            loadAndRestoreSavedDevices();
        });
        m_registryWatcher->setFuture(QtConcurrent::run([]() { return DeviceConfigManager::instance().load(); }));
        return;
    }

    disconnectCameraPreview();
    m_cameraHasFrame=false;
    m_liveStreamLabel->clear();
    m_liveStreamLabel->setText(tr("请右键点击设备并选择“连接此设备”以查看实时画面。"));
    m_deviceListWidget->clear();
    m_discoveredItemsMap.clear();

    if (!DatabaseSettings::fromEnvironment().enabled) DeviceConfigManager::instance().load();
    QList<DiscoveredCameraItem> devices = DeviceConfigManager::instance().getAllDevices();

    for (auto& dev : devices) {
        // Reuse only this session's verified state, never a persisted online flag.
        bool online = false;
        const bool known = m_discoveryService->knownOnlineState(dev.cameraId, online);
        dev.isOnline = known && online;
        if (known && !online) m_offlineCameraIds.insert(dev.cameraId);
        else m_offlineCameraIds.remove(dev.cameraId);
        DeviceConfigManager::instance().markDeviceOnline(dev.cameraId, dev.isOnline);
        m_discoveredItemsMap.insert(dev.cameraId, dev);
        QListWidgetItem *item = new QListWidgetItem(m_deviceListWidget);
        item->setData(Qt::UserRole, dev.cameraId);
        updateDeviceListItemWidget(item, dev);
    }

    if (m_statusCameraCountBadge) {
        m_statusCameraCountBadge->setText(tr("在库设备: %1 台").arg(m_discoveredItemsMap.size()));
    }

    // Initialize or sync inspection matrix table with all saved devices
    if (m_inspectionTable && !m_isPatrolRunning) {
        m_inspectionTable->setRowCount(devices.size());
        m_inspectionRecords.clear();
        if (m_safeRangeAlert) m_safeRangeAlert->close();
        m_patrolCameraIdQueue.clear();

        QString snapshotDir = QCoreApplication::applicationDirPath() + "/inspection_snapshots";
        for (int i = 0; i < devices.size(); ++i) {
            const auto& dev = devices[i];
            m_patrolCameraIdQueue.append(dev.cameraId);

            InspectionRecord rec;
            rec.cameraId = dev.cameraId;
        rec.device = dev;
            rec.customName = dev.customName.isEmpty() ? dev.modelName : dev.customName;
            rec.locationInfo = dev.installationLocation + (dev.installationPosition.isEmpty() ? QString() : " | " + dev.installationPosition);
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
            for (int column = 0; column <= 8; ++column)
                m_inspectionTable->item(i, column)->setTextAlignment(Qt::AlignCenter);

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
            m_inspectionTable->setCellWidget(i, 10, createDeviceLiveButton(rec.cameraId));
        }
        if (m_inspectionSummaryLabel) {
            m_inspectionSummaryLabel->setText(tr("📊 巡检状态: 就绪 | 监控节点总数: %1 台 | 点击【启动巡检】或选择设备单张抓拍").arg(devices.size()));
        }
    }
    // Start only after the loaded archives have rows to receive health results.
    // This checks known devices immediately without running device discovery.
    m_discoveryService->startHealthMonitoring();
}

void MainWindow::updateDeviceListItemWidget(QListWidgetItem* item, const DiscoveredCameraItem& data) {
    if (!item) return;

    const bool offline = m_offlineCameraIds.contains(data.cameraId);
    const bool previewConnected = !offline && data.cameraId == m_previewCameraId && m_cameraHasFrame;
    // This lamp represents device connectivity; preview status is shown separately in the tooltip.
    const QString statusIcon = offline ? QStringLiteral(u"🔴")
        : data.isOnline ? QStringLiteral(u"🟢") : QStringLiteral(u"🟡");
    const QString connectionStatus = offline ? tr("心跳/采集监测失联") : previewConnected
        ? tr("实时画面已连接") : data.isOnline ? tr("设备在线，实时画面未连接") : tr("正在检测设备连接，请稍候");
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
                         .arg(connectionStatus)
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
    if (!AccessControl::administrator()) return;
    m_autoDiscoverBtn->setEnabled(false);
    m_discoveryService->triggerScanNow();
}

void MainWindow::onDiscoveredCameraAdded(const DiscoveredCameraItem& item) {
    // Defer full registry snapshots until calibration commits and updates the cache.
    if (m_calibrationDialogOpen) {
        QTimer::singleShot(250, this, [this, item]() { onDiscoveredCameraAdded(item); });
        return;
    }
    if (DatabaseSettings::fromEnvironment().enabled && !m_registryLoaded) {
        // Do not submit an incomplete snapshot that could replace DB archives.
        m_offlineCameraIds.remove(item.cameraId);
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
        onLogReceived(DeviceConfigManager::instance().hasPendingRegistrySave()
            ? tr("[DEVICE] 新发现设备 %1 档案尚未保存，后续扫描将重试，原因见档案保存日志。").arg(registered.cameraId)
            : DatabaseSettings::fromEnvironment().enabled
            ? tr("[DEVICE] 新发现设备 %1 已自动登记并提交数据库同步，写入状态见 数据库 状态栏。").arg(registered.cameraId)
            : tr("[DEVICE] 新发现设备 %1 已登记到本地档案；启用 数据库 后可使用数据库设备管理。").arg(registered.cameraId));
    }
    if (registered.cameraId.isEmpty()) return;
    const auto previous = m_discoveredItemsMap.value(registered.cameraId);
    const bool addressChanged = !previous.cameraId.isEmpty() && !sameDeviceConnection(previous, registered);
    m_offlineCameraIds.remove(registered.cameraId);
    m_discoveredItemsMap.insert(registered.cameraId, registered);
    // The UUID is stable across an IP change; the old endpoint's cooldown is not.
    if (addressChanged) m_lastClockCheck.remove(registered.cameraId);
    // A verified discovery response supersedes any older pending heartbeat.
    m_discoveryService->recordCaptureState(registered.cameraId, true);
    if (addressChanged) checkAndSynchronizeCameraClock(registered.cameraId);
    if (m_deviceListWidget->currentItem() &&
        m_deviceListWidget->currentItem()->data(Qt::UserRole).toString() == registered.cameraId)
        updateSelectedDeviceArchive(registered.cameraId);
    if (addressChanged) {
        m_inspectionService->updateDeviceConnection(registered);
        m_singleService->updateDeviceConnection(registered);
        if (m_singleSubmittedTask.device.cameraId == registered.cameraId &&
            m_singleSubmittedTask.inputKind == InspectionInputKind::CameraSnapshot)
            ::updateDeviceConnection(m_singleSubmittedTask.device, registered);
        if (m_analysisInput.device.cameraId == registered.cameraId &&
            m_analysisInput.inputKind == InspectionInputKind::CameraSnapshot) {
            ::updateDeviceConnection(m_analysisInput.device, registered);
            m_analysisInput.previewSnapshot.release();
        }
        if (m_previewCameraId == registered.cameraId) {
            auto* currentPage = m_visionTabWidget->currentWidget();
            const bool wasStreaming = m_isLiveStreaming;
            switchActiveCameraSource(registered.cameraId);
            if (!wasStreaming) onToggleLiveStream();
            if (m_visionTabWidget->indexOf(currentPage) >= 0) m_visionTabWidget->setCurrentWidget(currentPage);
        }
        onLogReceived(tr("[DEVICE] 设备 %1 地址由 %2:%3 更新为 %4:%5，实时预览和待执行巡检已切换新地址，进行中的旧地址取图将重新执行。")
            .arg(registered.cameraId, previous.ipOrSn).arg(previous.port).arg(registered.ipOrSn).arg(registered.port));
    }
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
    if (!AccessControl::administrator()) return;
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
        onLogReceived(tr("[DEVICE] 请在设备列表中右键连接该设备以查看实时画面。"));

        if (m_statusCameraCountBadge) {
            m_statusCameraCountBadge->setText(tr("📷 在库设备: %1 台").arg(m_discoveredItemsMap.size()));
        }
    }
}

void MainWindow::onRemoveSelectedDevice() {
    if (!AccessControl::administrator()) return;
    QListWidgetItem *curr = m_deviceListWidget->currentItem();
    if (!curr) {
        QMessageBox::information(this, tr("提示"), tr("请先在列表中选中需要删除的摄像头！"));
        return;
    }

    QString camId = curr->data(Qt::UserRole).toString();
    auto reply = QMessageBox::question(this, tr("确认删除"),
                                      tr("确定要从持久化设备库中移除摄像头 [%1] 吗？\n此操作将同时更新设备档案及本机凭据缓存。").arg(camId),
                                      QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (reply == QMessageBox::Yes) {
        DeviceConfigManager::instance().removeDevice(camId, true);
        m_discoveredItemsMap.remove(camId);
        delete curr;

        onLogReceived(tr("[DEVICE] 已从设备库中删除摄像头: %1").arg(camId));

        if (m_deviceListWidget->count() > 0) {
            m_deviceListWidget->clearSelection();
            m_deviceListWidget->setCurrentItem(nullptr);
            disconnectCameraPreview();
            m_cameraHasFrame=false;
            m_liveStreamLabel->clear();
            m_liveStreamLabel->setText(tr("请右键点击设备并选择“连接此设备”以查看实时画面。"));
        } else {
            if (m_singleService) m_singleService->stop();
            disconnectCameraPreview();
            m_hasAnalysisInput = false;
            m_currentFrame.release();
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
        menu.addAction(tr("⚡ 连接此设备"), this, [this, camId]() {
            switchActiveCameraSource(camId);
        });
        auto* disconnectAction = menu.addAction(tr("断开连接"), this, [this, camId]() {
            if (m_previewCameraId == camId) {
                disconnectCameraPreview();
                onLogReceived(tr("[STREAM] 已断开设备 %1 的实时预览连接。").arg(camId));
            }
        });
        disconnectAction->setEnabled(m_previewCameraId == camId);
        auto* recognizeAction = menu.addAction(tr("📷 拍摄并识别当前表计"), this,
            [this, camId]() { onCaptureAndRecognizeCurrent(camId); });
        recognizeAction->setEnabled(m_singleService && !m_singleService->isRunning() &&
            (!DatabaseSettings::fromEnvironment().enabled || m_registryLoaded));
        if (AccessControl::administrator()) menu.addAction(tr("🌐 网络配置..."), this, [this, item]() {
            onEditCameraInfo(item);
        });
        menu.addAction(tr("⚙️ 设备安装档案..."), this, [this, item]() { onEditCameraInfo(item, true); });
        menu.addAction(tr("🎯 表计标定..."), this, &MainWindow::onCalibrateCardinalPoints);
        if (AccessControl::administrator()) menu.addAction(tr("🗑️ 从设备库移除此设备"), this, &MainWindow::onRemoveSelectedDevice);
        menu.addSeparator();
    }

    if (AccessControl::administrator()) menu.addAction(tr("➕ 添加新网络摄像头..."), this, &MainWindow::onAddNewDevice);
    if (AccessControl::administrator()) menu.addAction(tr("🔄 刷新局域网探针设备状态"), this, &MainWindow::onTriggerAutoDiscovery);

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

void MainWindow::restoreMainWindow() {
    if (isMinimized()) showNormal();
    else show();
    raise();
    activateWindow();
}

void MainWindow::setupSystemTray() {
    if (!QSystemTrayIcon::isSystemTrayAvailable()) return;
    // Render each tray size directly so the dial stays sharp at different DPI settings.
    QIcon gaugeIcon;
    for (int size : {16, 20, 24, 32, 48, 64}) {
        QPixmap pixmap(size, size);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.scale(size / 32.0, size / 32.0);
        painter.setPen(QPen(QColor("#80deea"), 1.8));
        painter.setBrush(QColor("#162b3a"));
        painter.drawEllipse(QPointF(16, 16), 14, 14);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(QColor("#4dd0e1"), 2.8, Qt::SolidLine, Qt::RoundCap));
        painter.drawArc(QRectF(7, 7, 18, 18), 45 * 16, 90 * 16);
        painter.setPen(QPen(QColor("#ffb74d"), 2.8, Qt::SolidLine, Qt::RoundCap));
        painter.drawArc(QRectF(7, 7, 18, 18), 0, 35 * 16);
        painter.setPen(QPen(Qt::white, 2.4, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(QPointF(16, 17), QPointF(22, 10));
        painter.setPen(Qt::NoPen);
        painter.setBrush(Qt::white);
        painter.drawEllipse(QPointF(16, 17), 2.3, 2.3);
        painter.setBrush(QColor("#4dd0e1"));
        painter.drawRoundedRect(QRectF(11, 23, 10, 2), 1, 1);
        painter.end();
        gaugeIcon.addPixmap(pixmap);
    }
    m_trayIcon = new QSystemTrayIcon(gaugeIcon, this);
    m_trayIcon->setToolTip(tr("表计自动巡检：支持后台运行"));
    auto* menu = new QMenu(this);
    auto* restore = menu->addAction(tr("显示主窗口"));
    connect(restore, &QAction::triggered, this, &MainWindow::restoreMainWindow);
    auto* alarms = menu->addAction(tr("查看异常提醒"));
    connect(alarms, &QAction::triggered, this, [this]() {
        if (m_safeRangeAlert) {
            m_safeRangeAlert->showNormal();
            m_safeRangeAlert->raise();
            m_safeRangeAlert->activateWindow();
        } else restoreMainWindow();
    });
    menu->addSeparator();
    auto* quit = menu->addAction(tr("退出软件（停止巡检）"));
    connect(quit, &QAction::triggered, this, [this]() {
        m_quitting = true;
        close();
        QApplication::quit();
    });
    m_trayIcon->setContextMenu(menu);
    connect(m_trayIcon, &QSystemTrayIcon::activated, this,
        [this](QSystemTrayIcon::ActivationReason reason) {
            if (reason == QSystemTrayIcon::DoubleClick || reason == QSystemTrayIcon::Trigger)
                restoreMainWindow();
        });
    connect(m_trayIcon, &QSystemTrayIcon::messageClicked, this, [this]() {
        if (m_safeRangeAlert) {
            m_safeRangeAlert->showNormal();
            m_safeRangeAlert->raise();
            m_safeRangeAlert->activateWindow();
        } else restoreMainWindow();
    });
    m_trayIcon->show();
    QApplication::setQuitOnLastWindowClosed(false);
}

void MainWindow::closeEvent(QCloseEvent *event) {
    if (!m_quitting) {
        const bool backgroundAvailable = m_trayIcon && m_trayIcon->isVisible() &&
            QSystemTrayIcon::isSystemTrayAvailable();
        QMessageBox confirmation(QMessageBox::Question, tr("关闭软件"),
            tr("请选择退出软件或转入后台运行。"), QMessageBox::NoButton, this);
        confirmation.setStyleSheet(R"(
            QMessageBox { background-color: #161822; color: #ffffff; }
            QMessageBox QLabel { background-color: transparent; color: #ffffff; font-size: 14px; }
            QMessageBox QPushButton {
                background-color: #37474f; color: #ffffff;
                border: 1px solid #78909c; border-radius: 4px;
                padding: 7px 18px; min-width: 76px;
            }
            QMessageBox QPushButton:hover { background-color: #455a64; }
            QMessageBox QPushButton:pressed { background-color: #263238; }
            QMessageBox QPushButton:default, QMessageBox QPushButton:focus {
                border: 2px solid #80deea;
            }
            QMessageBox QPushButton:disabled {
                background-color: #252936; color: #aeb8c4; border-color: #50586b;
            }
        )");
        confirmation.setInformativeText(backgroundAvailable
            ? tr("后台运行将继续已启用的自动巡检，异常读数仍会弹窗提醒；退出软件将停止巡检。")
            : tr("当前系统托盘不可用，无法转入后台运行。退出软件将停止巡检。"));
        auto* background = confirmation.addButton(tr("后台运行"), QMessageBox::AcceptRole);
        auto* quit = confirmation.addButton(tr("退出软件"), QMessageBox::DestructiveRole);
        auto* cancel = confirmation.addButton(tr("取消"), QMessageBox::RejectRole);
        background->setEnabled(backgroundAvailable);
        confirmation.setDefaultButton(backgroundAvailable ? background : cancel);
        confirmation.setEscapeButton(cancel);
        confirmation.exec();
        if (confirmation.clickedButton() == quit) {
            m_quitting = true;
        } else if (confirmation.clickedButton() != background || !backgroundAvailable) {
            event->ignore();
            return;
        }
    }
    if (!m_quitting) {
        event->ignore();
        hide();
        if (!m_backgroundHintShown) {
            m_backgroundHintShown = true;
            m_trayIcon->showMessage(tr("已转入后台运行"),
                tr("自动巡检继续运行，异常读数会弹窗提醒。右键托盘图标可退出软件。"),
                QSystemTrayIcon::Information);
        }
        return;
    }
    if (m_databaseSyncTimer) m_databaseSyncTimer->stop();
    if (m_liveTimer) m_liveTimer->stop();
    if (m_autoPatrolTimer) m_autoPatrolTimer->stop();
    if (m_clockCheckTimer) m_clockCheckTimer->stop();
    if (m_inspectionService) m_inspectionService->stop();
    if (m_singleService) m_singleService->stop();
    if (m_previewService) m_previewService->stop();
    if (m_discoveryService) m_discoveryService->stopHealthMonitoring();
    QApplication::setQuitOnLastWindowClosed(true);
    event->accept();
    QApplication::quit();
}

void MainWindow::onCameraSelectionChanged() {
    QListWidgetItem *curr = m_deviceListWidget->currentItem();
    if (!curr) {
        disconnectCameraPreview();
        m_selectedCameraId.clear();
        m_profileReady = false;
        updateSelectedDeviceArchive(QString());
        return;
    }
    selectCameraForDetails(curr->data(Qt::UserRole).toString());
}

void MainWindow::selectCameraForDetails(const QString& camId) {
    if (!m_discoveredItemsMap.contains(camId)) return;
    if (m_selectedCameraId == camId) { updateSelectedDeviceArchive(camId); return; }
    disconnectCameraPreview();
    m_selectedCameraId = camId;
    m_calibrationChecked.remove(camId);
    if (m_singleService && m_singleService->isRunning()) m_singleService->stop();
    m_hasAnalysisInput = false;
    m_latestResult = GaugeResult();
    m_valLabel->setText(tr("读取数值: --"));
    m_statusLabel->setText(tr("已选择设备档案；实时预览请通过右键菜单连接。"));
    updateImageCard(m_originalImage, cv::Mat(), "Original");
    updateImageCard(m_recognitionImage, cv::Mat(), "Recognition");
    m_currentFrame.release();
    m_currentImagePath.clear();
    applyCameraCalibrationAndProfile(camId);
}

void MainWindow::disconnectCameraPreview() {
    const auto previousCameraId = m_previewCameraId;
    m_previewCameraId.clear();
    ++m_cameraConnectionGeneration;
    if (m_previewService) m_previewService->select(InspectionTask());
    if (m_liveTimer) m_liveTimer->stop();
    m_isLiveStreaming = false;
    m_cameraHasFrame = false;
    if (auto* row = findListItemByCameraId(previousCameraId))
        updateDeviceListItemWidget(row, m_discoveredItemsMap.value(previousCameraId));
    m_calibrationSnapshot = FrameCaptureResult();
    m_calibrationSnapshotCamera.clear();
    m_calibrationSnapshotLoading = false;
    m_calibrationQueries.clear();
    m_pendingCalibrationCamera.clear();
    m_toggleLiveBtn->setEnabled(false);
    m_toggleLiveBtn->setText(tr("实时预览（未连接）"));
    m_liveCamInfoBadge->setText(tr("摄像头连接：未连接"));
    m_liveStreamLabel->clear();
    m_liveStreamLabel->setText(tr("请右键点击设备并选择“连接此设备”以查看实时画面。"));
    if (m_visionTabWidget && m_livePage) {
        const int liveIndex = m_visionTabWidget->indexOf(m_livePage);
        if (liveIndex >= 0) {
            m_visionTabWidget->setCurrentWidget(m_inspectionPage);
            m_visionTabWidget->removeTab(liveIndex);
            m_livePage->hide();
        }
    }
}

void MainWindow::switchActiveCameraSource(const QString& camId) {
    if (!m_discoveredItemsMap.contains(camId)) return;
    selectCameraForDetails(camId);
    disconnectCameraPreview();
    m_previewCameraId = camId;
    showVisionPage(m_livePage, tr("📷 摄像头实时预览"));
    m_isLiveStreaming = true;
    m_toggleLiveBtn->setEnabled(true);
    m_toggleLiveBtn->setText(tr("⏸ 暂停实时预览"));
    if (m_liveTimer) m_liveTimer->start(40);
    m_liveStreamLabel->setText(tr("正在连接所选设备..."));
    m_liveCamInfoBadge->setText(tr("正在连接设备：%1").arg(camId));
    m_liveCamInfoBadge->setToolTip(QString());
    InspectionTask task;
    task.device = m_discoveredItemsMap.value(camId);
    m_discoveryService->deferHealthForCapture(camId);
    checkAndSynchronizeCameraClock(camId);
    m_previewService->select(task);
    applyCameraCalibrationAndProfile(camId);
}

void MainWindow::checkAndSynchronizeCameraClock(const QString& cameraId) {
    if (!m_discoveredItemsMap.contains(cameraId)) return;
    const auto device = m_discoveredItemsMap.value(cameraId);
    if (device.protocolType != CameraProtocolType::ONVIF_RTSP_STREAM || m_clockChecksInFlight.contains(cameraId)) return;
    const auto now = QDateTime::currentDateTimeUtc();
    if (m_lastClockCheck.contains(cameraId) && m_lastClockCheck[cameraId].secsTo(now) < 60) return;
    m_clockChecksInFlight.insert(cameraId);
    m_lastClockCheck[cameraId] = now;
    auto* watcher = new QFutureWatcher<OnvifSoap::ClockSyncResult>(this);
    connect(watcher, &QFutureWatcher<OnvifSoap::ClockSyncResult>::finished, this, [this, watcher, cameraId, device]() {
        const auto result = watcher->result(); watcher->deleteLater();
        m_clockChecksInFlight.remove(cameraId);
        if (!m_discoveredItemsMap.contains(cameraId)) return;
        const auto current = m_discoveredItemsMap.value(cameraId);
        if (current.ipOrSn != device.ipOrSn || current.port != device.port ||
            current.protocolType != device.protocolType) {
            // Discard the old endpoint's late result and check the current address.
            m_lastClockCheck.remove(cameraId);
            checkAndSynchronizeCameraClock(cameraId);
            return;
        }
        const auto identity = tr("%1（%2:%3）").arg(cameraId, device.ipOrSn).arg(device.port);
        const auto message = result.logMessage(identity);
        if (!message.isEmpty()) onLogReceived(tr("[ONVIF TIME] %1").arg(message));
        if (m_previewCameraId == cameraId) m_liveCamInfoBadge->setToolTip(message);
    });
    watcher->setFuture(QtConcurrent::run([device]() {
        QUrl endpoint; endpoint.setScheme("http"); endpoint.setHost(device.ipOrSn);
        endpoint.setPort(device.port); endpoint.setPath("/onvif/device_service");
        return OnvifSoap::synchronizeClockDetailed(endpoint,device.username,device.password,true);
    }));
}

void MainWindow::updateSelectedDeviceArchive(const QString& camId) {
    if (!m_discoveredItemsMap.contains(camId)) {
        for (auto* label : {m_cfgGaugeId, m_cfgHardwareId, m_cfgNetworkInfo, m_cfgLocationInfo,
                           m_cfgGaugeSpec, m_cfgRange, m_cfgSafeRange, m_cfgCalibration})
            label->setText(tr("未选择设备"));
        return;
    }
    const auto summary = DeviceArchiveSummary::fromDevice(m_discoveredItemsMap.value(camId));
    m_cfgGaugeId->setText(summary.identity);
    m_cfgHardwareId->setText(summary.hardware);
    m_cfgNetworkInfo->setText(summary.network);
    m_cfgLocationInfo->setText(summary.location);
    m_cfgGaugeSpec->setText(summary.specification);
    m_cfgSafeRange->setText(summary.safety +
        (m_profileReady && QString::fromStdString(m_profile.gauge_id) == camId
            ? tr("\n读数单位：%1").arg(QString::fromStdString(m_profile.unit)) : QString()));
    m_cfgCalibration->setText(summary.calibration);
}

void MainWindow::applyCameraCalibrationAndProfile(const QString& camId) {
    if (!m_discoveredItemsMap.contains(camId)) return;
    updateSelectedDeviceArchive(camId);
    const auto generation = ++m_profileGeneration;
    const auto profileId = m_discoveredItemsMap[camId].defaultProfileId;
    if (profileId.isEmpty()) {
        m_profileReady = false;
        m_cfgRange->setText(tr("量程范围：请先在表计档案中选择模板"));
        return;
    }
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
        updateSelectedDeviceArchive(camId);
        tryOpenCameraCalibration();
        m_cfgRange->setText(tr("量程范围：%1 ～ %2 %3").arg(m_profile.min_val).arg(m_profile.max_val).arg(QString::fromStdString(m_profile.unit)));
    });
    watcher->setFuture(QtConcurrent::run([profileId]() -> ProfileReply {
        try { return {MySqlRepository().loadTemplate(profileId).definition, QString()}; }
        catch (const std::exception& error) { return {QJsonObject(), QString::fromUtf8(error.what())}; }
    }));
}
void MainWindow::onToggleLiveStream() {
    if (m_previewCameraId.isEmpty()) {
        m_statusLabel->setText(tr("请在设备右键菜单中连接摄像头。"));
        return;
    }
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

void MainWindow::onTabChanged(int index) {
    if (m_visionTabWidget->widget(index) == m_livePage) {
        onLogReceived(tr("[STREAM] 画面切回实时流监控页，恢复画面实时重绘。"));
        QTimer::singleShot(30, this, &MainWindow::onUpdateLiveStreamFrame);
    } else {
        onLogReceived(tr("[STREAM] 切换至算法调试/识别结果页，已暂停 GUI 界面重绘（底层视频流保持实时接收）以降低 CPU/GPU 损耗。"));
    }
}

void MainWindow::onUpdateLiveStreamFrame() {
    if (m_isLiveStreaming && m_previewService) m_previewService->requestFrame();
}

void MainWindow::onLogReceived(const QString& logMsg) {
    if (!m_logConsole) return;
    QString timeStr = QDateTime::currentDateTime().toString("HH:mm:ss.zzz");
    m_logConsole->append(QString("[%1] %2").arg(timeStr).arg(logMsg));
}

void MainWindow::onSelectImage() {
    if (!AccessControl::administrator()) return;
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
    if (!AccessControl::administrator()) return;
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
    if (!m_discoveredItemsMap.contains(cameraId) || m_calibrationDialogOpen) return;
    if (m_discoveredItemsMap.value(cameraId).defaultProfileId.trimmed().isEmpty()) {
        QMessageBox::warning(this, tr("请先选择表计模板"), tr("请右键设备打开“设备安装档案”，主动选择表计模板并保存后再标定。"));
        return;
    }
    // Calibration owns an ONVIF snapshot request; never select a live stream.
    ++m_cameraConnectionGeneration;
    m_calibrationSnapshot=FrameCaptureResult();
    m_calibrationSnapshotCamera.clear();
    m_calibrationSnapshotLoading=false;
    m_calibrationQueries.clear();
    m_calibrationChecked.remove(cameraId);
    m_pendingCalibrationCamera = cameraId;
    applyCameraCalibrationAndProfile(cameraId);
    m_discoveryService->deferHealthForCapture(cameraId);
    m_statusLabel->setText(tr("正在通过 ONVIF 获取设备 %1 的标定快照…").arg(cameraId));
    requestCalibrationSnapshot(cameraId);
    checkCameraCalibration(cameraId);
    tryOpenCameraCalibration();
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

void MainWindow::requestCalibrationSnapshot(const QString& cameraId) {
    if (m_calibrationSnapshotLoading || m_calibrationDialogOpen || cameraId!=m_pendingCalibrationCamera) return;
    auto task=makeSingleTask();
    if (task.device.protocolType!=CameraProtocolType::ONVIF_RTSP_STREAM) {
        m_pendingCalibrationCamera.clear();
        QMessageBox::warning(this,tr("无法获取标定底图"),tr("请为设备配置 ONVIF 协议后获取标定快照。")); return;
    }
    task.captureOnly=true; task.preferOnvifSnapshot=true; task.requireOnvifSnapshot=true; task.previewSnapshot.release();
    task.captureTimeoutMs=12000;
    const auto generation=m_cameraConnectionGeneration;
    m_calibrationSnapshotLoading=true;
    auto* watcher=new QFutureWatcher<FrameCaptureResult>(this);
    connect(watcher,&QFutureWatcher<FrameCaptureResult>::finished,this,[this,watcher,cameraId,generation]{
        auto frame=watcher->result(); watcher->deleteLater();
        if (generation!=m_cameraConnectionGeneration || cameraId!=m_pendingCalibrationCamera) return;
        m_calibrationSnapshotLoading=false;
        if (!frame.ok() || frame.message!="ONVIF_HTTP" || isUniformSnapshotFrame(frame.frame)) {
            m_pendingCalibrationCamera.clear();
            const auto message=tr("设备 %1 的 ONVIF 快照获取失败或返回灰色/空白图像，请检查 ONVIF 地址、账号和快照服务后重试。")
                .arg(cameraId);
            m_statusLabel->setText(message); onLogReceived(message);
            QMessageBox::warning(this,tr("无法获取标定底图"),message); return;
        }
        m_calibrationSnapshot=frame; m_calibrationSnapshot.frame=frame.frame.clone();
        m_calibrationSnapshotCamera=cameraId;
        onLogReceived(tr("[CALIBRATION] 已获取设备 %1 的 ONVIF 快照用于表计标定。").arg(cameraId));
        tryOpenCameraCalibration();
    });
    watcher->setFuture(QtConcurrent::run([task]{
        return isolatedCapture(task,std::make_shared<std::atomic<bool>>(false));
    }));
}

void MainWindow::tryOpenCameraCalibration() {
    const QString cameraId = m_pendingCalibrationCamera;
    if (cameraId.isEmpty() || cameraId != makeSingleTask().device.cameraId || !m_profileReady || m_calibrationDialogOpen || m_historicalCalibrationPending ||
        !m_calibrationChecked.contains(cameraId) || m_calibrationQueries.contains(cameraId)) return;
    if (m_calibrationSnapshotCamera!=cameraId || !m_calibrationSnapshot.ok()) {
        requestCalibrationSnapshot(cameraId); return;
    }
    const auto frame=m_calibrationSnapshot;
    m_calibrationSnapshot=FrameCaptureResult();
    m_calibrationSnapshotCamera.clear();
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

            if (cameraId == makeSingleTask().device.cameraId) applyCameraCalibrationAndProfile(cameraId);
            onLogReceived(tr("[CALIBRATION] 设备 %1 的表计标定已保存到 数据库。").arg(cameraId));
        }
        m_calibrationDialogOpen = false;
    });
}

void MainWindow::onEditCameraInfo(QListWidgetItem *item, bool archiveOnly) {
    if (!AccessControl::administrator()) archiveOnly = true;
    if (!item) return;

    QString camId = item->data(Qt::UserRole).toString();
    if (camId.isEmpty()) return;

    CameraInfoEditDialog dialog(camId, this, false, {}, archiveOnly);
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

void MainWindow::submitSingleTask(InspectionTask task, bool historicalCalibrated) {
    if (m_singleService->isRunning() || m_historicalCalibrationPending) return;
    const bool replayDatabaseSnapshot = usesHistoricalDatabaseConfiguration(task);
    if (task.inputKind == InspectionInputKind::HistoricalImage && !task.captureOnly && !historicalCalibrated && !replayDatabaseSnapshot) {
        struct Reply { InspectionTask task; cv::Mat frame; QString error; };
        m_historicalCalibrationPending = true;
        m_statusLabel->setText(tr("正在读取历史原图，分析前请完成手动标定…"));
        auto* watcher = new QFutureWatcher<Reply>(this);
        connect(watcher, &QFutureWatcher<Reply>::finished, this, [this, watcher]() {
            auto reply = watcher->result(); watcher->deleteLater();
            if (!reply.error.isEmpty()) {
                m_historicalCalibrationPending = false;
                m_statusLabel->setText(reply.error); onLogReceived("[HISTORY] " + reply.error); return;
            }
            GaugeProfile profile;
            if (!profile.loadFromJson(reply.task.historicalProfile, reply.task.device.defaultProfileId.toStdString())) {
                m_historicalCalibrationPending = false;
                m_statusLabel->setText(tr("历史算法模板无法加载，未启动分析。")); return;
            }
            // Start from the actual historical image; current camera geometry is irrelevant.
            profile.perspective_pts.clear(); profile.center = {-1,-1}; profile.scalePoints = {};
            profile.perspective_correction_enabled = false;
            CardinalSelectorDialog dialog(reply.frame, profile, reply.task.device.cameraId, this, true);
            const bool accepted = dialog.exec() == QDialog::Accepted;
            m_historicalCalibrationPending = false;
            if (!accepted) {
                m_statusLabel->setText(tr("已取消历史原图标定，未启动识别。"));
                onLogReceived(tr("[HISTORY] 已取消历史原图标定，未启动识别。")); return;
            }
            QJsonObject calibration{{"calibration_confirmed",true}, {"perspective_correction_enabled",true},
                {"calibrated_width",reply.frame.cols}, {"calibrated_height",reply.frame.rows},
                {"center_x",profile.center.x}, {"center_y",profile.center.y},
                {"pointer_base_radius",profile.pointerBaseRadius}, {"scale_points",profile.scalePoints}};
            for (int i=0; i<4; ++i) {
                calibration[QString("pt_%1_x").arg(i)] = profile.perspective_pts[i].x;
                calibration[QString("pt_%1_y").arg(i)] = profile.perspective_pts[i].y;
            }
            reply.task.historicalCalibration = calibration;
            auto device = deviceToJson(reply.task.device, true); device["calibration"] = calibration;
            reply.task.device = deviceFromJson(device);
            onLogReceived(tr("[HISTORY] 历史原图手动标定已确认，仅用于本次分析。"));
            submitSingleTask(reply.task, true);
        });
        watcher->setFuture(QtConcurrent::run([task]() mutable -> Reply {
            Reply reply; reply.task = task;
            try {
                if (!task.databaseOriginalTaskId.isEmpty()) {
                    const auto stored = MySqlRepository().original(task.databaseOriginalTaskId);
                    if (stored.inspection.cameraId != task.device.cameraId || stored.inspection.capturedAt != task.originalCapturedAt)
                        throw std::runtime_error("HISTORICAL_IDENTITY_MISMATCH");
                    reply.task.device = stored.inspection.device; reply.task.historicalProfile = stored.inspection.profile;
                    std::vector<uchar> bytes(stored.bytes.begin(),stored.bytes.end());
                    reply.frame = cv::imdecode(bytes,cv::IMREAD_COLOR);
                    if (reply.frame.cols != stored.width || reply.frame.rows != stored.height)
                        throw std::runtime_error("ORIGINAL_DIMENSIONS_INVALID");
                } else {
                    reply.frame = cv::imread(task.localImagePath.toStdString());
                    QFile file(QFileInfo(task.localImagePath).dir().filePath("result.json"));
                    if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("HISTORICAL_METADATA_MISSING");
                    const auto record = QJsonDocument::fromJson(file.readAll()).object();
                    if (record["task_id"].toString()!=task.parentTaskId || record["camera_id"].toString()!=task.device.cameraId ||
                        QDateTime::fromString(record["captured_at"].toString(),Qt::ISODateWithMs)!=task.originalCapturedAt ||
                        QFileInfo(record["original_path"].toString()).canonicalFilePath()!=QFileInfo(task.localImagePath).canonicalFilePath())
                        throw std::runtime_error("HISTORICAL_IDENTITY_MISMATCH");
                    reply.task.historicalProfile = record["profile_snapshot"].toObject();
                    if (!record["device"].toObject().isEmpty()) {
                        const auto archivedDevice = deviceFromJson(record["device"].toObject());
                        if (archivedDevice.cameraId != task.device.cameraId)
                            throw std::runtime_error("HISTORICAL_IDENTITY_MISMATCH");
                        reply.task.device = archivedDevice;
                    }
                }
                if (reply.frame.empty()) throw std::runtime_error("ORIGINAL_DECODE_FAILED");
                reply.task.useHistoricalConfiguration = task.useHistoricalConfiguration;
            } catch (const std::exception& e) { reply.error = QString::fromUtf8(e.what()); }
            return reply;
        }));
        return;
    }
    if (task.inputKind != InspectionInputKind::HistoricalImage || !task.useHistoricalConfiguration) {
        if (m_discoveredItemsMap.contains(task.device.cameraId))
            task.device = m_discoveredItemsMap.value(task.device.cameraId);
    }
    if (!replayDatabaseSnapshot && !task.captureOnly && !recognitionMissingRequirements(task.device).isEmpty()) {
        const auto message = tr("设备 %1 暂不能识别：\n%2\n请右键设备打开“设备安装档案”或“表计标定”完成配置。").arg(task.device.cameraId, recognitionMissingRequirements(task.device).join("\n"));
        m_statusLabel->setText(tr("请先完善设备 %1 的表计档案与标定。").arg(task.device.cameraId));
        onLogReceived(tr("[CALIBRATION] %1").arg(message));
        QMessageBox::warning(this, tr("设备识别条件未满足"), message);
        return;
    }
    if (replayDatabaseSnapshot)
        onLogReceived(tr("[HISTORY] 使用历史任务 %1 保存的标定和算法配置直接重分析。").arg(task.databaseOriginalTaskId));
    task.outputDirectory = QCoreApplication::applicationDirPath() + "/inspection_snapshots";
    task.runMode = GaugeRunMode::Evidence;
    task.returnImages = true;
    m_singleSubmittedTask = task;
    m_startTime = std::chrono::high_resolution_clock::now();
    if (!m_singleService->start({task})) return;
    if (!task.captureOnly) showVisionPage(m_recognitionPage, tr("⚡ 仪表读数识别"));
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

void MainWindow::logRecognitionStageTimings(const QString& context, const QJsonObject& engineSteps) {
    QStringList details;
    for (auto it = engineSteps.begin(); it != engineSteps.end(); ++it)
        if (it.value().isDouble()) details << it.key() + "=" + QString::number(it.value().toDouble(), 'f', 1);
    onLogReceived(tr("[ENGINE TIMING] %1 · 耗时(ms)：%2").arg(context, details.join("; ")));
    const auto pointerSteps = engineSteps["pointer_details"].toObject();
    if (!pointerSteps.isEmpty()) {
        QStringList pointerDetails;
        const std::pair<const char*, const char*> stages[] = {
            {"gradient_preprocessing_ms", "梯度预处理"}, {"coarse_search_ms", "方向粗搜索"},
            {"candidate_diagnostics_ms", "候选整理"}, {"fine_search_ms", "方向精搜索"},
            {"direction_scoring_ms", "最终方向判定"}, {"debug_rendering_ms", "调试绘图"}};
        for (const auto& stage : stages)
            if (pointerSteps.contains(stage.first))
                pointerDetails << QString::fromUtf8(stage.second) + " " + QString::number(pointerSteps[stage.first].toDouble(), 'f', 1) + " ms";
        onLogReceived(tr("[POINTER TIMING] 设备 %1 · %2").arg(context, pointerDetails.join("；")));
        for (const auto& prefix : {QStringLiteral("coarse"), QStringLiteral("fine")}) {
            if (!pointerSteps.contains(prefix + "_candidate_count")) continue;
            onLogReceived(tr("[POINTER DETAIL] 设备 %1 · %2 · 候选轴 %3 个 · 边缘采样与区域验证 %4 ms；截面构建 %5 ms；针身评分 %6 ms；针尾检测与评分 %7 ms（各线程累计，包含于搜索耗时，不与实际耗时相加）")
                .arg(context, prefix == "coarse" ? tr("粗搜索") : tr("精搜索"))
                .arg(pointerSteps[prefix+"_candidate_count"].toInt())
                .arg(pointerSteps[prefix+"_region_worker_ms"].toDouble(),0,'f',1)
                .arg(pointerSteps[prefix+"_sections_worker_ms"].toDouble(),0,'f',1)
                .arg(pointerSteps[prefix+"_scoring_worker_ms"].toDouble(),0,'f',1)
                .arg(pointerSteps[prefix+"_tail_worker_ms"].toDouble(),0,'f',1));
        }
    }
}

void MainWindow::onRecognitionFinished() {
    auto tEnd = std::chrono::high_resolution_clock::now();
    double elapsedMs = std::chrono::duration<double, std::milli>(tEnd - m_startTime).count();


    m_confidenceLabel->setToolTip(qualitySummary(m_latestResult.recognitionQuality) + "\n" + QString::fromUtf8(QJsonDocument(m_latestResult.recognitionQuality).toJson()));
    if (auto label = findChild<QLabel*>("recognitionQuality")) {
        label->setText(qualitySummary(m_latestResult.recognitionQuality));
        label->setToolTip(qualityDetails(m_latestResult.recognitionQuality));
    }
    onLogReceived(qualitySummary(m_latestResult.recognitionQuality));
    logRecognitionStageTimings(tr("指定图像/历史重分析"), m_latestResult.stageTimings);
    // Show only the original capture and final recognition result.
    updateImageCard(m_originalImage, m_latestResult.step1_original, tr("采集原图"));
    updateImageCard(m_recognitionImage, m_latestResult.step4_recognition, tr("最终识别结果"));

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
        m_valLabel->setText(tr("<b>读取数值:</b> <font color='#00e676' size='+2'>%1 %2</font>")
                                .arg(m_latestResult.value, 0, 'f', 3).arg(QString::fromStdString(m_latestResult.unit)));
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

        onLogReceived(tr("[PIPELINE] 指定图像识别完成: 数值=%1 %2, 耗时=%3 ms。")
                          .arg(m_latestResult.value, 0, 'f', 3).arg(QString::fromStdString(m_latestResult.unit)).arg(elapsedMs, 0, 'f', 2));
    } else {
        QString errStep = QString::fromStdString(m_latestResult.errorStep);
        QString errMsg = QString::fromStdString(m_latestResult.errorMessage);
        if (errMsg.isEmpty()) errMsg = tr("未知识别错误 (置信度不足或未找到指针)");

        m_statusLabel->setText(QString(tr("<font color='#ff5252'>❌ 识别失败 [%1]: %2</font>")).arg(errStep.isEmpty() ? tr("检测阶段") : errStep).arg(errMsg));
        m_valLabel->setText(tr("<b>读取数值:</b> <font color='#ff5252'>-- (识别失败)</font>"));
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
    if (!AccessControl::administrator()) return;
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
    QWidget *ctrlBox = new QWidget(container);
    QHBoxLayout *ctrlLayout = new QHBoxLayout(ctrlBox);
    ctrlLayout->setContentsMargins(10, 8, 10, 8);

    m_startPatrolBtn = new QPushButton(tr("启动巡检"), ctrlBox);
    m_startPatrolBtn->setStyleSheet("QPushButton { background-color: #00897b; color: white; font-weight: bold; font-size: 13px; padding: 7px 16px; border-radius: 4px; }"
                                   "QPushButton:hover { background-color: #00bfa5; }");

    m_autoPatrolCheck = new QCheckBox(tr("定时轮巡"), ctrlBox);
    m_autoPatrolInterval = new QSpinBox(ctrlBox);
    m_autoPatrolInterval->setObjectName("autoPatrolIntervalMinutes");
    m_autoPatrolInterval->setRange(1, 1440);
    m_autoPatrolInterval->setSuffix(tr("分钟"));
    m_autoPatrolInterval->setKeyboardTracking(false);
    QSettings preferences("XCsLab", "GaugeReader");
    m_autoPatrolInterval->setValue(preferences.value("patrol/intervalMinutes", 10).toInt());
    m_autoPatrolInterval->setToolTip(tr("巡检间隔：1～1440 分钟。启用或修改后重新计时；上次巡检尚未完成时跳过本次触发。"));
    connect(m_autoPatrolInterval, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int minutes) {
        QSettings preferences("XCsLab", "GaugeReader");
        preferences.setValue("patrol/intervalMinutes", minutes);
        if (m_autoPatrolCheck->isChecked() && m_autoPatrolTimer) {
            m_autoPatrolTimer->start(minutes * 60 * 1000);
            onLogReceived(tr("[PATROL] 定时轮巡间隔已更新为 %1 分钟，重新计时。").arg(minutes));
        }
    });
    m_autoPatrolCheck->setStyleSheet("QCheckBox { color: #80deea; font-size: 12px; font-weight: bold; }");
    connect(m_autoPatrolCheck, &QCheckBox::toggled, this, [this](bool checked) {
        if (checked) {
            const int minutes = m_autoPatrolInterval->value();
            m_autoPatrolTimer->start(minutes * 60 * 1000);
            onLogReceived(tr("[PATROL] ⏱ 已启动定时轮巡，间隔 %1分钟。").arg(minutes));
        } else {
            m_autoPatrolTimer->stop();
            onLogReceived(tr("[PATROL] ⏱ 已关闭定时自动巡视轮询。"));
        }
    });

    m_exportReportBtn = new QPushButton(tr("📥 导出巡检数据报表 (CSV)"), ctrlBox);
    m_exportReportBtn->setStyleSheet("QPushButton { background-color: #37474f; color: #00e5ff; font-weight: bold; border: 1px solid #00acc1; padding: 7px 14px; border-radius: 4px; }"
                                    "QPushButton:hover { background-color: #455a64; color: #ffffff; }");

    ctrlLayout->addWidget(m_startPatrolBtn);
    ctrlLayout->addWidget(m_autoPatrolCheck);
    ctrlLayout->addWidget(m_autoPatrolInterval);
    ctrlLayout->addStretch();
    ctrlLayout->addWidget(m_exportReportBtn);

    layout->addWidget(ctrlBox);

    // 2. Summary Badge & Progress Bar
    QHBoxLayout *statusLayout = new QHBoxLayout();
    m_inspectionSummaryLabel = new QLabel(tr("📊 巡检状态: 待开始 | 监控节点总数: 0 台 | 正常: 0 | 异常: 0"), container);
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
    m_inspectionTable->verticalHeader()->setVisible(false);
    m_inspectionTable->setColumnCount(11);
    QStringList headers = {
        tr("序号"), tr("设备编号"), tr("测点名称"), tr("安装区域/位置"),
        tr("最新读数"), tr("指针偏角"), tr("置信度"), tr("巡检状态"), tr("更新时间"),
        tr("巡检图片"), tr("实时监控")
    };
    m_inspectionTable->setColumnHidden(2, true); // Point name remains in the device archive, not the inspection list.
    m_inspectionTable->setColumnHidden(5, true); // Keep internal column indices stable; angle is not user-facing.
    m_inspectionTable->setHorizontalHeaderLabels(headers);
    m_inspectionTable->horizontalHeader()->setDefaultAlignment(Qt::AlignCenter);
    auto* inspectionHeader = m_inspectionTable->horizontalHeader();
    inspectionHeader->setSectionResizeMode(QHeaderView::Fixed);
    const auto resizingColumns = std::make_shared<bool>(false);
    const auto resizeColumns = [this, resizingColumns]() {
        if (*resizingColumns) return;
        *resizingColumns = true;
        // Location keeps the largest share; remaining fields receive usable minimum widths.
        const int columns[] = {0, 1, 3, 4, 6, 7, 8, 9, 10};
        const int weights[] = {3, 14, 24, 10, 7, 14, 13, 7, 8};
        const int minimums[] = {44, 140, 230, 95, 75, 140, 150, 105, 110};
        const int width = m_inspectionTable->viewport()->width();
        for (int i = 0; i < 9; ++i)
            m_inspectionTable->setColumnWidth(columns[i], qMax(minimums[i], width * weights[i] / 100));
        *resizingColumns = false;
    };
    connect(inspectionHeader, &QHeaderView::geometriesChanged, this, resizeColumns);
    resizeColumns();
    m_inspectionTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_inspectionTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_inspectionTable->setAlternatingRowColors(true);
    UiTheme::applyTable(m_inspectionTable);

    layout->addWidget(m_inspectionTable, 1);

    // Connections
    connect(m_startPatrolBtn, &QPushButton::clicked, this, [this]() {
        if (m_isPatrolRunning) onStopAllDevicesInspection();
        else onStartAllDevicesInspection();
    });
    connect(m_exportReportBtn, &QPushButton::clicked, this, &MainWindow::onExportPatrolReport);
    connect(m_inspectionTable, &QTableWidget::cellDoubleClicked, this, &MainWindow::onInspectionTableItemDoubleClicked);

    return container;
}

void MainWindow::onCaptureAndRecognizeCurrent(const QString& cameraId) {
    if (!m_discoveredItemsMap.contains(cameraId)) return;
    InspectionTask task;
    task.device = m_discoveredItemsMap.value(cameraId);
    task.inputKind = InspectionInputKind::CameraSnapshot;
    task.preferOnvifSnapshot=true; task.requireOnvifSnapshot=true;
    task.captureTimeoutMs=12000;
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
        if (!recognitionMissingRequirements(*it).isEmpty()) {
            uncalibrated.append(it->cameraId + "：" + recognitionMissingRequirements(*it).join("；"));
            it = devices.erase(it);
        } else ++it;
    }
    if (!uncalibrated.isEmpty()) {
        const auto message = tr("以下设备识别条件未满足，已排除在本次全站批量巡检之外：\n%1\n请右键对应设备，完善“设备安装档案”、选择模板并完成“表计标定”。").arg(uncalibrated.join("\n"));
        onLogReceived(tr("[CALIBRATION] %1").arg(message));
        m_inspectionSummaryLabel->setText(tr("配置不完整设备已跳过：%1 台；请完善表计档案与标定。").arg(uncalibrated.size()));
        if (sender() != m_autoPatrolTimer)
            QMessageBox::warning(this, tr("设备识别条件未满足"), message);
        if (devices.isEmpty()) return;
    }

    m_isPatrolRunning = true;
    if (m_safeRangeAlert) m_safeRangeAlert->close();
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
        rec.locationInfo = dev.installationLocation + (dev.installationPosition.isEmpty() ? QString() : " | " + dev.installationPosition);
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
        for (int column = 0; column <= 8; ++column)
            m_inspectionTable->item(i, column)->setTextAlignment(Qt::AlignCenter);

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
        m_inspectionTable->setCellWidget(i, 10, createDeviceLiveButton(rec.cameraId));
    }

    m_patrolCurrentIndex = 0;
    updatePatrolControls();
    m_inspectionProgressBar->setValue(0);
    m_inspectionSummaryLabel->setText(tr("📊 正在全站巡检 (0/%1)...").arg(devices.size()));

    // Switch vision tab to inspection matrix
    m_visionTabWidget->setCurrentWidget(m_inspectionPage);

    onLogReceived(tr("[PATROL] 启动巡检流水线，待巡检设备总计: %1 台。").arg(devices.size()));

    QList<InspectionTask> tasks;
    for (const auto& device : devices) {
        InspectionTask task;
        task.device = device;
        task.preferOnvifSnapshot = true;
        if (device.protocolType == CameraProtocolType::LOCAL_FILE) task.inputKind = InspectionInputKind::LocalImage;
        task.runMode = GaugeRunMode::Evidence;
        task.localImagePath = m_hasAnalysisInput && m_analysisInput.inputKind == InspectionInputKind::LocalImage
            ? m_analysisInput.localImagePath : QString();
        task.outputDirectory = QCoreApplication::applicationDirPath() + "/inspection_snapshots";
        tasks.append(task);
    }
    if (!m_inspectionService->start(tasks)) {
        m_isPatrolRunning = false;
        updatePatrolControls();
        m_inspectionSummaryLabel->setText(tr("巡检未启动，请稍后重试。"));
    }
}

void MainWindow::updatePatrolControls(bool stopping) {
    const auto text = stopping ? tr("正在停止…") : m_isPatrolRunning ? tr("停止巡检") : tr("启动巡检");
    const auto color = m_isPatrolRunning ? "#d32f2f" : "#00897b";
    for (auto* button : {m_startPatrolBtn}) {
        if (!button) continue;
        button->setText(text);
        button->setEnabled(!stopping);
        button->setStyleSheet(QString("QPushButton { background-color: %1; color: white; font-weight: bold; font-size: 13px; padding: 8px 16px; border-radius: 4px; } QPushButton:disabled { background-color: #555868; }").arg(color));
    }
}

void MainWindow::onStopAllDevicesInspection() {
    if (!m_isPatrolRunning) return;
    updatePatrolControls(true);
    m_inspectionSummaryLabel->setText(tr("正在停止，等待在途任务退出..."));
    m_inspectionService->stop();
}

void MainWindow::onInspectionOutcome(const InspectionOutcome& outcome) {
    if (outcome.inputKind == InspectionInputKind::CameraSnapshot)
        m_discoveryService->recordInspectionState(outcome.cameraId, outcome.errorCode, outcome.capturedAt.isValid());
    if (!outcome.databaseError.isEmpty()) onLogReceived(tr("[数据库] 设备 %1 · 任务 %2 · %3 · 待同步: %4")
        .arg(outcome.cameraId,outcome.taskId,outcome.databaseError).arg(outcome.databasePending));
    const int row = m_patrolCameraIdQueue.indexOf(outcome.cameraId);
    if (row < 0 || row >= m_inspectionRecords.size()) return;
    auto& rec = m_inspectionRecords[row];
    if (!outcome.deviceSnapshot.isEmpty()) rec.device = deviceFromJson(outcome.deviceSnapshot);
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
    rec.readingValid = valid;
    rec.safetyAssessment = outcome.safetyAssessment;
    if (rec.safetyAssessment.isEmpty())
        rec.safetyAssessment = rec.device.safeWorkingRange.assess(res.value, valid && outcome.recognitionPerformed);
    const bool outsideSafeRange = rec.safetyAssessment["status"].toString() == "out_of_range";
    const bool invalidSafeRange = rec.safetyAssessment["status"].toString() == "invalid_configuration";
    const QString rangeDetails = tr("设备：%1（%2）\n读数：%3 %4\n安全工作范围：[%5，%6] %4")
        .arg(rec.customName, rec.cameraId).arg(res.value, 0, 'f', 3).arg(QString::fromStdString(res.unit))
        .arg(rec.safetyAssessment["safe_min"].toDouble(), 0, 'f', 3)
        .arg(rec.safetyAssessment["safe_max"].toDouble(), 0, 'f', 3);
    if (outsideSafeRange) rec.status = tr("异常：超出安全工作范围") +
        (outcome.errorCode.isEmpty() ? QString() : tr("；任务失败：%1").arg(outcome.errorCode));
    else if (invalidSafeRange) rec.status = tr("安全工作范围待配置或无效，待复核");
    const QString statusDetails = rec.status;
    const bool normal = valid && res.confidence >= 0.40 && !outsideSafeRange && !invalidSafeRange;
    QStringList abnormalReasons;
    if (outsideSafeRange) abnormalReasons.append(tr("超范围"));
    if (invalidSafeRange) abnormalReasons.append(tr("正常范围未配置或无效"));
    const auto& error = outcome.errorCode;
    if (!error.isEmpty()) {
        QString reason;
        if (error == "CALIBRATION_REQUIRED" || error == "HISTORICAL_CALIBRATION_REQUIRED") reason = tr("未完成标定");
        else if (error == "DEVICE_CONFIGURATION_REQUIRED" || error == "HISTORICAL_CONFIGURATION_MISSING") reason = tr("设备配置不完整");
        else if (error == "TEMPLATE_DATABASE_FAILED") reason = tr("模板读取失败");
        else if (error == "PROFILE_INVALID") reason = tr("模板配置无效");
        else if (error == "STORAGE_FAILED") reason = tr("结果保存失败");
        else if (error == "RECOGNITION_FAILED" || error == "RECOGNITION_EXCEPTION") reason = tr("识别失败");
        else if (error == "UNSUPPORTED_PROTOCOL") reason = tr("不支持的采集协议");
        else if (error == "NO_FRAME" && outcome.errorMessage.contains("uniform")) reason = tr("灰色或空白图像");
        else if (error == "NO_FRAME" || error == "OPEN_FAILED" || error == "READ_FAILED" ||
                 error == "CAPTURE_EXCEPTION" || error == "CAPTURE_TIMEOUT" || error == "NOT_OPEN" ||
                 error == "WORKER_FAILED" || error == "STALE_FRAME") reason = tr("图像采集失败");
        else reason = tr("任务失败（%1）").arg(error);
        abnormalReasons.append(reason);
    } else if (!res.success) abnormalReasons.append(tr("识别失败"));
    else if (!(res.confidence >= 0.40)) abnormalReasons.append(tr("识别置信度低"));
    rec.status = normal ? tr("正常") : tr("异常：%1").arg(abnormalReasons.join(tr("；")));

    rec.diagnosticText += "\n" + statusDetails;
    if (!normal) {
        rec.diagnosticText += "\n" + (outsideSafeRange ? rangeDetails : statusDetails);
        for (int column = 0; column < m_inspectionTable->columnCount(); ++column) {
            auto* cell = m_inspectionTable->item(row, column);
            if (!cell) continue;
            cell->setForeground(QColor("#ff5252"));
            cell->setBackground(QColor("#38191f"));
            auto font = cell->font(); font.setBold(true); cell->setFont(font);
            cell->setToolTip(rec.diagnosticText);
        }
    }
    if (outsideSafeRange) {
        if (auto* button = qobject_cast<QPushButton*>(m_inspectionTable->cellWidget(row, 10))) {
            button->setText(tr("查看异常设备实时画面"));
            button->setStyleSheet("QPushButton { background-color: #b71c1c; color: white; font-weight: bold; padding: 5px; }");
        }
        if (!m_safeRangeAlert) {
            m_safeRangeAlert = new InspectionAlarmDialog(this,
                [this](const QString& cameraId) {
                    // Dismiss the topmost list before bringing its owner forward.
                    m_safeRangeAlert->hide();
                    restoreMainWindow();
                    openDeviceLivePreview(cameraId);
                },
                [this](const QString& taskId) {
                    m_safeRangeAlert->hide();
                    restoreMainWindow();
                    for (int row = 0; row < m_inspectionRecords.size(); ++row) {
                        if (m_inspectionRecords[row].taskId == taskId) {
                            showInspectionPhotosDialog(row);
                            return;
                        }
                    }
                    QMessageBox::information(this, tr("巡检记录已更新"), tr("该异常所属的巡检记录已更新，请查看当前巡检报表。"));
                });
        }
        QList<InspectionAlarm> alarms;
        for (const auto& record : m_inspectionRecords) {
            const auto& assessment = record.safetyAssessment;
            if (assessment["status"].toString() != "out_of_range") continue;
            const auto unit = QString::fromStdString(record.unit);
            alarms.append({record.cameraId, record.device.installationLocation +
                    (record.device.installationPosition.isEmpty() ? QString() : " | " + record.device.installationPosition),
                tr("%1 %2").arg(record.readingValue, 0, 'f', 3).arg(unit),
                tr("[%1，%2] %3").arg(assessment["safe_min"].toDouble(), 0, 'f', 3)
                    .arg(assessment["safe_max"].toDouble(), 0, 'f', 3).arg(unit),
                m_discoveredItemsMap.contains(record.cameraId) &&
                    m_discoveredItemsMap.value(record.cameraId).protocolType != CameraProtocolType::LOCAL_FILE, record.taskId});
        }
        m_safeRangeAlert->setAlarms(alarms);
        m_safeRangeAlert->showNormal();
        m_safeRangeAlert->raise();
        m_safeRangeAlert->activateWindow();
        QApplication::alert(m_safeRangeAlert, 0);
        if (m_trayIcon && (!isVisible() || isMinimized())) {
            m_trayIcon->showMessage(tr("巡检异常读数提醒"),
                tr("设备 %1：%2\n本轮已有 %3 台设备超出安全工作范围，点击查看详情。")
                    .arg(rec.cameraId, rangeDetails).arg(alarms.size()),
                QSystemTrayIcon::Warning);
        }
        onLogReceived(tr("[SAFETY] %1").arg(rangeDetails));
    }
    m_inspectionTable->item(row, 4)->setText(valid ? QString::asprintf("%.3f %s", rec.readingValue, rec.unit.c_str()) : "--");
    m_inspectionTable->item(row, 5)->setText(valid ? QString::asprintf("%.1f°", rec.pointerAngle) : "--");
    m_inspectionTable->item(row, 6)->setText(QString::asprintf("%.1f%%", rec.confidence));
    m_inspectionTable->item(row, 7)->setText(rec.status);
    onLogReceived(tr("[CAPTURE] 设备 %1 · 采集方式 %2").arg(outcome.cameraId, outcome.captureMethod));
    if (!outcome.archiveMetadata.isEmpty()) {
        qint64 imageBytes=0;
        for(const auto& image:outcome.archiveMetadata["images"].toArray()) imageBytes+=qint64(image.toObject()["bytes"].toDouble());
        onLogReceived(tr("[ARCHIVE] 设备 %1 · %2 · 图片保留 %3 天 · 图片合计 %4 KB")
            .arg(outcome.cameraId).arg(outcome.archiveMetadata["anomaly"].toBool()?tr("异常样本"):tr("正常样本"))
            .arg(outcome.archiveMetadata["retention_days"].toInt()).arg(imageBytes/1024.0,0,'f',1));
    }
    const auto& timings = outcome.timings;
    const auto engineSteps = timings["engine_steps"].toObject();
    QStringList details;
    for (auto it = engineSteps.begin(); it != engineSteps.end(); ++it)
        if (it.value().isDouble()) details.append(it.key() + "=" + QString::number(it.value().toDouble(), 'f', 1));
    onLogReceived(tr("[RECOGNITION TIMING] 设备 %1 · 模板读取 %2 ms · 配置准备 %3 ms · %4")
        .arg(outcome.cameraId).arg(timings["template_load_ms"].toDouble(), 0, 'f', 1)
        .arg(timings["profile_setup_ms"].toDouble(), 0, 'f', 1).arg(details.join("; ")));
    logRecognitionStageTimings(outcome.cameraId, engineSteps);
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

QPushButton* MainWindow::createDeviceLiveButton(const QString& cameraId) {
    auto* button = new QPushButton(tr("查看实时画面"), m_inspectionTable);
    button->setEnabled(m_discoveredItemsMap.contains(cameraId) &&
        m_discoveredItemsMap.value(cameraId).protocolType != CameraProtocolType::LOCAL_FILE);
    button->setToolTip(tr("连接此设备并打开实时监控，巡检继续运行。"));
    connect(button, &QPushButton::clicked, this, [this, cameraId]() { openDeviceLivePreview(cameraId); });
    return button;
}

void MainWindow::openDeviceLivePreview(const QString& cameraId) {
    if (!m_discoveredItemsMap.contains(cameraId)) {
        QMessageBox::warning(this, tr("设备不存在"), tr("该设备已不在设备列表中，请确认设备档案。"));
        return;
    }
    if (m_discoveredItemsMap.value(cameraId).protocolType == CameraProtocolType::LOCAL_FILE) {
        QMessageBox::information(this, tr("无法打开实时画面"), tr("本地图像设备不提供实时监控画面。"));
        return;
    }
    if (m_previewCameraId == cameraId && m_cameraHasFrame) {
        if (!m_isLiveStreaming) onToggleLiveStream();
        showVisionPage(m_livePage, tr("📷 摄像头实时预览"));
        return;
    }
    if (auto* item = findListItemByCameraId(cameraId)) {
        item->setHidden(false);
        m_deviceListWidget->setCurrentItem(item);
        m_deviceListWidget->scrollToItem(item);
    }
    switchActiveCameraSource(cameraId);
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

    QSaveFile file(savePath);
    if (!file.open(QIODevice::WriteOnly)) {
        QMessageBox::warning(this, tr("保存失败"), tr("无法写入报表：%1").arg(file.errorString()));
        return;
    }
    QList<QStringList> rows;
    rows.append(QStringList{tr("序号"), tr("设备编号"), tr("安装区域/位置"), tr("最新读数"),
        tr("单位"), tr("识别置信度"), tr("巡检状态"), tr("巡检时间戳"),
        tr("正常下限"), tr("正常上限"), tr("安全下限"), tr("安全上限"), tr("安全范围检查")});
    for (int i = 0; i < m_inspectionRecords.size(); ++i) {
        const auto& r = m_inspectionRecords[i];
        QStringList row{QString::number(i + 1), r.cameraId, r.locationInfo,
            QString::number(r.readingValue, 'f', 3), QString::fromStdString(r.unit),
            QString::number(r.confidence) + "%", r.status, r.timestamp};
        const auto range = r.safetyAssessment;
        for (const auto& key : {"normal_min", "normal_max", "safe_min", "safe_max"})
            row.append(range["enabled"].toBool() && range[key].isDouble() ? QString::number(range[key].toDouble(), 'f', 3) : "");
        row.append(range["status"].toString() == "out_of_range" ? tr("超范围") :
            range["status"].toString() == "normal" ? tr("正常") :
            range["status"].toString() == "disabled" ? tr("未启用") : tr("未完成检查"));
        rows.append(row);
    }
    if (!CsvReport::write(file, rows) || !file.commit()) {
        QMessageBox::warning(this, tr("保存失败"), tr("报表未成功写入：%1").arg(file.errorString()));
        return;
    }
    QMessageBox::information(this, tr("导出成功"), tr("已成功导出 %1 条巡检数据至:\n%2").arg(m_inspectionRecords.size()).arg(savePath));
    onLogReceived(tr("[EXPORT] 巡检报表已成功导出至: %1").arg(savePath));
}

void MainWindow::showInspectionPhotosDialog(int row) {
    if (row < 0 || row >= m_inspectionRecords.size()) return;
    const InspectionRecord rec = m_inspectionRecords[row]; // Freeze the reviewed task across timer updates.

    QDialog dlg(this);
    dlg.setWindowTitle(tr("巡检视角矫正图 - %1 (%2)").arg(rec.cameraId).arg(rec.customName));
    dlg.resize(820, 780);
    dlg.setMinimumSize(700, 640);
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
    QString statusColor = rec.status == tr("正常") ? "#00e676" : "#f44336";
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

    QString readingStr = rec.readingValid ? QString::asprintf("%.3f %s", rec.readingValue, rec.unit.c_str()) : "--";
    QString confStr = (rec.confidence == 0.0 && rec.status != tr("正常")) ? "--" : QString::asprintf("%.1f%%", rec.confidence);
    QString timeStr = rec.timestamp.isEmpty() ? "-" : rec.timestamp;

    metricsRow->addWidget(makeMetricWidget(tr("最新读数:"), readingStr, "#00e676"));
    metricsRow->addWidget(makeMetricWidget(tr("识别置信度:"), confStr, "#ffb300"));
    metricsRow->addWidget(makeMetricWidget(tr("巡检抓拍时间:"), timeStr, "#e0e0e0"));
    metricsRow->addStretch();

    headerVLayout->addLayout(metricsRow);
    mainLayout->addWidget(headerFrame);

    // Load only the rectified image belonging to this frozen inspection record.
    cv::Mat step2Mat = rec.step2_rectified;
    if (step2Mat.empty() && !rec.step2Path.isEmpty() && QFileInfo::exists(rec.step2Path)) {
        step2Mat = cv::imread(rec.step2Path.toStdString());
    }
    // The process viewer presents only the rectified dial.
    QHBoxLayout *imagesLayout = new QHBoxLayout();
    imagesLayout->setSpacing(14);

    // Rectified dial card
    QGroupBox *step2Box = new QGroupBox(tr("切点视角矫正后的表盘图像"), &dlg);
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
        step2Label->setPixmap(pix.scaled(700, 520, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    } else {
        step2Label->setText(tr("<div align='center' style='color:#757575;'><br><br><br>📷 当前记录未保存视角矫正图<br><br><span style='font-size:12px;'>请重新巡检或使用历史原图重新分析</span></div>"));
    }
    step2Layout->addWidget(step2Label, 1);
    imagesLayout->addWidget(step2Box, 1);

    mainLayout->addLayout(imagesLayout, 1);

    // Bottom Action Bar
    QHBoxLayout *actionLayout = new QHBoxLayout();
    actionLayout->setSpacing(10);

    QPushButton *exportBtn = new QPushButton(tr("导出矫正图..."), &dlg);
    exportBtn->setStyleSheet("QPushButton { background-color: #00695c; color: #80cbc4; border: 1px solid #00897b; } "
                            "QPushButton:hover { background-color: #00897b; color: #ffffff; }");
    connect(exportBtn, &QPushButton::clicked, &dlg, [this, &rec, step2Mat]() {
        if (step2Mat.empty()) {
            QMessageBox::warning(this, tr("提示"), tr("当前记录未保存视角矫正图，无法导出。"));
            return;
        }
        const auto path = QFileDialog::getSaveFileName(this, tr("导出视角矫正图"),
            rec.cameraId + "_rectified.png", tr("PNG 图像 (*.png)"));
        if (path.isEmpty()) return;
        if (cv::imwrite(path.toStdString(), step2Mat))
            QMessageBox::information(this, tr("导出成功"), tr("视角矫正图已保存。"));
        else QMessageBox::warning(this, tr("导出失败"), tr("无法保存图片，请检查路径和写入权限。"));
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
        submitSingleTask(task);
        dlg.accept();

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


