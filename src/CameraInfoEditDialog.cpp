#include "CameraInfoEditDialog.hpp"
#include "DeviceConfigManager.hpp"
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QMessageBox>
#include <QFileInfo>
#include <QCoreApplication>
#include <QTabWidget>
#include <QDebug>

CameraInfoEditDialog::CameraInfoEditDialog(const QString& configPath, const QString& cameraId, QWidget *parent, bool isNewDevice)
    : QDialog(parent), m_configPath(configPath), m_originalCameraId(cameraId), m_isNewDevice(isNewDevice) {
    setupUi();
    loadCameraInfo();
}

QString CameraInfoEditDialog::getUpdatedCameraId() const {
    if (m_cameraIdEdit && !m_cameraIdEdit->text().trimmed().isEmpty()) {
        return m_cameraIdEdit->text().trimmed();
    }
    return m_originalCameraId;
}

DiscoveredCameraItem CameraInfoEditDialog::getUpdatedDeviceItem() const {
    DiscoveredCameraItem item;
    if (DeviceConfigManager::instance().hasDevice(m_originalCameraId)) {
        item = DeviceConfigManager::instance().getDevice(m_originalCameraId);
    }
    item.cameraId = getUpdatedCameraId();
    item.customName = m_customNameEdit ? m_customNameEdit->text().trimmed() : "";
    item.ipOrSn = m_ipEdit ? m_ipEdit->text().trimmed() : "192.168.1.101";

    int protoIdx = m_protocolCombo ? m_protocolCombo->currentIndex() : 0;
    switch (protoIdx) {
        case 0:
            item.protocolType = CameraProtocolType::ONVIF_RTSP_STREAM;
            item.protocolName = "ONVIF (RTSP)";
            break;
        case 1:
            item.protocolType = CameraProtocolType::SDK_JLINK;
            item.protocolName = "雄迈 JLink SDK";
            break;
        case 2:
            item.protocolType = CameraProtocolType::GB28181_PS_STREAM;
            item.protocolName = "国标 GB28181";
            break;
        case 3:
        default:
            item.protocolType = CameraProtocolType::LOCAL_FILE;
            item.protocolName = "智能图像采集终端";
            break;
    }

    item.modelName = m_modelNameEdit ? m_modelNameEdit->text().trimmed() : "Network Camera";
    item.port = m_portSpin ? m_portSpin->value() : 80;
    item.rtspPort = m_rtspPortSpin ? m_rtspPortSpin->value() : 554;
    item.rtspUrl = m_rtspUrlEdit ? m_rtspUrlEdit->text().trimmed() : "";
    item.username = "";
    item.password = "";
    item.defaultProfileId = m_gaugeProfileCombo ? m_gaugeProfileCombo->currentText() : "PRESSURE_GAUGE_1.6MPA";
    item.isAutoConnect = m_autoConnectCheck ? m_autoConnectCheck->isChecked() : false;
    item.isOnline = true;

    // Asset & Physical Installation Metadata
    item.substation = m_substationEdit ? m_substationEdit->text().trimmed() : "";
    item.area = m_areaEdit ? m_areaEdit->text().trimmed() : "";
    item.installationLocation = m_locationEdit ? m_locationEdit->text().trimmed() : "";
    item.deviceTag = m_deviceTagEdit ? m_deviceTagEdit->text().trimmed() : "";
    item.gaugeType = m_gaugeTypeEdit ? m_gaugeTypeEdit->text().trimmed() : "";
    item.manufacturer = m_manufacturerEdit ? m_manufacturerEdit->text().trimmed() : "";
    item.modelName = m_modelNameEdit ? m_modelNameEdit->text().trimmed() : item.modelName;
    item.accuracyClass = m_accuracyClassEdit ? m_accuracyClassEdit->text().trimmed() : "";

    return item;
}

