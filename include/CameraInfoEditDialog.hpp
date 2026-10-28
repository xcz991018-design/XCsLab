#ifndef CAMERA_INFO_EDIT_DIALOG_HPP
#define CAMERA_INFO_EDIT_DIALOG_HPP

#include <QDialog>
#include <QLineEdit>
#include <QComboBox>
#include <QPushButton>
#include <QCheckBox>
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QString>
#include <QStringList>
#include "AutoCameraDiscoveryService.hpp"
#include "GaugeTemplateRecord.hpp"

class CameraInfoEditDialog : public QDialog {
    Q_OBJECT

public:
    CameraInfoEditDialog(const QString& cameraId, QWidget *parent = nullptr, bool isNewDevice = false,
                         GaugeTemplateBackend backend = {}, bool archiveOnly = false);
    ~CameraInfoEditDialog() override = default;

    QString getUpdatedCameraId() const;
    DiscoveredCameraItem getUpdatedDeviceItem() const;

private slots:
    void onSave();

private:
    void setupUi();
    void loadCameraInfo();
    bool saveCameraInfo();

    GaugeTemplateBackend m_templateBackend;
    QString m_originalCameraId;
    bool m_isNewDevice{false};
    bool m_archiveOnly{false};

    // Device Network & Registry Parameters
    QLineEdit *m_hardwareIdEdit{nullptr};
    QLineEdit *m_cameraIdEdit{nullptr};
    QLineEdit *m_customNameEdit{nullptr};
    QComboBox *m_protocolCombo{nullptr};
    QLineEdit *m_ipEdit{nullptr};
    QSpinBox *m_portSpin{nullptr};
    QSpinBox *m_rtspPortSpin{nullptr};
    QLineEdit *m_rtspUrlEdit{nullptr};
    QLineEdit *m_usernameEdit{nullptr};
    QLineEdit *m_passwordEdit{nullptr};

    // Gauge Profile & Location Parameters
    QLineEdit *m_installationPositionEdit{nullptr};
    QLineEdit *m_locationEdit{nullptr};
    QLineEdit *m_deviceTagEdit{nullptr};
    QLineEdit *m_manufacturerEdit{nullptr};

    QCheckBox *m_safeRangeEnabled{nullptr};
    QDoubleSpinBox *m_normalMin{nullptr}, *m_normalMax{nullptr};
    QLabel *m_safeRangePreview{nullptr};
    QPushButton *m_saveBtn{nullptr};
    QPushButton *m_cancelBtn{nullptr};
    QTabWidget *m_tabWidget{nullptr};
};

#endif // CAMERA_INFO_EDIT_DIALOG_HPP
