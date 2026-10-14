#include "GaugeTemplateDialog.hpp"
#include "DeviceConfigManager.hpp"
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrent>

namespace {
struct TemplateOperationResult {
    GaugeTemplateStore store;
    QString error;
    bool ok{false};
};
class TemplateEditor : public QDialog {
public:
    TemplateEditor(const QJsonObject& original, const QJsonObject& effective, bool creating, QWidget* parent)
        : QDialog(parent), m_original(original) {
        setObjectName("gaugeTemplateEditor");
        setWindowTitle(creating ? tr("新增表计算法模板") : tr("编辑表计算法模板"));
        resize(620, 660);
        auto* layout = new QVBoxLayout(this);
        auto* tabs = new QTabWidget(this);
        m_tabs = tabs;
        const QStringList titles{tr("量程与刻度"), tr("指针检测"), tr("抗干扰"), tr("尾部与灰度"), tr("评分权重")};
        QVector<QFormLayout*> forms;
        for (const auto& title : titles) {
            auto* panel = new QWidget;
            auto* form = new QFormLayout(panel);
            form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
            auto* scroll = new QScrollArea(tabs);
            scroll->setWidgetResizable(true);
            scroll->setWidget(panel);
            tabs->addTab(scroll, title);
            forms.append(form);
        }
        m_id = new QLineEdit(GaugeTemplateStore::idOf(original), this);
        m_id->setObjectName("templateId");
        m_id->setReadOnly(!creating);
        m_id->setMaxLength(128);
        m_id->setPlaceholderText(tr("例如：PRESSURE_4MPA"));
        m_unit = new QLineEdit(original["unit"].toString(effective["unit"].toString("MPa")), this);
        m_unit->setObjectName("templateUnit");
        forms[0]->addRow(tr("模板编号"), m_id);
        forms[0]->addRow(tr("读数单位"), m_unit);
        auto* hint = new QLabel(tr("量程和数值增长方向由模板设置。\n每台设备必须在矫正图上完成轴心和刻度标定后才能识别。"), this);
        hint->setWordWrap(true);
        forms[0]->addRow(hint);
        for (const auto& field : GaugeTemplateStore::fields()) {
            auto value = original[field.key];
            if (value.isUndefined()) for (const auto& alias : field.aliases)
                if (original.contains(alias)) { value = original[alias]; break; }
            if (value.isUndefined()) value = effective.contains(field.key) ? effective[field.key] : field.defaultValue;
            QWidget* editor;
            if (field.defaultValue.isBool()) {
                auto* check = new QCheckBox(this);
                check->setChecked(value.toBool());
                editor = check;
            } else {
                auto* spin = new QDoubleSpinBox(this);
                spin->setDecimals(field.decimals);
                spin->setRange(field.minimum, field.maximum);
                spin->setSingleStep(field.maximum <= 1 ? 0.01 : field.key.contains("Step") ? 0.01 : 1.0);
                spin->setValue(value.toDouble());
                editor = spin;
            }
            editor->setObjectName("template_" + field.key);
            forms[field.page]->addRow(field.label, editor);
            m_editors[field.key] = editor;
            m_initial[field.key] = value;
        }
        layout->addWidget(tabs);
        m_error = new QLabel(this);
        m_error->setObjectName("templateValidationError");
        m_error->setWordWrap(true);
        m_error->setStyleSheet("color: #ef5350;");
        layout->addWidget(m_error);
        auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
        m_buttons = buttons;
        buttons->button(QDialogButtonBox::Save)->setObjectName("saveTemplate");
        buttons->button(QDialogButtonBox::Save)->setText(tr("保存模板"));
        buttons->button(QDialogButtonBox::Cancel)->setText(tr("取消"));
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(buttons, &QDialogButtonBox::accepted, this, [this]() {
            const auto error = GaugeTemplateStore::validate(value());
            if (!error.isEmpty()) { showError(error); return; }
            // The manager performs the atomic write before closing this editor.
            if (save && !save(value())) return;
            accept();
        });
    }
    QJsonObject value() const {
        auto profile = m_original;
        profile.remove("profile_id");
        profile.remove("min_angle_deg"); profile.remove("max_angle_deg");
        profile["gauge_id"] = m_id->text().trimmed();
        profile["unit"] = m_unit->text().trimmed();
        for (const auto& field : GaugeTemplateStore::fields()) {
            const auto* editor = m_editors[field.key];
            const QJsonValue value = field.defaultValue.isBool()
                ? QJsonValue(static_cast<const QCheckBox*>(editor)->isChecked())
                : QJsonValue(static_cast<const QDoubleSpinBox*>(editor)->value());
            // Leave absent/defaulted settings and extra fields intact when editing.
            if (value != m_initial[field.key] || profile.contains(field.key)) {
                for (const auto& alias : field.aliases) profile.remove(alias);
                profile[field.key] = value;
            }
        }
        return profile;
    }
    void showError(const QString& error) { m_error->setText(error); }
    void setBusy(bool busy) {
        m_busy = busy;
        m_tabs->setEnabled(!busy);
        m_buttons->setEnabled(!busy);
        if (busy) m_error->setText(tr("正在保存到数据库…"));
    }
    void reject() override { if (!m_busy) QDialog::reject(); }
    std::function<bool(const QJsonObject&)> save;
private:
    QJsonObject m_original;
    QMap<QString, QWidget*> m_editors;
    QMap<QString, QJsonValue> m_initial;
    QLineEdit* m_id;
    QLineEdit* m_unit;
    QLabel* m_error;
    QTabWidget* m_tabs;
    QDialogButtonBox* m_buttons;
    bool m_busy{false};
};
}

