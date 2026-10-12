#pragma once
#include "DatabaseSettingsStore.hpp"
#include <QDialog>
#include <QThreadPool>
class QLineEdit;
class QSpinBox;
class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class DatabaseConnectionDialog : public QDialog {
public:
    explicit DatabaseConnectionDialog(QWidget* parent=nullptr, const QString& settingsPath=QString());
    ~DatabaseConnectionDialog() override;
private:
    DatabaseSettings settings() const;
    void testConnection();
    void saveSettings();
    void setBusy(bool);
    QString m_path;
    QThreadPool m_pool;
    QCheckBox* m_enabled;
    QLineEdit *m_host, *m_database, *m_user, *m_password, *m_ca;
    QSpinBox* m_port;
    QComboBox* m_tls;
    QLabel* m_status;
    QPushButton *m_test, *m_save;
    QWidget* m_form;
};
