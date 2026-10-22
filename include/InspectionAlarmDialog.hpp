#pragma once
#include <QDialog>
#include "UiTheme.hpp"
#include <QTableWidget>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <functional>

struct InspectionAlarm {
    QString cameraId, location, reading, safeRange;
    bool liveAvailable{true};
    QString taskId;
};

class InspectionAlarmDialog : public QDialog {
public:
    InspectionAlarmDialog(QWidget* parent, std::function<void(const QString&)> openLive,
                          std::function<void(const QString&)> openImage = {})
        : QDialog(parent), m_openLive(std::move(openLive)), m_openImage(std::move(openImage)) {
        setModal(false);
        // Keep alarms visible even while the owning main window is hidden/minimized.
        setWindowFlag(Qt::WindowStaysOnTopHint, true);
        resize(980, 420);
        setStyleSheet("QDialog { background-color: #161822; color: #e0e0e0; }"
            "QLabel { color: #ffb74d; padding: 6px; }"
            "QPushButton { background-color: #37474f; color: white; padding: 6px 12px; border-radius: 4px; }"
            "QPushButton:hover { background-color: #00695c; }"
            "QPushButton:disabled { color: #777; background-color: #252936; }");
        setStyleSheet(styleSheet() + UiTheme::scrollAndTableStyle());
        auto* layout = new QVBoxLayout(this);
        m_summary = new QLabel(this);
        m_summary->setWordWrap(true);
        layout->addWidget(m_summary);
        m_table = new QTableWidget(this);
        UiTheme::applyTable(m_table);
        m_table->setObjectName("inspectionAlarmDevices");
        m_table->setColumnCount(6);
        m_table->setHorizontalHeaderLabels({tr("设备编号"), tr("安装位置"), tr("异常读数"), tr("安全工作范围"), tr("实时监控"), tr("巡检图片")});
        m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
        m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
        m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
        layout->addWidget(m_table);
        auto* close = new QPushButton(tr("关闭"), this);
        connect(close, &QPushButton::clicked, this, &QDialog::close);
        layout->addWidget(close, 0, Qt::AlignRight);
    }

    void setAlarms(const QList<InspectionAlarm>& alarms) {
        setWindowTitle(tr("巡检异常值提醒（%1 台设备）").arg(alarms.size()));
        m_summary->setText(tr("本轮巡检共发现 %1 台设备读数超出安全工作范围。可逐台打开实时画面核查，后台巡检继续运行。").arg(alarms.size()));
        m_table->clearContents();
        m_table->setRowCount(alarms.size());
        for (int row = 0; row < alarms.size(); ++row) {
            const auto& alarm = alarms[row];
            const QStringList values{alarm.cameraId, alarm.location, alarm.reading, alarm.safeRange};
            for (int column = 0; column < values.size(); ++column) {
                auto* cell = new QTableWidgetItem(values[column]);
                cell->setToolTip(values[column]);
                if (column == 2) cell->setForeground(QColor("#ff5252"));
                m_table->setItem(row, column, cell);
            }
            auto* live = new QPushButton(tr("查看实时画面"), m_table);
            live->setEnabled(alarm.liveAvailable);
            live->setProperty("cameraId", alarm.cameraId);
            // Capture this row's immutable archive ID, never the latest alarm.
            connect(live, &QPushButton::clicked, this, [this, id = alarm.cameraId]() { m_openLive(id); });
            m_table->setCellWidget(row, 4, live);
            auto* image = new QPushButton(tr("查看巡检图片"), m_table);
            image->setEnabled(bool(m_openImage) && !alarm.taskId.isEmpty());
            image->setToolTip(tr("查看本次异常读数对应的切点视角矫正图，与巡检报表一致。"));
            connect(image, &QPushButton::clicked, this, [this, id = alarm.taskId]() {
                if (m_openImage) m_openImage(id);
            });
            m_table->setCellWidget(row, 5, image);
        }
    }

private:
    QLabel* m_summary;
    QTableWidget* m_table;
    std::function<void(const QString&)> m_openLive;
    std::function<void(const QString&)> m_openImage;
};