GaugeTemplateDialog::GaugeTemplateDialog(QWidget* parent, GaugeTemplateBackend backend, UsageLookup usageLookup)
    : QDialog(parent), m_store(std::move(backend)), m_usageLookup(std::move(usageLookup)) {
    m_pool.setMaxThreadCount(1);
    setObjectName("gaugeTemplateManager");
    setWindowTitle(tr("表计算法模板管理"));
    resize(820, 520);
    setStyleSheet(
        "QDialog, QWidget { background-color: #161822; color: #d1d5db; font-family: 'Microsoft YaHei'; font-size: 13px; }"
        "QListWidget, QLineEdit, QDoubleSpinBox { background-color: #1f2230; border: 1px solid #33384c; border-radius: 4px; padding: 5px; color: #ffffff; }"
        "QListWidget::item:selected { background-color: #24516b; color: #ffffff; }"
        "QPushButton { background-color: #272a3a; border: 1px solid #3a3f58; border-radius: 4px; padding: 6px 12px; }"
        "QPushButton:hover { border-color: #00e5ff; }"
        "QPushButton:disabled { color: #777b88; }"
        "QTabBar::tab { padding: 8px 12px; background-color: #272a3a; }"
        "QTabBar::tab:selected { color: #00e5ff; background-color: #1f2230; }"
    );
    auto* layout = new QVBoxLayout(this);
    auto* hint = new QLabel(tr("模板直接保存到 MySQL 数据库。保存后立即生效，历史任务保留原有配置快照。"), this);
    hint->setWordWrap(true);
    layout->addWidget(hint);
    auto* content = new QHBoxLayout;
    m_list = new QListWidget(this);
    m_list->setObjectName("gaugeTemplateList");
    m_list->setMinimumWidth(300);
    m_details = new QLabel(this);
    m_details->setObjectName("gaugeTemplateDetails");
    m_details->setWordWrap(true);
    m_details->setTextFormat(Qt::PlainText);
    m_details->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    content->addWidget(m_list, 1);
    content->addWidget(m_details, 1);
    layout->addLayout(content, 1);
    m_status = new QLabel(this);
    m_status->setObjectName("gaugeTemplateStatus");
    m_status->setWordWrap(true);
    layout->addWidget(m_status);
    auto* actions = new QHBoxLayout;
    m_add = new QPushButton(tr("新增模板"), this);
    m_copy = new QPushButton(tr("复制模板"), this);
    m_edit = new QPushButton(tr("编辑模板"), this);
    m_delete = new QPushButton(tr("删除模板"), this);
    auto* refresh = new QPushButton(tr("刷新"), this);
    m_refresh = refresh;
    auto* close = new QPushButton(tr("关闭"), this);
    m_add->setObjectName("addGaugeTemplate");
    m_copy->setObjectName("copyGaugeTemplate");
    m_edit->setObjectName("editGaugeTemplate");
    m_delete->setObjectName("deleteGaugeTemplate");
    for (auto* button : {m_add, m_copy, m_edit, m_delete, refresh}) actions->addWidget(button);
    actions->addStretch();
    actions->addWidget(close);
    layout->addLayout(actions);
    connect(m_list, &QListWidget::currentRowChanged, this, [this]() { updateSelection(); });
    connect(m_list, &QListWidget::itemDoubleClicked, this, [this]() { editProfile(false); });
    connect(m_add, &QPushButton::clicked, this, [this]() { editProfile(true); });
    connect(m_copy, &QPushButton::clicked, this, [this]() { editProfile(true, true); });
    connect(m_edit, &QPushButton::clicked, this, [this]() { editProfile(false); });
    connect(m_delete, &QPushButton::clicked, this, &GaugeTemplateDialog::deleteProfile);
    connect(refresh, &QPushButton::clicked, this, [this]() { reload(m_list->currentItem() ? m_list->currentItem()->text() : QString()); });
    connect(close, &QPushButton::clicked, this, &QDialog::accept);
    reload();
}

