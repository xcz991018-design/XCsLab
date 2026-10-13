#pragma once
#include "GaugeTemplateStore.hpp"
#include <QDialog>
#include <functional>
#include <QThreadPool>

class QListWidget;
class QLabel;
class QPushButton;

class GaugeTemplateDialog : public QDialog {
    Q_OBJECT
public:
    using UsageLookup = std::function<QStringList(const QString&)>;
    explicit GaugeTemplateDialog(QWidget* parent = nullptr, GaugeTemplateBackend backend = {},
                                 UsageLookup usageLookup = UsageLookup());
    ~GaugeTemplateDialog() override;
    void setDeletionAvailable(bool available);
signals:
    void templatesChanged();
private:
    void reload(const QString& selected = QString());
    void updateSelection();
    void setBusy(bool busy);
    void editProfile(bool creating, bool copying = false);
    void deleteProfile();
    QStringList usedBy(const QString& id) const;
    GaugeTemplateStore m_store;
    UsageLookup m_usageLookup;
    bool m_deletionAvailable{true};
    bool m_busy{false};
    QThreadPool m_pool;
    QListWidget* m_list;
    QLabel* m_details;
    QLabel* m_status;
    QPushButton* m_add;
    QPushButton* m_copy;
    QPushButton* m_edit;
    QPushButton* m_delete;
    QPushButton* m_refresh;
};