QStringList CameraInfoEditDialog::loadAvailableProfiles() {
    QStringList profiles;
    std::vector<QString> candidates = {
        QCoreApplication::applicationDirPath() + "/config/gauge_profile.json",
        "config/gauge_profile.json",
        "../config/gauge_profile.json",
        "gauge_profile.json"
    };

    for (const auto& cand : candidates) {
        QFile file(cand);
        if (file.open(QIODevice::ReadOnly)) {
            QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
            file.close();
            if (doc.isObject()) {
                QJsonObject root = doc.object();
                QJsonArray arr = root.contains("profiles") ? root["profiles"].toArray() : root["cameras"].toArray();
                for (const auto& val : arr) {
                    if (val.isObject()) {
                        QJsonObject obj = val.toObject();
                        QString id = obj.contains("gauge_id") ? obj["gauge_id"].toString() : obj["profile_id"].toString();
                        if (!id.isEmpty() && !profiles.contains(id)) {
                            profiles.append(id);
                        }
                    }
                }
            }
            if (!profiles.isEmpty()) break;
        }
    }

    if (profiles.isEmpty()) {
        profiles << "PRESSURE_GAUGE_1.6MPA" << "PRESSURE_GAUGE_2.5MPA" << "VACUUM_GAUGE_1.0MPA" << "TEMP_GAUGE_150C";
    }

    return profiles;
}