GaugeTemplateDialog::~GaugeTemplateDialog() { m_pool.waitForDone(); }

void GaugeTemplateDialog::setBusy(bool busy) {
    m_busy = busy;
    m_list->setEnabled(!busy);
    m_add->setEnabled(!busy);
    m_refresh->setEnabled(!busy);
    updateSelection();
}

QStringList GaugeTemplateDialog::usedBy(const QString& id) const {
    if (m_usageLookup) return m_usageLookup(id);
    QStringList devices = m_store.usedBy(id);
    for (const auto& device : DeviceConfigManager::instance().getAllDevices())
        if ((device.defaultProfileId.isEmpty() ? QStringLiteral(u"PRESSURE_GAUGE_1.6MPA") : device.defaultProfileId) == id)
            devices.append(device.cameraId);
    devices.removeDuplicates(); return devices;
}

void GaugeTemplateDialog::setDeletionAvailable(bool available) {
    m_deletionAvailable = available;
    m_delete->setToolTip(available ? QString() : tr("设备档案尚未成功加载，请完成加载后重新打开模板管理窗口。"));
    updateSelection();
}

void GaugeTemplateDialog::reload(const QString& selected) {
    if (m_busy) return;
    setBusy(true);
    m_status->setText(tr("正在读取数据库模板库…"));
    auto* watcher = new QFutureWatcher<TemplateOperationResult>(this);
    connect(watcher, &QFutureWatcher<TemplateOperationResult>::finished, this, [this, watcher, selected]() {
        const auto result = watcher->result(); watcher->deleteLater();
        m_store = result.store;
        const QSignalBlocker block(m_list);
        m_list->clear();
        if (result.ok) m_list->addItems(m_store.ids());
        setBusy(false);
        m_add->setEnabled(result.ok);
        m_status->setText(result.ok ? tr("数据库中共 %1 个模板").arg(m_list->count()) : tr("数据库模板加载失败：%1").arg(result.error));
        int row = m_store.ids().indexOf(selected);
        if (row < 0 && m_list->count()) row = 0;
        m_list->setCurrentRow(row);
        updateSelection();
    });
    watcher->setFuture(QtConcurrent::run(&m_pool, [store = m_store]() mutable {
        TemplateOperationResult result{store, QString(), false};
        result.ok = result.store.load(result.error);
        return result;
    }));
}

void GaugeTemplateDialog::updateSelection() {
    const bool selected = m_list->currentItem() != nullptr;
    m_copy->setEnabled(selected && !m_busy);
    m_edit->setEnabled(selected && !m_busy);
    m_delete->setEnabled(selected && m_deletionAvailable && !m_busy);
    if (!selected) { m_details->setText(tr("尚无模板。点击“新增模板”设置量程、刻度和算法参数。")); return; }
    const auto id = m_list->currentItem()->text();
    const auto profile = m_store.effectiveProfile(id);
    const auto devices = usedBy(id);
    m_details->setText(tr("模板编号：%1\n\n量程：%2 ～ %3 %4\n数值方向：%5\n\n使用设备：%6")
        .arg(id).arg(profile["min_val"].toDouble()).arg(profile["max_val"].toDouble(1))
        .arg(profile["unit"].toString("MPa")).arg(profile["clockwise"].toBool(true) ? tr("顺时针") : tr("逆时针"))
        .arg(devices.isEmpty() ? tr("暂无") : devices.join("、")));
}

