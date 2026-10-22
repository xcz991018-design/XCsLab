#include "InspectionAlarmDialog.hpp"
#include <QApplication>
#include <iostream>
#include <stdexcept>

static void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    try {
        QStringList opened, images;
        InspectionAlarmDialog dialog(nullptr, [&](const QString& id) { opened.append(id); },
            [&](const QString& id) { images.append(id); });
        QList<InspectionAlarm> alarms{{"CAM_A", "A", "0.900 MPa", "[0.4, 0.6] MPa", true},
                                    {"CAM_B", "B", "0.100 MPa", "[0.4, 0.6] MPa", true}};
        alarms[0].taskId = "TASK_A"; alarms[1].taskId = "TASK_B";
        dialog.setAlarms(alarms);
        auto* table = dialog.findChild<QTableWidget*>("inspectionAlarmDevices");
        require(table && table->rowCount() == 2, "must list every alarm");
        require(table->item(0, 2)->text() == "0.900 MPa" &&
                table->item(1, 2)->text() == "0.100 MPa", "must preserve device-specific readings");
        qobject_cast<QPushButton*>(table->cellWidget(0, 4))->click();
        qobject_cast<QPushButton*>(table->cellWidget(1, 4))->click();
        qobject_cast<QPushButton*>(table->cellWidget(0, 5))->click();
        qobject_cast<QPushButton*>(table->cellWidget(1, 5))->click();
        require(images == QStringList({"TASK_A", "TASK_B"}), "image buttons must select the exact inspection task");
        require(opened == QStringList({"CAM_A", "CAM_B"}), "each button must open its own device");
        alarms.append({"CAM_C", "C", "1.200 MPa", "[0.4, 0.6] MPa", true});
        dialog.setAlarms(alarms);
        require(table->rowCount() == 3 && table->item(0, 0)->text() == "CAM_A",
                "new alarms must not replace earlier devices");
        dialog.close();
        dialog.show();
        qobject_cast<QPushButton*>(table->cellWidget(0, 4))->click();
        qobject_cast<QPushButton*>(table->cellWidget(2, 4))->click();
        require(opened.mid(2) == QStringList({"CAM_A", "CAM_C"}) && dialog.isVisible(),
                "earlier and latest buttons must remain independent and keep the list available");
        dialog.setAlarms({{"LOCAL", "Local", "1 MPa", "[0.4, 0.6] MPa", false}});
        require(table->rowCount() == 1 && !table->cellWidget(0, 4)->isEnabled(),
                "new rounds must replace old alarms and disable unavailable live sources");
        QWidget hiddenOwner;
        InspectionAlarmDialog backgroundAlarm(&hiddenOwner, [](const QString&) {});
        backgroundAlarm.showNormal();
        app.processEvents();
        require(!hiddenOwner.isVisible() && backgroundAlarm.isVisible() &&
                backgroundAlarm.windowFlags().testFlag(Qt::WindowStaysOnTopHint) &&
                !backgroundAlarm.isModal(),
                "background alarms must appear above a hidden owner without blocking patrol");
        backgroundAlarm.close();
        std::cout << "PASS: multiple readings, independent live buttons, accumulation, reopen, round reset\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