void CameraInfoEditDialog::setupUi() {
    setWindowTitle(m_isNewDevice ? tr("➕ 添加新摄像头设备 (Add Camera Device)") : tr("✏️ 编辑摄像头与设备连接档案 (Device & Camera Editor)"));
    resize(640, 560);
    setStyleSheet("QDialog { background-color: #121216; color: #e0e0e0; font-family: 'Segoe UI', Microsoft YaHei, sans-serif; }"
                  "QLabel { color: #b0bec5; font-size: 13px; font-weight: 500; }"
                  "QLineEdit, QSpinBox, QComboBox { background-color: #1a1a24; border: 1px solid #33384c; border-radius: 4px; padding: 6px; color: #00e5ff; font-weight: bold; font-size: 13px; }"
                  "QLineEdit:focus, QSpinBox:focus, QComboBox:focus { border: 1px solid #00e5ff; background-color: #202230; }"
                  "QTabWidget::pane { border: 1px solid #272a38; background-color: #14151c; border-radius: 4px; }"
                  "QTabBar::tab { background-color: #1a1c26; color: #9fa6b2; padding: 8px 18px; font-weight: bold; border: 1px solid #272a38; border-top-left-radius: 4px; border-top-right-radius: 4px; margin-right: 2px; }"
                  "QTabBar::tab:selected { background-color: #272a3e; color: #00e5ff; border-bottom-color: #00e5ff; }");

    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(14, 14, 14, 14);
    mainLayout->setSpacing(12);

    QLabel *headerLabel = new QLabel(m_isNewDevice ? tr("🌟 登记并配置新摄像头接入网络参数:") : tr("✏️ 修改摄像头节点网络连接属性与表计档案:"), this);
    headerLabel->setStyleSheet("font-size: 14px; font-weight: bold; color: #00e5ff; padding-bottom: 2px;");
    mainLayout->addWidget(headerLabel);

    m_tabWidget = new QTabWidget(this);

    // ================= Tab 1: 网络与连接参数 =================
    QWidget *netTab = new QWidget(m_tabWidget);
    QFormLayout *netForm = new QFormLayout(netTab);
    netForm->setSpacing(10);
    netForm->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);

    m_cameraIdEdit = new QLineEdit(netTab);
    m_cameraIdEdit->setPlaceholderText("例如: CAM_192_168_1_101 或 CAM_01");

    m_customNameEdit = new QLineEdit(netTab);
    m_customNameEdit->setPlaceholderText("例如: 1号主变油压表监控相机");

    m_protocolCombo = new QComboBox(netTab);
    m_protocolCombo->addItem("ONVIF (RTSP 标准视频流)");
    m_protocolCombo->addItem("雄迈 / JFTech JLink SDK (端口 34567)");
    m_protocolCombo->addItem("国标 GB28181 (PS流接入)");
    m_protocolCombo->addItem("本地图像采集终端 (Local File)");

    m_ipEdit = new QLineEdit(netTab);
    m_ipEdit->setPlaceholderText("例如: 192.168.1.101 或 雄迈设备序列号");

    QHBoxLayout *portLayout = new QHBoxLayout();
    m_portSpin = new QSpinBox(netTab);
    m_portSpin->setRange(1, 65535);
    m_portSpin->setValue(80);
    QLabel *rtspPortLabel = new QLabel(tr("RTSP 端口:"), netTab);
    m_rtspPortSpin = new QSpinBox(netTab);
    m_rtspPortSpin->setRange(1, 65535);
    m_rtspPortSpin->setValue(554);
    portLayout->addWidget(m_portSpin);
    portLayout->addWidget(rtspPortLabel);
    portLayout->addWidget(m_rtspPortSpin);

    m_rtspUrlEdit = new QLineEdit(netTab);
    m_rtspUrlEdit->setPlaceholderText("留空则系统根据IP/端口自动生成标准 RTSP 地址");

    m_autoConnectCheck = new QCheckBox(tr("⭐ 设为开机默认自动连接此设备 (Auto-Connect on Startup)"), netTab);
    m_autoConnectCheck->setStyleSheet("QCheckBox { color: #ffd740; font-weight: bold; font-size: 13px; }");

    netForm->addRow(tr("设备唯一编号 (camera_id):"), m_cameraIdEdit);
    netForm->addRow(tr("设备别名/备注 (custom_name):"), m_customNameEdit);
    netForm->addRow(tr("通信协议 (protocol):"), m_protocolCombo);
    netForm->addRow(tr("设备IP/序列号 (ip_or_sn):"), m_ipEdit);
    netForm->addRow(tr("控制端口 (port):"), portLayout);
    netForm->addRow(tr("自定义 RTSP 地址 (rtsp_url):"), m_rtspUrlEdit);
    netForm->addRow("", m_autoConnectCheck);

    m_tabWidget->addTab(netTab, tr("🌐 网络与自动连接配置"));

    // ================= Tab 2: 表计规格与安装档案 =================
    QWidget *gaugeTab = new QWidget(m_tabWidget);
    QFormLayout *gaugeForm = new QFormLayout(gaugeTab);
    gaugeForm->setSpacing(10);
    gaugeForm->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);

    m_gaugeProfileCombo = new QComboBox(gaugeTab);
    m_gaugeProfileCombo->addItems(loadAvailableProfiles());
    m_gaugeProfileCombo->setStyleSheet("QComboBox { color: #ffd700; font-weight: bold; }");

    m_substationEdit = new QLineEdit(gaugeTab);
    m_substationEdit->setPlaceholderText(tr("例如: 南京一期变电站 (必填)"));

    m_areaEdit = new QLineEdit(gaugeTab);
    m_areaEdit->setPlaceholderText(tr("例如: 10kV 高压室 (必填)"));

    m_locationEdit = new QLineEdit(gaugeTab);
    m_locationEdit->setPlaceholderText(tr("例如: #3 变压器压力监控位 (必填)"));

    m_deviceTagEdit = new QLineEdit(gaugeTab);
    m_deviceTagEdit->setPlaceholderText(tr("例如: PT-2026-01 (必填)"));

    m_gaugeTypeEdit = new QLineEdit(gaugeTab);
    m_gaugeTypeEdit->setPlaceholderText(tr("例如: 弹簧管耐震压力表 (必填)"));

    m_manufacturerEdit = new QLineEdit(gaugeTab);
    m_manufacturerEdit->setPlaceholderText(tr("例如: 红旗仪表有限公司 (必填)"));

    m_modelNameEdit = new QLineEdit(gaugeTab);
    m_modelNameEdit->setPlaceholderText(tr("例如: Y-100BF (必填)"));

    m_accuracyClassEdit = new QLineEdit(gaugeTab);
    m_accuracyClassEdit->setPlaceholderText(tr("例如: 1.6级 (必填)"));

    gaugeForm->addRow(tr("表计算法模板 (gauge_profile):"), m_gaugeProfileCombo);
    gaugeForm->addRow(tr("<font color='#ff5252'>*</font> 变电站/厂区 (substation):"), m_substationEdit);
    gaugeForm->addRow(tr("<font color='#ff5252'>*</font> 安装区域 (area):"), m_areaEdit);
    gaugeForm->addRow(tr("<font color='#ff5252'>*</font> 安装点位 (installation_location):"), m_locationEdit);
    gaugeForm->addRow(tr("<font color='#ff5252'>*</font> 设备标签 (device_tag):"), m_deviceTagEdit);
    gaugeForm->addRow(tr("<font color='#ff5252'>*</font> 表计类型 (gauge_type):"), m_gaugeTypeEdit);
    gaugeForm->addRow(tr("<font color='#ff5252'>*</font> 生产厂家 (manufacturer):"), m_manufacturerEdit);
    gaugeForm->addRow(tr("<font color='#ff5252'>*</font> 规格型号 (model_name):"), m_modelNameEdit);
    gaugeForm->addRow(tr("<font color='#ff5252'>*</font> 精度等级 (accuracy_class):"), m_accuracyClassEdit);

    m_tabWidget->addTab(gaugeTab, tr("⚙️ 表计规格与安装点位档案"));

    mainLayout->addWidget(m_tabWidget, 1);

    // Action Buttons
    QHBoxLayout *btnLayout = new QHBoxLayout();
    m_saveBtn = new QPushButton(tr("💾 保存设备配置并立即生效"), this);
    m_cancelBtn = new QPushButton(tr("取消"), this);

    m_saveBtn->setStyleSheet("QPushButton { background-color: #00897b; color: white; font-weight: bold; padding: 8px 20px; border-radius: 4px; font-size: 13px; }"
                             "QPushButton:hover { background-color: #004d40; }");
    m_cancelBtn->setStyleSheet("QPushButton { background-color: #2a2a38; border: 1px solid #3a3a4c; color: white; padding: 8px 16px; border-radius: 4px; }"
                               "QPushButton:hover { background-color: #3a3a4c; }");

    btnLayout->addStretch();
    btnLayout->addWidget(m_saveBtn);
    btnLayout->addWidget(m_cancelBtn);

    mainLayout->addLayout(btnLayout);

    connect(m_saveBtn, &QPushButton::clicked, this, &CameraInfoEditDialog::onSave);
    connect(m_cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
}

