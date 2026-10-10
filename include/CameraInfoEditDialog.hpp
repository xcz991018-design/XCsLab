#ifndef CAMERA_INFO_EDIT_DIALOG_HPP
#define CAMERA_INFO_EDIT_DIALOG_HPP

#include <QDialog>
#include <QLineEdit>
#include <QComboBox>
#include <QPushButton>
#include <QCheckBox>
#include <QSpinBox>
#include <QFormLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QString>
#include <QStringList>
#include "AutoCameraDiscoveryService.hpp"

class CameraInfoEditDialog : public QDialog {
    Q_OBJECT

public:
    CameraInfoEditDialog(const QString& configPath, const QString& cameraId, QWidget *parent = nullptr, bool isNewDevice = false);
    ~CameraInfoEditDialog() override = default;

    QString getUpdatedCameraId() const;
    DiscoveredCameraItem getUpdatedDeviceItem() const;

private slots:
    void onSave();

private:
    void setupUi();
    void loadCameraInfo();
    bool saveCameraInfo();
    QStringList loadAvailableProfiles();

    QString m_configPath;
    QString m_originalCameraId;
    bool m_isNewDevice{false};

    // Device Network & Registry Parameters
    QLineEdit *m_cameraIdEdit{nullptr};
    QLineEdit *m_customNameEdit{nullptr};
    QComboBox *m_protocolCombo{nullptr};
    QLineEdit *m_ipEdit{nullptr};
    QSpinBox *m_portSpin{nullptr};
    QSpinBox *m_rtspPortSpin{nullptr};
    QLineEdit *m_rtspUrlEdit{nullptr};
    QCheckBox *m_autoConnectCheck{nullptr};

    // Gauge Profile & Location Parameters
    QComboBox *m_gaugeProfileCombo{nullptr};
    QLineEdit *m_substationEdit{nullptr};
    QLineEdit *m_areaEdit{nullptr};
    QLineEdit *m_locationEdit{nullptr};
    QLineEdit *m_deviceTagEdit{nullptr};
    QLineEdit *m_gaugeTypeEdit{nullptr};
    QLineEdit *m_manufacturerEdit{nullptr};
    QLineEdit *m_modelNameEdit{nullptr};
    QLineEdit *m_accuracyClassEdit{nullptr};

    QPushButton *m_saveBtn{nullptr};
    QPushButton *m_cancelBtn{nullptr};
    QTabWidget *m_tabWidget{nullptr};
};

#endif // CAMERA_INFO_EDIT_DIALOG_HPP
