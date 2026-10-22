#include "UiTheme.hpp"
#include "DatabaseHistoryDialog.hpp"
#include "InspectionCalibration.hpp"
#include <QDateTimeEdit>
#include <QLineEdit>
#include <QTableWidget>
#include <QHeaderView>
#include <QPushButton>
#include <QLabel>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QComboBox>
#include <QScrollArea>
#include <QFutureWatcher>
#include <QMessageBox>
#include <QFileDialog>
#include <QSaveFile>
#include <QImageReader>
#include <QBuffer>
#include <QtConcurrent/QtConcurrent>

namespace {
struct PageResult { QList<StoredInspection> rows; QString error; };
struct SelectionResult { QStringList ids; QString error; };
struct DeleteResult { HistoryDeletion deleted; QString error; };
struct ImageResult { StoredImage original; QImage image; QString error; };
}
DatabaseHistoryDialog::DatabaseHistoryDialog(QWidget* parent,
    std::function<void(InspectionTask)> analyze) : QDialog(parent), m_analyze(std::move(analyze)) {
    setWindowTitle(tr("历史数据")); resize(1250,760);
    setStyleSheet(QString::fromLatin1(R"(
        QDialog { background-color: #161822; }
        QWidget { color: #d1d5db; }
        QLineEdit, QComboBox, QDateTimeEdit {
            background-color: #1f2230; color: #f5f7fa;
            border: 1px solid #33384c; border-radius: 4px; padding: 5px;
            selection-background-color: #005b64; selection-color: #ffffff;
        }
        QComboBox QAbstractItemView { background-color: #1f2230; color: #f5f7fa; }
        QPushButton { background-color: #272a3a; color: #ffffff; border: 1px solid #3a3f58; border-radius: 4px; padding: 6px 12px; }
        QPushButton:hover { background-color: #35394e; border-color: #00e5ff; }
        QPushButton:disabled { background-color: #15161f; color: #9299ab; }
    )") + UiTheme::scrollAndTableStyle());
    m_pool.setMaxThreadCount(2);
    auto* layout=new QVBoxLayout(this); auto* filters=new QHBoxLayout;
    m_camera=new QLineEdit(this); m_camera->setPlaceholderText(tr("摄像头编号，留空查询全部"));
    m_from=new QDateTimeEdit(QDateTime::currentDateTime().addDays(-7),this);
    m_until=new QDateTimeEdit(QDateTime::currentDateTime().addSecs(60),this);
    for (auto* editor:{m_from,m_until}) { editor->setCalendarPopup(true); editor->setDisplayFormat("yyyy-MM-dd HH:mm:ss"); }
    m_search=new QPushButton(tr("查询"),this);
    m_search->setObjectName("historyQueryButton");
    filters->addWidget(m_camera); filters->addWidget(new QLabel(tr("处理时间从"),this)); filters->addWidget(m_from);
    filters->addWidget(new QLabel(tr("至（不含）"),this)); filters->addWidget(m_until); filters->addWidget(m_search);
    layout->addLayout(filters);
    m_table=new QTableWidget(this);
    UiTheme::applyTable(m_table);
    m_table->setObjectName("historyTable");
    m_table->setColumnCount(11);
    m_table->setHorizontalHeaderLabels({tr("设备编号"),tr("档案名称"),tr("原始采集时间"),tr("处理时间"),tr("输入来源"),
        tr("读数"),tr("置信度"),tr("识别状态"),tr("原图"),tr("任务编号"),tr("最近人工复核")});
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows); m_table->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    layout->addWidget(m_table);
    auto* actions=new QHBoxLayout; m_status=new QLabel(this);
    m_previous=new QPushButton(tr("上一页"),this); m_next=new QPushButton(tr("下一页"),this);
    m_view=new QPushButton(tr("从数据库拉取原图复核"),this);
    m_view->setObjectName("historyOriginalButton");
    m_delete=new QPushButton(tr("删除选中记录"),this);
    m_delete->setObjectName("historyDeleteButton");
    m_delete->setEnabled(false);
    m_delete->setToolTip(tr("可按 Ctrl/Shift 多选记录；仅删除选中的数据库历史数据。"));
    connect(m_delete,&QPushButton::clicked,this,[this]{ deleteSelected(); });
    m_selectAll=new QPushButton(tr("全选当前页"),this);
    m_selectAll->setObjectName("historySelectAllButton");
    connect(m_selectAll,&QPushButton::clicked,m_table,&QTableWidget::selectAll);
    m_deleteFiltered=new QPushButton(tr("删除全部筛选结果"),this);
    m_deleteFiltered->setObjectName("historyDeleteFilteredButton");
    m_deleteFiltered->setToolTip(tr("按上方设备和处理时间范围删除全部匹配记录，包含所有分页。设备留空时包含所有设备。"));
    connect(m_deleteFiltered,&QPushButton::clicked,this,[this]{ deleteFiltered(); });

    connect(m_table,&QTableWidget::itemSelectionChanged,this,[this]{
        m_delete->setEnabled(m_search->isEnabled() && !m_table->selectionModel()->selectedRows().isEmpty());
    });
    actions->addWidget(m_status,1); actions->addWidget(m_previous); actions->addWidget(m_next); actions->addWidget(m_view); actions->addWidget(m_selectAll); actions->addWidget(m_delete); actions->addWidget(m_deleteFiltered);
    layout->addLayout(actions);
    connect(m_search,&QPushButton::clicked,this,[this]{ m_page=0; query(); });
    connect(m_previous,&QPushButton::clicked,this,[this]{ if (m_page>0) --m_page; query(); });
    connect(m_next,&QPushButton::clicked,this,[this]{ ++m_page; query(); });
    connect(m_view,&QPushButton::clicked,this,[this]{ viewOriginal(); });
    connect(m_table,&QTableWidget::cellDoubleClicked,this,[this](int,int){ viewOriginal(); });
    auto filtersChanged = [this]() {
        m_page = 0;
        m_previous->setEnabled(false); m_next->setEnabled(false);
        m_status->setText(tr("筛选条件已修改，请点击查询。"));
    };
    connect(m_camera, &QLineEdit::textEdited, this, filtersChanged);
    connect(m_from, &QDateTimeEdit::dateTimeChanged, this, filtersChanged);
    connect(m_until, &QDateTimeEdit::dateTimeChanged, this, filtersChanged);
    query();
}
DatabaseHistoryDialog::~DatabaseHistoryDialog() { m_pool.waitForDone(); }
void DatabaseHistoryDialog::query() {
    if (!m_search->isEnabled()) return;
    const auto camera=m_camera->text(); const auto from=m_from->dateTime().toUTC(), until=m_until->dateTime().toUTC();
    const auto page=m_page;
    m_search->setEnabled(false); m_previous->setEnabled(false); m_next->setEnabled(false); m_view->setEnabled(false); m_delete->setEnabled(false); m_selectAll->setEnabled(false); m_deleteFiltered->setEnabled(false);
    m_camera->setEnabled(false); m_from->setEnabled(false); m_until->setEnabled(false);
    m_table->setRowCount(0); m_rows.clear(); m_status->setText(tr("后台查询中…"));
    auto* watcher=new QFutureWatcher<PageResult>(this);
    connect(watcher,&QFutureWatcher<PageResult>::finished,this,[this,watcher]{
        const auto result=watcher->result(); watcher->deleteLater(); m_search->setEnabled(true);
        m_camera->setEnabled(true); m_from->setEnabled(true); m_until->setEnabled(true);
        m_selectAll->setEnabled(true); m_deleteFiltered->setEnabled(true);
        m_rows=result.rows; m_table->setRowCount(m_rows.size());
        for (int i=0;i<m_rows.size();++i) {
            const auto& r=m_rows[i];
            auto status=!r.errorCode.isEmpty()?r.errorCode:!r.recognitionPerformed?tr("仅采集"):
                r.success?(r.qualityStatus=="review_required"?tr("待复核"):tr("识别成功")):tr("识别失败");
            const auto safety = r.resultSnapshot["safety_assessment"].toObject();
            const bool outsideSafeRange = safety["status"].toString() == "out_of_range";
            if (outsideSafeRange) status = tr("异常：超出安全工作范围");
            const QStringList values={r.cameraId,r.device.customName,
                r.capturedAt.isValid()?r.capturedAt.toLocalTime().toString("yyyy-MM-dd HH:mm:ss.zzz"):"--",
                r.processedAt.toLocalTime().toString("yyyy-MM-dd HH:mm:ss.zzz"),r.inputKind,
                r.success?QString::number(r.value,'f',3)+" "+r.unit:"--",QString::number(r.confidence*100,'f',1)+"%",
                status,r.hasOriginal?tr("可拉取"):tr("不可用"),r.taskId,
                r.latestReview.isEmpty()?tr("未复核"):r.latestReview["reviewer"].toString()+" · "+
                    r.latestReview["decision"].toString()+(r.latestReview["value"].isDouble()?" · "+QString::number(r.latestReview["value"].toDouble(),'f',3):QString())};
            for (int j=0;j<values.size();++j) {
                auto* item=new QTableWidgetItem(values[j]); item->setToolTip(r.errorMessage); m_table->setItem(i,j,item);
                if (outsideSafeRange) {
                    item->setForeground(QColor("#ff5252")); item->setBackground(QColor("#38191f"));
                    auto font=item->font(); font.setBold(true); item->setFont(font);
                    item->setToolTip(tr("安全工作范围：[%1，%2] %3")
                        .arg(safety["safe_min"].toDouble(),0,'f',3).arg(safety["safe_max"].toDouble(),0,'f',3).arg(r.unit));
                }
            }
        }
        m_previous->setEnabled(m_page>0); m_next->setEnabled(result.error.isEmpty() && m_rows.size()==50);
        m_view->setEnabled(!m_rows.isEmpty());
        m_status->setText(result.error.isEmpty()?tr("第 %1 页 · %2 条 · 列表未加载图片内容").arg(m_page+1).arg(m_rows.size()):result.error);
    });
    watcher->setFuture(QtConcurrent::run(&m_pool,[camera,from,until,page]{
        PageResult result;
        try { result.rows=MySqlRepository().history(camera,from,until,page); }
        catch (const std::exception& e) { result.error=QString::fromUtf8(e.what()); }
        return result;
    }));
}
void DatabaseHistoryDialog::deleteSelected() {
    if (!m_delete->isEnabled() || !m_search->isEnabled()) return;
    QStringList ids, description;
    for (const auto& index : m_table->selectionModel()->selectedRows()) {
        if (index.row()<0 || index.row()>=m_rows.size()) continue;
        const auto& row=m_rows[index.row()]; ids.append(row.taskId);
        description.append(tr("设备 %1 | %2 | 任务 %3").arg(row.cameraId,
            row.processedAt.toLocalTime().toString("yyyy-MM-dd HH:mm:ss"),row.taskId));
    }
    if (ids.isEmpty()) return;
    confirmDeletion(ids,tr("当前选中的记录"),description);
}

void DatabaseHistoryDialog::deleteFiltered() {
    if (!m_search->isEnabled()) return;
    const auto camera=m_camera->text().trimmed();
    const auto from=m_from->dateTime().toUTC(), until=m_until->dateTime().toUTC();
    const auto scope=tr("设备：%1\n处理时间：%2 至 %3（不含结束时间）\n范围：全部匹配记录，包含所有分页。")
        .arg(camera.isEmpty()?tr("全部设备"):camera,from.toLocalTime().toString("yyyy-MM-dd HH:mm:ss"),until.toLocalTime().toString("yyyy-MM-dd HH:mm:ss"));
    m_search->setEnabled(false); m_previous->setEnabled(false); m_next->setEnabled(false);
    m_view->setEnabled(false); m_delete->setEnabled(false); m_selectAll->setEnabled(false); m_deleteFiltered->setEnabled(false);
    m_table->setEnabled(false); m_camera->setEnabled(false); m_from->setEnabled(false); m_until->setEnabled(false);
    m_status->setText(tr("正在统计全部筛选结果…"));
    auto* watcher=new QFutureWatcher<SelectionResult>(this);
    connect(watcher,&QFutureWatcher<SelectionResult>::finished,this,[this,watcher,scope]{
        const auto result=watcher->result(); watcher->deleteLater();
        m_search->setEnabled(true); m_table->setEnabled(true);
        m_camera->setEnabled(true); m_from->setEnabled(true); m_until->setEnabled(true);
        m_selectAll->setEnabled(true); m_deleteFiltered->setEnabled(true);
        if (!result.error.isEmpty()) QMessageBox::warning(this,tr("无法获取删除范围"),result.error);
        else if (result.ids.isEmpty()) QMessageBox::information(this,tr("没有匹配数据"),tr("当前设备和时间范围内没有历史记录。"));
        else { confirmDeletion(result.ids,scope); return; }
        query();
    });
    watcher->setFuture(QtConcurrent::run(&m_pool,[camera,from,until]{
        SelectionResult result;
        try { result.ids=MySqlRepository().historyTaskIds(camera,from,until); }
        catch (const std::exception& e) { result.error=QString::fromUtf8(e.what()); }
        return result;
    }));
}

void DatabaseHistoryDialog::confirmDeletion(const QStringList& ids, const QString& scope, const QStringList& details) {
    QMessageBox confirmation(QMessageBox::Warning,tr("确认删除历史数据"),
        tr("将永久删除选中的 %1 条数据库历史记录、关联复核记录，以及不再被其他记录共用的数据库图片。此操作无法撤销。\n本地归档文件保留，设备档案和标定配置不删除。").arg(ids.size())+"\n\n"+scope,
        QMessageBox::Yes|QMessageBox::Cancel,this);
    confirmation.setButtonText(QMessageBox::Yes,tr("永久删除"));
    confirmation.setButtonText(QMessageBox::Cancel,tr("取消"));
    confirmation.setDefaultButton(QMessageBox::Cancel);
    confirmation.setDetailedText(details.join("\n"));
    if (confirmation.exec()!=QMessageBox::Yes) { query(); return; }
    m_search->setEnabled(false); m_previous->setEnabled(false); m_next->setEnabled(false);
    m_view->setEnabled(false); m_delete->setEnabled(false); m_selectAll->setEnabled(false); m_deleteFiltered->setEnabled(false); m_table->setEnabled(false);
    m_camera->setEnabled(false); m_from->setEnabled(false); m_until->setEnabled(false);
    m_status->setText(tr("正在删除选中的历史记录…"));
    auto* watcher=new QFutureWatcher<DeleteResult>(this);
    connect(watcher,&QFutureWatcher<DeleteResult>::finished,this,[this,watcher]{
        const auto result=watcher->result(); watcher->deleteLater();
        m_search->setEnabled(true); m_table->setEnabled(true);
        m_camera->setEnabled(true); m_from->setEnabled(true); m_until->setEnabled(true);
        m_selectAll->setEnabled(true); m_deleteFiltered->setEnabled(true);
        if (!result.error.isEmpty()) {
            QMessageBox::warning(this,tr("删除未完成"),result.error);
        } else {
            QMessageBox::information(this,tr("删除完成"),tr("已删除 %1 条历史记录，清理 %2 份不再共用的数据库图片，回收图片数据 %3 MB。")
                .arg(result.deleted.tasks).arg(result.deleted.images).arg(result.deleted.imageBytes/(1024.0*1024),0,'f',2));
            m_page=0;
        }
        query();
    });
    watcher->setFuture(QtConcurrent::run(&m_pool,[ids]{
        DeleteResult result;
        try { result.deleted=MySqlRepository().deleteHistory(ids); }
        catch (const std::exception& e) { result.error=QString::fromUtf8(e.what()); }
        return result;
    }));
}

void DatabaseHistoryDialog::viewOriginal() {
    if (!m_view->isEnabled()) return;
    const int index=m_table->currentRow(); if (index<0 || index>=m_rows.size()) return;
    const auto task=m_rows[index].taskId;
    if (!m_rows[index].hasOriginal) { QMessageBox::information(this,tr("原图不可用"),tr("该任务没有可读取的数据库原图。")); return; }
    m_view->setEnabled(false); m_search->setEnabled(false); m_previous->setEnabled(false); m_next->setEnabled(false); m_delete->setEnabled(false); m_selectAll->setEnabled(false); m_deleteFiltered->setEnabled(false);
    m_camera->setEnabled(false); m_from->setEnabled(false); m_until->setEnabled(false);
    m_status->setText(tr("后台读取并校验原图…"));
    auto* watcher=new QFutureWatcher<ImageResult>(this);
    connect(watcher,&QFutureWatcher<ImageResult>::finished,this,[this,watcher]{
        const auto result=watcher->result(); watcher->deleteLater();
        m_view->setEnabled(true); m_search->setEnabled(true); m_previous->setEnabled(m_page>0); m_next->setEnabled(m_rows.size()==50);
        m_delete->setEnabled(!m_table->selectionModel()->selectedRows().isEmpty());
        m_camera->setEnabled(true); m_from->setEnabled(true); m_until->setEnabled(true);
        m_selectAll->setEnabled(true); m_deleteFiltered->setEnabled(true);
        if (!result.error.isEmpty()) { m_status->setText(result.error); return; }
        m_status->setText(tr("原图已拉取，SHA-256 校验通过")); showOriginal(result.original,result.image);
        query();
    });
    watcher->setFuture(QtConcurrent::run(&m_pool,[task]{
        ImageResult result;
        try {
            result.original=MySqlRepository().original(task);
            QBuffer buffer(&result.original.bytes); buffer.open(QIODevice::ReadOnly); QImageReader reader(&buffer);
            const auto size=reader.size();
            if (size.width()!=result.original.width || size.height()!=result.original.height ||
                qint64(size.width())*size.height()>50000000) throw std::runtime_error("ORIGINAL_DIMENSIONS_INVALID");
            result.image=reader.read(); if (result.image.isNull()) throw std::runtime_error("ORIGINAL_DECODE_FAILED");
        } catch (const std::exception& e) { result.error=QString::fromUtf8(e.what()); }
        return result;
    }));
}
void DatabaseHistoryDialog::showOriginal(const StoredImage& stored,const QImage& image) {
    const auto record=stored.inspection;
    QDialog viewer(this); viewer.resize(1080,820);
    viewer.setObjectName("databaseOriginalViewer");
    viewer.setWindowTitle(tr("数据库原图复核 | %1 | %2").arg(record.cameraId,record.taskId));
    auto* layout=new QVBoxLayout(&viewer);
    auto* identity=new QLabel(tr("设备: %1 | 采集时间: %2 | 原读数: %3 | 图片: %4 × %5")
        .arg(record.cameraId,record.capturedAt.toLocalTime().toString(Qt::ISODateWithMs),
             record.success?QString::number(record.value,'f',3)+" "+record.unit:"--")
        .arg(stored.width).arg(stored.height),&viewer); layout->addWidget(identity);
    auto* scroll=new QScrollArea(&viewer); auto* imageLabel=new QLabel;
    imageLabel->setPixmap(QPixmap::fromImage(image)); scroll->setWidget(imageLabel); layout->addWidget(scroll,1);
    imageLabel->setObjectName("databaseOriginalImage");
    auto* zoom=new QComboBox(&viewer); zoom->addItems({"25%","50%","100%","200%"}); zoom->setCurrentIndex(2);
    connect(zoom,qOverload<int>(&QComboBox::currentIndexChanged),&viewer,[imageLabel,image](int i){
        const double factor[]={0.25,0.5,1.0,2.0};
        imageLabel->setPixmap(QPixmap::fromImage(image).scaled(image.size()*factor[i],Qt::KeepAspectRatio,Qt::SmoothTransformation));
        imageLabel->adjustSize();
    }); layout->addWidget(zoom);
    auto* form=new QFormLayout; auto* reviewer=new QLineEdit(&viewer); auto* decision=new QComboBox(&viewer);
    decision->addItem(tr("确认读数"),"confirmed"); decision->addItem(tr("修正读数"),"corrected"); decision->addItem(tr("无法读数"),"unreadable");
    auto* corrected=new QLineEdit(&viewer); corrected->setEnabled(false); auto* comment=new QLineEdit(&viewer);
    reviewer->setMaxLength(128); comment->setMaxLength(4096);
    form->addRow(tr("复核人编号"),reviewer); form->addRow(tr("复核结论"),decision); form->addRow(tr("修正值（原单位）"),corrected);
    form->addRow(tr("备注"),comment); layout->addLayout(form);
    connect(decision,qOverload<int>(&QComboBox::currentIndexChanged),&viewer,[corrected](int i){ corrected->setEnabled(i==1); });
    auto* actions=new QHBoxLayout; auto* save=new QPushButton(tr("保存复核记录"),&viewer);
    auto* exportButton=new QPushButton(tr("导出数据库原图"),&viewer);
    auto* analyze=new QPushButton(tr("使用数据库原图重新分析"),&viewer);
    auto* status=new QLabel(&viewer); layout->addWidget(status);
    if (!record.latestReview.isEmpty()) status->setText(tr("最近复核: %1 · %2 · %3")
        .arg(record.latestReview["reviewer"].toString(),record.latestReview["decision"].toString(),record.latestReview["comment"].toString()));
    actions->addWidget(save); actions->addWidget(exportButton); actions->addWidget(analyze); layout->addLayout(actions);
    connect(save,&QPushButton::clicked,&viewer,[this,&viewer,save,status,reviewer,decision,corrected,comment,record]{
        const auto operatorId=reviewer->text(), verdict=decision->currentData().toString(), value=corrected->text(), note=comment->text();
        save->setEnabled(false); status->setText(tr("保存中…"));
        auto* watcher=new QFutureWatcher<QString>(&viewer);
        connect(watcher,&QFutureWatcher<QString>::finished,&viewer,[watcher,save,status]{
            const auto error=watcher->result(); watcher->deleteLater(); save->setEnabled(true);
            status->setText(error.isEmpty()?QObject::tr("复核已保存，原始算法读数保持可追溯"):error);
        });
        watcher->setFuture(QtConcurrent::run(&m_pool,[record,operatorId,verdict,value,note]{
            try { MySqlRepository().addReview(record.taskId,operatorId,verdict,value,note); return QString(); }
            catch (const std::exception& e) { return QString::fromUtf8(e.what()); }
        }));
    });
    connect(exportButton,&QPushButton::clicked,&viewer,[this,&viewer,stored,status,exportButton]{
        const auto path=QFileDialog::getSaveFileName(&viewer,tr("导出原图"),stored.inspection.taskId+".png",tr("PNG (*.png)"));
        if (path.isEmpty()) return;
        exportButton->setEnabled(false); auto* watcher=new QFutureWatcher<QString>(&viewer);
        connect(watcher,&QFutureWatcher<QString>::finished,&viewer,[watcher,status,exportButton]{
            const auto error=watcher->result(); watcher->deleteLater(); exportButton->setEnabled(true);
            status->setText(error.isEmpty()?QObject::tr("原图已导出"):error);
        });
        watcher->setFuture(QtConcurrent::run(&m_pool,[path,bytes=stored.bytes]{
            QSaveFile file(path);
            return file.open(QIODevice::WriteOnly) && file.write(bytes)==bytes.size() && file.commit()?QString():QString("EXPORT_FAILED");
        }));
    });
    analyze->setText(tr("使用历史标定和配置重新分析"));
    analyze->setObjectName("databaseReanalyzeButton");
    analyze->setToolTip(tr("直接使用该历史任务保存的标定和算法配置，不读取当前设备标定，不需要重新标定。"));
    analyze->setEnabled(canRecognizeDevice(record.device) && !record.profile.isEmpty());
    if (!analyze->isEnabled()) analyze->setToolTip(tr("该历史任务缺少有效标定或算法快照，无法直接重分析。"));
    connect(analyze,&QPushButton::clicked,&viewer,[this,&viewer,record]{
        InspectionTask task; task.device=record.device; task.inputKind=InspectionInputKind::HistoricalImage;
        task.databaseOriginalTaskId=task.parentTaskId=record.taskId; task.originalCapturedAt=record.capturedAt;
        task.useHistoricalConfiguration=true;
        task.historicalProfile=record.profile; m_analyze(task); viewer.accept();
    });
    viewer.exec();
}
