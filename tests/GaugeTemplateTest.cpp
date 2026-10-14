#include "GaugeTemplateStore.hpp"
#include "GaugeTemplateDialog.hpp"
#include "GaugeTemplateMigration.hpp"
#include "CameraInfoEditDialog.hpp"
#include "GaugeProfile.hpp"
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QDoubleSpinBox>
#include <QEventLoop>
#include <QFile>
#include <QFontDatabase>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTimer>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>

static void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
static void waitUntil(const std::function<bool()>& ready) {
    QEventLoop loop; QTimer poll, timeout; bool expired = false;
    timeout.setSingleShot(true);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&]() { if (ready()) loop.quit(); });
    QObject::connect(&timeout, &QTimer::timeout, &loop, [&]() { expired = true; loop.quit(); });
    poll.start(2); timeout.start(5000);
    if (!ready()) loop.exec();
    require(!expired && ready(), "Asynchronous UI operation timed out");
}
struct FakeDatabase {
    std::mutex mutex;
    QMap<QString, GaugeTemplateRecord> rows;
    std::set<QString> deleted;
    bool offline{false};
};
static GaugeTemplateBackend fakeBackend(const std::shared_ptr<FakeDatabase>& database) {
    GaugeTemplateBackend backend;
    backend.load = [database]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        std::lock_guard<std::mutex> lock(database->mutex);
        if (database->offline) throw std::runtime_error("DATABASE_UNAVAILABLE");
        return database->rows.values();
    };
    backend.save = [database](const QJsonObject& definition, quint64 expected) {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
        std::lock_guard<std::mutex> lock(database->mutex);
        if (database->offline) throw std::runtime_error("DATABASE_UNAVAILABLE");
        const auto id = definition["gauge_id"].toString();
        if (!expected && (database->rows.contains(id) || database->deleted.count(id))) throw std::runtime_error("模板编号已存在");
        if (expected && (!database->rows.contains(id) || database->rows[id].revision != expected)) throw std::runtime_error("模板版本冲突");
        auto record = database->rows.value(id);
        record.id = id; record.definition = definition; record.revision = expected + 1;
        database->rows[id] = record; return record;
    };
    backend.remove = [database](const QString& id, quint64 expected) {
        std::lock_guard<std::mutex> lock(database->mutex);
        if (database->offline) throw std::runtime_error("DATABASE_UNAVAILABLE");
        if (!database->rows.contains(id) || database->rows[id].revision != expected) throw std::runtime_error("模板版本冲突");
        if (!database->rows[id].usedBy.isEmpty()) throw std::runtime_error("模板正在使用");
        database->rows.remove(id); database->deleted.insert(id);
    };
    return backend;
}
static void runEditor(QPushButton* button, const std::function<void(QDialog*)>& action) {
    bool opened = false;
    QTimer::singleShot(0, [&]() {
        auto* editor = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (!editor) return;
        opened = editor->objectName() == "gaugeTemplateEditor";
        if (opened) action(editor); else editor->reject();
    });
    button->click();
    require(opened, "Template editor missing");
}
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QFontDatabase::addApplicationFont("C:/Windows/Fonts/msyh.ttc");
    app.setFont(QFont("Microsoft YaHei", 10));
    try {
        auto database = std::make_shared<FakeDatabase>(); const auto backend = fakeBackend(database);
        GaugeTemplateStore store(backend); QString error;
        require(store.load(error) && store.ids().isEmpty(), "Empty database load failed");
        auto first = GaugeTemplateStore::defaults(); first["gauge_id"] = "PRESSURE_TEST"; first["max_val"] = 4;
        require(!first.contains("min_angle_deg") && !first.contains("max_angle_deg"),"Angle fields remain in template defaults");
        first["min_angle_deg"]=135; first["max_angle_deg"]=45;
        first["custom_data"] = QJsonObject{{"keep", true}};
        require(store.saveProfile(first, false, error), "Create failed");
        require(!store.profile("PRESSURE_TEST").contains("min_angle_deg") && !store.profile("PRESSURE_TEST").contains("max_angle_deg"),"Deprecated template angles saved");
        auto second = first; second["gauge_id"] = "TEMPERATURE_TEST"; second["unit"] = "℃"; second["max_val"] = 150;
        require(store.saveProfile(second, false, error), "Second create failed");
        require(!store.saveProfile(first, false, error), "Duplicate accepted");
        auto invalid = second; invalid["max_val"] = -1;
        require(!store.saveProfile(invalid, true, error), "Invalid range accepted");
        invalid = second; invalid["coarseAngleStep"] = 0;
        require(!store.saveProfile(invalid, true, error), "Zero search step accepted");
        invalid = second; invalid["parent_model_path"] = "gauge_profile.json";
        require(!store.saveProfile(invalid, true, error), "Local model reference accepted");
        GaugeTemplateStore stale(backend); require(stale.load(error), "Second reader failed");
        second["max_val"] = 200; require(store.saveProfile(second, true, error), "Update failed");
        require(!stale.saveProfile(second, true, error) && !stale.removeProfile("TEMPERATURE_TEST", {}, error), "Stale revision accepted");
        GaugeProfile algorithm; require(algorithm.loadFromJson(store.profile("TEMPERATURE_TEST")) && algorithm.max_val == 200, "In-memory algorithm load failed");
        auto nested = second; nested["extra"] = QJsonObject{{"max_val", 999}};
        require(algorithm.loadFromJson(nested) && algorithm.max_val == 200, "Unknown nested data overrode template parameters");
        auto sparse = QJsonObject{{"gauge_id", "SPARSE"}, {"unit", "MPa"}, {"min_val", 0}, {"max_val", 1}};
        require(algorithm.loadFromJson(sparse) && algorithm.pointerMaxWidth == GaugeProfile().pointerMaxWidth, "Previous template leaked");
        require(store.profile("TEMPERATURE_TEST")["custom_data"] == first["custom_data"], "Unknown parameters lost");
        {
            std::lock_guard<std::mutex> lock(database->mutex); database->rows["PRESSURE_TEST"].usedBy = QStringList{"DEVICE_1"};
        }
        require(!store.removeProfile("PRESSURE_TEST", {}, error), "Server usage check bypassed");
        require(store.load(error), "Usage reload failed");
        GaugeTemplateDialog manager(nullptr, backend); auto* list = manager.findChild<QListWidget*>("gaugeTemplateList");
        auto* add = manager.findChild<QPushButton*>("addGaugeTemplate");
        auto* edit = manager.findChild<QPushButton*>("editGaugeTemplate");
        auto* copy = manager.findChild<QPushButton*>("copyGaugeTemplate");
        auto* remove = manager.findChild<QPushButton*>("deleteGaugeTemplate");
        require(list && add && !add->isEnabled(), "Database read was not asynchronous");
        waitUntil([&]() { return list->isEnabled() && list->count() == 2; });
        int changes = 0; QObject::connect(&manager, &GaugeTemplateDialog::templatesChanged, [&]() { ++changes; });
        list->setCurrentRow(0); remove->click();
        require(changes == 0 && manager.findChild<QLabel*>("gaugeTemplateStatus")->text().contains("DEVICE_1"), "Used template deletion not explained");
        runEditor(add, [](QDialog* editor) {
            require(!editor->findChild<QDoubleSpinBox*>("template_min_angle_deg") && !editor->findChild<QDoubleSpinBox*>("template_max_angle_deg"),"Template angle controls remain visible");
            auto* id = editor->findChild<QLineEdit*>("templateId");
            auto* maximum = editor->findChild<QDoubleSpinBox*>("template_max_val");
            auto* save = editor->findChild<QPushButton*>("saveTemplate");
            id->setText("PRESSURE_TEST"); maximum->setValue(10); save->click();
            auto* poll = new QTimer(editor);
            QObject::connect(poll, &QTimer::timeout, editor, [=]() {
                if (editor->findChild<QLabel*>("templateValidationError")->text().contains("已存在")) {
                    poll->stop(); id->setText("UI_CREATED"); save->click();
                }
            });
            poll->start(2);
        });
        waitUntil([&]() { return list->isEnabled() && list->count() == 3; });
        require(changes == 1 && store.load(error) && store.profile("UI_CREATED")["max_val"].toDouble() == 10, "UI create failed");
        runEditor(edit, [](QDialog* editor) {
            editor->findChild<QDoubleSpinBox*>("template_max_val")->setValue(20);
            QDir().mkpath("test_outputs"); editor->grab().save(QDir::currentPath() + "/test_outputs/gauge_template_editor.png");
            editor->findChild<QPushButton*>("saveTemplate")->click();
        });
        waitUntil([&]() { return list->isEnabled(); });
        require(changes == 2 && store.load(error) && store.profile("UI_CREATED")["max_val"].toDouble() == 20, "UI edit failed");
        runEditor(copy, [](QDialog* editor) { editor->findChild<QPushButton*>("saveTemplate")->click(); });
        waitUntil([&]() { return list->isEnabled() && list->count() == 4; });
        require(changes == 3 && store.load(error) && store.profile("UI_CREATED_COPY")["max_val"].toDouble() == 20, "UI copy failed");
        QTimer::singleShot(0, [&]() {
            if (auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget())) dialog->findChild<QPushButton*>("confirmDeleteTemplate")->click();
        });
        remove->click(); waitUntil([&]() { return list->isEnabled() && list->count() == 3; });
        require(changes == 4 && store.load(error) && !store.ids().contains("UI_CREATED_COPY"), "UI delete failed");
        CameraInfoEditDialog device("", nullptr, true, backend); auto* picker = device.findChild<QComboBox*>("deviceGaugeTemplate");
        require(picker && !picker->isEnabled(), "Picker read blocked UI");
        waitUntil([&]() { return picker->isEnabled() && picker->count() == 3; });
        picker->setCurrentIndex(picker->findData("TEMPERATURE_TEST"));
        require(device.getUpdatedDeviceItem().defaultProfileId == "TEMPERATURE_TEST", "Picker stored wrong ID");
        QTimer::singleShot(0, [&]() {
            auto* dialog = qobject_cast<GaugeTemplateDialog*>(QApplication::activeModalWidget());
            if (!dialog) return;
            auto added = GaugeTemplateStore::defaults(); added["gauge_id"] = "REFRESHED";
            require(store.saveProfile(added, false, error), "Refresh setup failed");
            dialog->templatesChanged(); dialog->accept();
        });
        device.findChild<QPushButton*>("manageDeviceGaugeTemplates")->click();
        waitUntil([&]() { return picker->isEnabled() && picker->findData("REFRESHED") >= 0; });
        require(picker->currentData().toString() == "TEMPERATURE_TEST", "Picker selection lost on refresh");
        {
            std::lock_guard<std::mutex> lock(database->mutex); database->offline = true;
        }
        require(!store.load(error) && store.ids().isEmpty(), "Database outage fell back to local state");
        GaugeTemplateDialog offline(nullptr, backend);
        waitUntil([&]() { return offline.findChild<QLabel*>("gaugeTemplateStatus")->text().contains("加载失败"); });
        require(!offline.findChild<QPushButton*>("addGaugeTemplate")->isEnabled(), "Offline writes enabled");
        {
            std::lock_guard<std::mutex> lock(database->mutex); database->offline = false;
        }
        // File access exists only in the explicitly invoked one-time importer.
        QTemporaryDir folder(QDir::currentPath() + "/template-test-XXXXXX"); require(folder.isValid(), "Migration directory failed");
        const auto path = folder.path() + "/legacy.json"; QFile legacy(path);
        require(legacy.open(QIODevice::WriteOnly), "Legacy fixture open failed");
        legacy.write(QJsonDocument(QJsonObject{{"profiles", QJsonArray{sparse, second}},
            {"pointer_detector_global_config", QJsonObject{{"pointerMinWidth", 5}}}}).toJson()); legacy.close();
        const auto migrated = materializeLegacyGaugeTemplates(path);
        require(migrated.size() == 2 && migrated[0]["pointerMinWidth"].toDouble() == 5 &&
                migrated[0]["pointerMaxWidth"].toDouble() == GaugeProfile().pointerMaxWidth &&
                !migrated[0].contains("parent_model_path"), "Migration lost resolved global parameters");
        manager.show(); app.processEvents(); QDir().mkpath("test_outputs");
        require(manager.grab().save(QDir::currentPath() + "/test_outputs/gauge_template_manager.png"), "Preview failed");
        std::cout << "Database-only templates, optimistic revisions, async GUI, usage protection and one-time migration passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