void CameraInfoEditDialog::loadCameraInfo() {
    if (m_isNewDevice) {
        int nextId = DeviceConfigManager::instance().getAllDevices().size() + 1;
        m_cameraIdEdit->setText(QString("CAM_IP_192_168_1_%1").arg(100 + nextId));
        m_customNameEdit->setText(QString("%1号新摄像头节点").arg(nextId));
        m_ipEdit->setText(QString("192.168.1.%1").arg(100 + nextId));
        m_protocolCombo->setCurrentIndex(0); // Default ONVIF
        m_portSpin->setValue(80);
        m_rtspPortSpin->setValue(554);
        m_autoConnectCheck->setChecked(false);

        // No fake default values, require explicit user input
        m_substationEdit->clear();
        m_areaEdit->clear();
        m_locationEdit->clear();
        m_deviceTagEdit->clear();
        m_gaugeTypeEdit->clear();
        m_manufacturerEdit->clear();
        m_modelNameEdit->clear();
        m_accuracyClassEdit->clear();
        return;
    }

    // 1. Load from DeviceConfigManager
    if (DeviceConfigManager::instance().hasDevice(m_originalCameraId)) {
        DiscoveredCameraItem dev = DeviceConfigManager::instance().getDevice(m_originalCameraId);
        m_cameraIdEdit->setText(dev.cameraId);
        m_customNameEdit->setText(dev.customName);
        m_ipEdit->setText(dev.ipOrSn);
        m_portSpin->setValue(dev.port > 0 ? dev.port : 80);
        m_rtspPortSpin->setValue(dev.rtspPort > 0 ? dev.rtspPort : 554);
        m_rtspUrlEdit->setText(dev.rtspUrl);
        m_autoConnectCheck->setChecked(dev.isAutoConnect);

        if (dev.protocolType == CameraProtocolType::ONVIF_RTSP_STREAM) {
            m_protocolCombo->setCurrentIndex(0);
        } else if (dev.protocolType == CameraProtocolType::SDK_JLINK) {
            m_protocolCombo->setCurrentIndex(1);
        } else if (dev.protocolType == CameraProtocolType::GB28181_PS_STREAM) {
            m_protocolCombo->setCurrentIndex(2);
        } else {
            m_protocolCombo->setCurrentIndex(3);
        }

        if (!dev.defaultProfileId.isEmpty()) {
            m_gaugeProfileCombo->setCurrentText(dev.defaultProfileId);
        }

        m_substationEdit->setText(dev.substation);
        m_areaEdit->setText(dev.area);
        m_locationEdit->setText(dev.installationLocation);
        m_deviceTagEdit->setText(dev.deviceTag);
        m_gaugeTypeEdit->setText(dev.gaugeType);
        m_manufacturerEdit->setText(dev.manufacturer);
        m_modelNameEdit->setText(dev.modelName);
        m_accuracyClassEdit->setText(dev.accuracyClass);
    } else {
        m_cameraIdEdit->setText(m_originalCameraId);
    }
}

void CameraInfoEditDialog::onSave() {
    if (saveCameraInfo()) {
        accept();
    }
}

