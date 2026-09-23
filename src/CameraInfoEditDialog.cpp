#include "CameraInfoEditDialog.hpp"
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QMessageBox>
#include <QFileInfo>
#include <QDebug>

CameraInfoEditDialog::CameraInfoEditDialog(const QString& configPath, const QString& cameraId, QWidget *parent)
    : QDialog(parent), m_configPath(configPath), m_originalCameraId(cameraId) {
    setupUi();
    loadCameraInfo();
}

QStringList CameraInfoEditDialog::loadAvailableProfiles() {
    QStringList profiles;
    std::vector<QString> candidates = {
        "config/gauge_profile.json",
        "../config/gauge_profile.json",
        "../../config/gauge_profile.json",
        "gauge_profile.json",
        "C:/Users/95254/code/GaugeReaderTest/config/gauge_profile.json"
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
    setWindowTitle(tr("编辑摄像头与表头属性档案 (Camera & Gauge Editor)"));
    resize(560, 520);
    setStyleSheet("QDialog { background-color: #121216; color: #e0e0e0; font-family: 'Segoe UI', Microsoft YaHei, sans-serif; }"
                  "QLabel { color: #b0bec5; font-size: 13px; font-weight: 500; }"
                  "QLineEdit { background-color: #1a1a24; border: 1px solid #33384c; border-radius: 4px; padding: 6px; color: #00e5ff; font-weight: bold; font-size: 13px; }"
                  "QLineEdit:focus { border: 1px solid #00e5ff; background-color: #202230; }");

    QVBoxLayout *mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(16, 16, 16, 16);
    mainLayout->setSpacing(12);

    QLabel *headerLabel = new QLabel(tr("✏️ 修改摄像头节点属性与表头参数档案:"), this);
    headerLabel->setStyleSheet("font-size: 14px; font-weight: bold; color: #00e5ff; padding-bottom: 4px;");
    mainLayout->addWidget(headerLabel);

    QFormLayout *formLayout = new QFormLayout();
    formLayout->setSpacing(10);
    formLayout->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);

    m_cameraIdEdit = new QLineEdit(this);

    m_gaugeProfileCombo = new QComboBox(this);
    m_gaugeProfileCombo->addItems(loadAvailableProfiles());
    m_gaugeProfileCombo->setStyleSheet("QComboBox { background-color: #1a1a24; border: 1px solid #33384c; border-radius: 4px; padding: 6px; color: #ffd700; font-weight: bold; font-size: 13px; }"
                                       "QComboBox::drop-down { border: none; }"
                                       "QComboBox QAbstractItemView { background-color: #1f2230; color: #ffffff; selection-background-color: #005c99; }");

    m_substationEdit = new QLineEdit(this);
    m_areaEdit = new QLineEdit(this);
    m_locationEdit = new QLineEdit(this);
    m_deviceTagEdit = new QLineEdit(this);
    m_gaugeTypeEdit = new QLineEdit(this);
    m_manufacturerEdit = new QLineEdit(this);
    m_modelNameEdit = new QLineEdit(this);
    m_accuracyClassEdit = new QLineEdit(this);

    formLayout->addRow(tr("摄像头编号 (camera_id):"), m_cameraIdEdit);
    formLayout->addRow(tr("表计模板 (gauge_profile):"), m_gaugeProfileCombo);
    formLayout->addRow(tr("变电站/厂区 (substation):"), m_substationEdit);
    formLayout->addRow(tr("安装区域 (area):"), m_areaEdit);
    formLayout->addRow(tr("安装点位 (installation_location):"), m_locationEdit);
    formLayout->addRow(tr("设备标签 (device_tag):"), m_deviceTagEdit);
    formLayout->addRow(tr("表计类型 (gauge_type):"), m_gaugeTypeEdit);
    formLayout->addRow(tr("生产厂家 (manufacturer):"), m_manufacturerEdit);
    formLayout->addRow(tr("规格型号 (model_name):"), m_modelNameEdit);
    formLayout->addRow(tr("精度等级 (accuracy_class):"), m_accuracyClassEdit);

    mainLayout->addLayout(formLayout, 1);

    // Action Buttons
    QHBoxLayout *btnLayout = new QHBoxLayout();
    m_saveBtn = new QPushButton(tr("💾 保存修改并更新配置文件"), this);
    m_cancelBtn = new QPushButton(tr("取消"), this);

    m_saveBtn->setStyleSheet("QPushButton { background-color: #00897b; color: white; font-weight: bold; padding: 8px 18px; border-radius: 4px; font-size: 13px; }"
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
    QFile file(m_configPath);
    if (!file.open(QIODevice::ReadOnly)) {
        QStringList candidates = {
            m_configPath,
            "../" + m_configPath,
            "config/" + QFileInfo(m_configPath).fileName()
        };
        for (const auto& cand : candidates) {
            file.setFileName(cand);
            if (file.open(QIODevice::ReadOnly)) break;
        }
    }

    if (!file.isOpen()) return;

    QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    file.close();

    if (!doc.isObject()) return;
    QJsonObject root = doc.object();
    QJsonArray camArr = root.contains("cameras") ? root["cameras"].toArray() : root["profiles"].toArray();

    for (const auto& val : camArr) {
        if (!val.isObject()) continue;
        QJsonObject obj = val.toObject();
        QString camId = obj.contains("camera_id") ? obj["camera_id"].toString() : obj["gauge_id"].toString();
        if (camId == m_originalCameraId) {
            m_cameraIdEdit->setText(camId);

            QString profStr = obj["gauge_profile"].toString();
            if (!profStr.isEmpty()) {
                if (m_gaugeProfileCombo->findText(profStr) < 0) {
                    m_gaugeProfileCombo->addItem(profStr);
                }
                m_gaugeProfileCombo->setCurrentText(profStr);
            }

            m_substationEdit->setText(obj["substation"].toString());
            m_areaEdit->setText(obj["area"].toString());
            m_locationEdit->setText(obj["installation_location"].toString());
            m_deviceTagEdit->setText(obj["device_tag"].toString());
            m_gaugeTypeEdit->setText(obj["gauge_type"].toString());
            m_manufacturerEdit->setText(obj["manufacturer"].toString());
            m_modelNameEdit->setText(obj["model_name"].toString());
            m_accuracyClassEdit->setText(obj["accuracy_class"].toString());
            break;
        }
    }
}

void CameraInfoEditDialog::onSave() {
    if (saveCameraInfo()) {
        accept();
    }
}

bool CameraInfoEditDialog::saveCameraInfo() {
    QString resolvedPath = m_configPath;
    QFile file(resolvedPath);

    if (!file.open(QIODevice::ReadWrite)) {
        QStringList candidates = {
            m_configPath,
            "../" + m_configPath,
            "config/" + QFileInfo(m_configPath).fileName()
        };
        for (const auto& cand : candidates) {
            file.setFileName(cand);
            if (file.open(QIODevice::ReadWrite)) {
                resolvedPath = cand;
                break;
            }
        }
    }

    if (!file.isOpen()) {
        QMessageBox::critical(this, tr("保存失败"), tr("无法打开配置文件以写入修改！"));
        return false;
    }

    QByteArray data = file.readAll();
    QJsonDocument doc = QJsonDocument::fromJson(data);
    if (!doc.isObject()) {
        file.close();
        QMessageBox::critical(this, tr("保存失败"), tr("配置文件格式无效 JSON 对象！"));
        return false;
    }

    QJsonObject root = doc.object();
    QString arrayKey = root.contains("cameras") ? "cameras" : "profiles";
    QJsonArray camArr = root[arrayKey].toArray();

    bool found = false;
    for (int i = 0; i < camArr.size(); ++i) {
        if (!camArr[i].isObject()) continue;
        QJsonObject obj = camArr[i].toObject();
        QString camId = obj.contains("camera_id") ? obj["camera_id"].toString() : obj["gauge_id"].toString();
        if (camId == m_originalCameraId) {
            obj["camera_id"] = m_cameraIdEdit->text().trimmed();
            obj["gauge_profile"] = m_gaugeProfileCombo->currentText().trimmed();
            obj["substation"] = m_substationEdit->text().trimmed();
            obj["area"] = m_areaEdit->text().trimmed();
            obj["installation_location"] = m_locationEdit->text().trimmed();
            obj["device_tag"] = m_deviceTagEdit->text().trimmed();
            obj["gauge_type"] = m_gaugeTypeEdit->text().trimmed();
            obj["manufacturer"] = m_manufacturerEdit->text().trimmed();
            obj["model_name"] = m_modelNameEdit->text().trimmed();
            obj["accuracy_class"] = m_accuracyClassEdit->text().trimmed();

            camArr[i] = obj;
            found = true;
            break;
        }
    }

    if (!found) {
        file.close();
        QMessageBox::warning(this, tr("未找到配置项"), tr("在配置文件中未找到匹配的摄像头节点 [%1]").arg(m_originalCameraId));
        return false;
    }

    root[arrayKey] = camArr;
    doc.setObject(root);

    file.seek(0);
    file.resize(0);
    file.write(doc.toJson(QJsonDocument::Indented));
    file.close();

    QMessageBox::information(this, tr("保存成功"), tr("摄像头与表头属性属性参数已成功保存更新到配置文件！"));
    return true;
}