void GaugeTemplateDialog::editProfile(bool creating, bool copying) {
    if (m_busy) return;
    if ((!creating || copying) && !m_list->currentItem()) return;
    auto profile = creating && !copying ? GaugeTemplateStore::defaults() : m_store.profile(m_list->currentItem()->text());
    const auto effective = creating && !copying ? profile : m_store.effectiveProfile(GaugeTemplateStore::idOf(profile));
    if (copying) {
        profile.remove("profile_id");
        profile.remove("min_angle_deg"); profile.remove("max_angle_deg");
        QString id = GaugeTemplateStore::idOf(profile) + "_COPY";
        int suffix = 2;
        while (m_store.ids().contains(id)) id = GaugeTemplateStore::idOf(profile) + "_COPY_" + QString::number(suffix++);
        profile["gauge_id"] = id;
    }
    TemplateEditor editor(profile, effective, creating, this);
    QString savedId;
    editor.save = [&](const QJsonObject& value) {
        editor.setBusy(true);
        auto* watcher = new QFutureWatcher<TemplateOperationResult>(&editor);
        connect(watcher, &QFutureWatcher<TemplateOperationResult>::finished, &editor, [&, watcher, value]() {
            const auto result = watcher->result(); watcher->deleteLater();
            editor.setBusy(false);
            if (!result.ok) { editor.showError(result.error); return; }
            m_store = result.store;
            savedId = GaugeTemplateStore::idOf(value);
            emit templatesChanged();
            editor.accept();
        });
        watcher->setFuture(QtConcurrent::run(&m_pool, [store = m_store, value, creating]() mutable {
            TemplateOperationResult result{store, QString(), false};
            result.ok = result.store.saveProfile(value, !creating, result.error);
            return result;
        }));
        return false;
    };
    if (editor.exec() == QDialog::Accepted) reload(savedId);
}

void GaugeTemplateDialog::deleteProfile() {
    if (m_busy || !m_deletionAvailable || !m_list->currentItem()) return;
    const auto id = m_list->currentItem()->text();
    auto devices = usedBy(id);
    QString error;
    if (!devices.isEmpty()) {
        m_store.removeProfile(id, devices, error);
        m_status->setText(error); return;
    }
    QDialog confirmation(this);
    confirmation.setWindowTitle(tr("删除模板"));
    auto* layout = new QVBoxLayout(&confirmation);
    auto* label = new QLabel(tr("确认删除模板“%1”？").arg(id), &confirmation);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    layout->addWidget(label);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Yes | QDialogButtonBox::No, &confirmation);
    buttons->button(QDialogButtonBox::Yes)->setText(tr("删除"));
    buttons->button(QDialogButtonBox::Yes)->setObjectName("confirmDeleteTemplate");
    buttons->button(QDialogButtonBox::Yes)->setDefault(false);
    buttons->button(QDialogButtonBox::No)->setText(tr("取消"));
    buttons->button(QDialogButtonBox::No)->setDefault(true);
    connect(buttons, &QDialogButtonBox::accepted, &confirmation, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &confirmation, &QDialog::reject);
    layout->addWidget(buttons);
    if (confirmation.exec() != QDialog::Accepted) return;
    devices = usedBy(id);
    setBusy(true);
    m_status->setText(tr("正在从数据库删除模板…"));
    auto* watcher = new QFutureWatcher<TemplateOperationResult>(this);
    connect(watcher, &QFutureWatcher<TemplateOperationResult>::finished, this, [this, watcher]() {
        const auto result = watcher->result(); watcher->deleteLater();
        setBusy(false);
        if (!result.ok) { m_status->setText(result.error); return; }
        m_store = result.store;
        emit templatesChanged(); reload();
    });
    watcher->setFuture(QtConcurrent::run(&m_pool, [store = m_store, id, devices]() mutable {
        TemplateOperationResult result{store, QString(), false};
        result.ok = result.store.removeProfile(id, devices, result.error);
        return result;
    }));
}