bool CameraInfoEditDialog::saveCameraInfo() {
    QString newCamId = getUpdatedCameraId();
    if (newCamId.isEmpty()) {
        if (m_tabWidget) m_tabWidget->setCurrentIndex(0);
        QMessageBox::warning(this, tr("验证失败"), tr("设备编号 (camera_id) 不能为空，请输入设备唯一编号！"));
        if (m_cameraIdEdit) m_cameraIdEdit->setFocus();
        return false;
    }

    if (m_customNameEdit && m_customNameEdit->text().trimmed().isEmpty()) {
        if (m_tabWidget) m_tabWidget->setCurrentIndex(0);
        QMessageBox::warning(this, tr("验证失败"), tr("设备别名/备注 (custom_name) 不能为空！"));
        m_customNameEdit->setFocus();
        return false;
    }

    // Strict validation for Tab 2 (Physical Installation & Gauge Spec Metadata)
    if (m_substationEdit && m_substationEdit->text().trimmed().isEmpty()) {
        if (m_tabWidget) m_tabWidget->setCurrentIndex(1);
        QMessageBox::warning(this, tr("验证失败"), tr("【所属变电站/厂区 (substation)】为必填项，必须输入真实点位信息！"));
        m_substationEdit->setFocus();
        return false;
    }

    if (m_areaEdit && m_areaEdit->text().trimmed().isEmpty()) {
        if (m_tabWidget) m_tabWidget->setCurrentIndex(1);
        QMessageBox::warning(this, tr("验证失败"), tr("【安装区域 (area)】为必填项，必须输入真实安装区域！"));
        m_areaEdit->setFocus();
        return false;
    }

    if (m_locationEdit && m_locationEdit->text().trimmed().isEmpty()) {
        if (m_tabWidget) m_tabWidget->setCurrentIndex(1);
        QMessageBox::warning(this, tr("验证失败"), tr("【物理安装点位 (installation_location)】为必填项，必须输入具体的安装位置！"));
        m_locationEdit->setFocus();
        return false;
    }

    if (m_deviceTagEdit && m_deviceTagEdit->text().trimmed().isEmpty()) {
        if (m_tabWidget) m_tabWidget->setCurrentIndex(1);
        QMessageBox::warning(this, tr("验证失败"), tr("【设备标签/位号 (device_tag)】为必填项，必须输入现场设备位号！"));
        m_deviceTagEdit->setFocus();
        return false;
    }

    if (m_gaugeTypeEdit && m_gaugeTypeEdit->text().trimmed().isEmpty()) {
        if (m_tabWidget) m_tabWidget->setCurrentIndex(1);
        QMessageBox::warning(this, tr("验证失败"), tr("【表计类型 (gauge_type)】为必填项，必须输入表计类型！"));
        m_gaugeTypeEdit->setFocus();
        return false;
    }

    if (m_manufacturerEdit && m_manufacturerEdit->text().trimmed().isEmpty()) {
        if (m_tabWidget) m_tabWidget->setCurrentIndex(1);
        QMessageBox::warning(this, tr("验证失败"), tr("【生产厂家 (manufacturer)】为必填项，必须输入仪表生产厂家！"));
        m_manufacturerEdit->setFocus();
        return false;
    }

    if (m_modelNameEdit && m_modelNameEdit->text().trimmed().isEmpty()) {
        if (m_tabWidget) m_tabWidget->setCurrentIndex(1);
        QMessageBox::warning(this, tr("验证失败"), tr("【规格型号 (model_name)】为必填项，必须输入仪表型号！"));
        m_modelNameEdit->setFocus();
        return false;
    }

    if (m_accuracyClassEdit && m_accuracyClassEdit->text().trimmed().isEmpty()) {
        if (m_tabWidget) m_tabWidget->setCurrentIndex(1);
        QMessageBox::warning(this, tr("验证失败"), tr("【精度等级 (accuracy_class)】为必填项，必须输入仪表精度等级！"));
        m_accuracyClassEdit->setFocus();
        return false;
    }

    // 1. Save to DeviceConfigManager (devices.json)
    DiscoveredCameraItem item = getUpdatedDeviceItem();
    if (!m_isNewDevice && newCamId != m_originalCameraId) {
        DeviceConfigManager::instance().removeDevice(m_originalCameraId, false);
    }
    DeviceConfigManager::instance().addOrUpdateDevice(item, true);

    if (item.isAutoConnect) {
        DeviceConfigManager::instance().setDeviceAutoConnect(newCamId, true);
    }

    QMessageBox::information(this, tr("保存成功"), tr("摄像头网络参数与设备档案已成功更新至 devices.json！"));
    return true;
}
