#ifndef CAMERA_INFO_EDIT_DIALOG_HPP
#define CAMERA_INFO_EDIT_DIALOG_HPP

#include <QDialog>
#include <QLineEdit>
#include <QComboBox>
#include <QPushButton>
#include <QFormLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QString>
#include <QStringList>

class CameraInfoEditDialog : public QDialog {
    Q_OBJECT

public:
    CameraInfoEditDialog(const QString& configPath, const QString& cameraId, QWidget *parent = nullptr);
    ~CameraInfoEditDialog() override = default;

    QString getUpdatedCameraId() const { return m_cameraIdEdit ? m_cameraIdEdit->text().trimmed() : m_originalCameraId; }

private slots:
    void onSave();

private:
    void setupUi();
    void loadCameraInfo();
    bool saveCameraInfo();
    QStringList loadAvailableProfiles();

    QString m_configPath;
    QString m_originalCameraId;

    QLineEdit *m_cameraIdEdit{nullptr};
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
};

#endif // CAMERA_INFO_EDIT_DIALOG_HPP
