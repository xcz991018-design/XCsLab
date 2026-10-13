#include "MySqlRepository.hpp"
#include "DeviceCodec.hpp"
#include "DatabaseHistoryDialog.hpp"
#include "DatabaseConnectionDialog.hpp"
#include "DeviceConfigManager.hpp"
#include "GaugeTemplateMigration.hpp"
#include "GaugeTemplateStore.hpp"
#include "GaugeProfile.hpp"
#include <mysql.h>
#include <QApplication>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <QJsonDocument>
#include <QJsonArray>
#include <QEventLoop>
#include <QTimer>
#include <QTableWidget>
#include <QPushButton>
#include <QLabel>
#include <QUrl>
#include <QLineEdit>
#include <QSpinBox>
#include <QUuid>
#include <QTextStream>
#include <functional>
#include <stdexcept>

static void require(bool ok,const char* message) { if (!ok) throw std::runtime_error(message); }
static bool throws(const std::function<void()>& operation) {
    try { operation(); return false; } catch (const std::exception&) { return true; }
}
struct TestDatabase {
    MYSQL* connection{nullptr};
    QByteArray name;
    explicit TestDatabase() {
        require(qEnvironmentVariable("GAUGE_MYSQL_TEST_PORT")=="33379","Use the isolated test instance on port 33379 only");
        connection=mysql_init(nullptr);
        require(mysql_real_connect(connection,"127.0.0.1","root","",nullptr,33379,nullptr,0)!=nullptr,"Test server connect failed");
        name="gauge_test_"+QUuid::createUuid().toString(QUuid::WithoutBraces).remove('-').toLatin1();
        qputenv("GAUGE_MYSQL_DATABASE",name); qputenv("GAUGE_MYSQL_HOST","127.0.0.1");
        qputenv("GAUGE_MYSQL_PORT","33379"); qputenv("GAUGE_MYSQL_USER","root"); qputenv("GAUGE_MYSQL_PASSWORD","");
        qputenv("GAUGE_MYSQL_TLS_MODE","required");
        MySqlRepository().checkConnection();
        execute("USE `"+name+"`");
        require(scalar("SELECT COUNT(*) FROM information_schema.tables WHERE table_schema=DATABASE()")==8, "Automatic schema initialization failed");

    }
    ~TestDatabase() {
        if (connection) { mysql_query(connection,("DROP DATABASE "+name).constData()); mysql_close(connection); }
    }
    void execute(const QByteArray& sql) {
        if (mysql_real_query(connection,sql.constData(),static_cast<unsigned long>(sql.size())))
            throw std::runtime_error(mysql_error(connection));
        if (auto* result=mysql_store_result(connection)) mysql_free_result(result);
    }
    int scalar(const QByteArray& sql) {
        require(mysql_real_query(connection,sql.constData(),static_cast<unsigned long>(sql.size()))==0,"Scalar failed");
        auto* result=mysql_store_result(connection); auto* row=mysql_fetch_row(result);
        int value=row&&row[0]?QByteArray(row[0]).toInt():0; mysql_free_result(result); return value;
    }
};
static InspectionOutcome run(InspectionService& service,InspectionTask task) {
    QEventLoop loop; QTimer timer; timer.setSingleShot(true); InspectionOutcome result; bool done=false;
    auto signal=QObject::connect(&service,&InspectionService::taskFinished,&loop,[&](const InspectionOutcome& out){ result=out; done=true; });
    QObject::connect(&service,&InspectionService::finished,&loop,&QEventLoop::quit);
    QObject::connect(&timer,&QTimer::timeout,&loop,&QEventLoop::quit); timer.start(20000);
    require(service.start({task}),"Service start failed"); loop.exec(); QObject::disconnect(signal);
    require(done,"Service timed out"); return result;
}
int main(int argc,char** argv) {
    QApplication app(argc,argv);
    if (qEnvironmentVariable("GAUGE_MYSQL_TEST_PORT").isEmpty()) { QTextStream(stdout)<<"Skipped: isolated MySQL test server not configured\n"; return 77; }
    try {
        TestDatabase database; QTemporaryDir folder; require(folder.isValid(),"Temporary directory failed");
        qputenv("GAUGE_MYSQL_OUTBOX",(folder.path()+"/registry_outbox").toUtf8());
        MySqlRepository repository; repository.checkConnection();
        // Simulate upgrading an existing database with the original seven tables.
        database.execute("DROP TABLE gauge_templates");
        repository.checkConnection();
        require(database.scalar("SELECT COUNT(*) FROM information_schema.tables WHERE table_schema=DATABASE()")==8,
                "Existing database template upgrade failed");
        const auto imported = materializeLegacyGaugeTemplates(QString(REPO_SOURCE_DIR)+"/config/gauge_profile.json");
        require(repository.importTemplates(imported)==imported.size(), "Legacy migration failed");
        require(repository.importTemplates(imported)==0, "Repeated migration overwrote templates");
        auto custom = GaugeTemplateStore::defaults(); custom["gauge_id"] = "SQL_'quoted"; custom["max_val"] = 4;
        custom["extra"] = QJsonObject{{"preserved", true}};
        auto created = repository.saveTemplate(custom);
        require(created.revision==1 && repository.loadTemplate(created.id).definition==custom, "Database template round trip failed");
        require(throws([&]{repository.saveTemplate(custom);}), "Duplicate database template accepted");
        custom["max_val"] = 5;
        const auto updated = repository.saveTemplate(custom, created.revision);
        require(updated.revision==2 && updated.definition["extra"].isObject(), "Database template update failed");
        require(throws([&]{repository.saveTemplate(custom,created.revision);}) &&
                throws([&]{repository.deleteTemplate(created.id,created.revision);}), "Stale template revision accepted");
        repository.deleteTemplate(created.id,updated.revision);
        require(throws([&]{repository.loadTemplate(created.id);}) && throws([&]{repository.saveTemplate(custom);}),
                "Deleted template resurrected");
        require(database.scalar("SELECT COUNT(*) FROM gauge_templates WHERE deleted_at IS NOT NULL")==1, "Template deletion was not retained");
        {
            DatabaseConnectionDialog connectionDialog(nullptr,folder.path()+"/gui_connection.json");
            connectionDialog.findChild<QLineEdit*>("mysqlHost")->setText("127.0.0.1");
            connectionDialog.findChild<QSpinBox*>("mysqlPort")->setValue(33379);
            connectionDialog.findChild<QLineEdit*>("mysqlDatabase")->setText(QString::fromLatin1(database.name));
            connectionDialog.findChild<QLineEdit*>("mysqlUser")->setText("root");
            auto* testButton=connectionDialog.findChild<QPushButton*>("mysqlTestConnection");
            auto* status=connectionDialog.findChild<QLabel*>("mysqlConnectionStatus");
            auto waitForConnection=[&]{
                QEventLoop loop; QTimer pulse,timeout; timeout.setSingleShot(true); int heartbeats=0;
                QObject::connect(&pulse,&QTimer::timeout,&loop,[&]{++heartbeats;if(testButton->isEnabled())loop.quit();});
                QObject::connect(&timeout,&QTimer::timeout,&loop,&QEventLoop::quit);
                testButton->click(); require(!testButton->isEnabled(),"Connection test did not run asynchronously");
                pulse.start(1); timeout.start(10000); loop.exec();
                require(testButton->isEnabled() && heartbeats>0,"GUI blocked during connection test");
            };
            waitForConnection(); require(status->text().contains("连接成功"),"GUI connection test failed");
            connectionDialog.findChild<QLineEdit*>("mysqlUser")->setText("missing_user_"+QUuid::createUuid().toString(QUuid::WithoutBraces));
            waitForConnection(); require(status->text().contains("连接失败"),"GUI failed connection not reported");
        }
        DiscoveredCameraItem device; device.cameraId="CAM_DATABASE_1"; device.customName="测试油压表";
        device.protocolType=CameraProtocolType::ONVIF_RTSP_STREAM;
        device.rtspUrl="rtsp://operator:secret@127.0.0.1/live"; device.password="secret"; device.username="operator";
        device.substation="测试站"; device.area="一区"; device.isAutoConnect=true;
        QJsonObject registry; registry["system"]=QJsonObject{{"last_active_device_id",device.cameraId}};
        registry["devices"]=QJsonArray{deviceToJson(device,true)};
        repository.saveRegistry(registry);
        const auto usedTemplate = repository.loadTemplate(device.defaultProfileId);
        require(throws([&]{repository.deleteTemplate(usedTemplate.id,usedTemplate.revision);}), "Referenced template deleted");
        auto invalidRegistry = registry;
        auto missingTemplateDevice = deviceToJson(device,true); missingTemplateDevice["default_profile_id"] = "MISSING_TEMPLATE";
        invalidRegistry["devices"] = QJsonArray{missingTemplateDevice};
        require(throws([&]{repository.saveRegistry(invalidRegistry);}), "Registry accepted missing template");
        const auto loaded=repository.loadRegistry(); auto archive=loaded["devices"].toArray()[0].toObject();
        require(!archive.contains("password") && QUrl(archive["rtsp_url"].toString()).password().isEmpty(),"Credentials leaked into archive");
        require(archive["custom_name"].toString()==device.customName,"Archive round trip failed");

        cv::Mat pixels(48,64,CV_8UC3,cv::Scalar(17,98,205)); const auto imagePath=folder.path()+"/original.png";
        require(cv::imwrite(imagePath.toStdString(),pixels),"Image creation failed");
        QFile imageFile(imagePath); require(imageFile.open(QIODevice::ReadOnly),"Image read failed"); auto imageBytes=imageFile.readAll(); imageFile.close();
        InspectionTask task; task.taskId=QUuid::createUuid().toString(QUuid::WithoutBraces); task.device=device;
        task.inputKind=InspectionInputKind::LocalImage; task.captureOnly=true; task.localImagePath=imagePath;
        task.outputDirectory=folder.path()+"/evidence";
        InspectionOutcome out; out.taskId=task.taskId; out.cameraId=device.cameraId;
        out.capturedAt=QDateTime::currentDateTimeUtc(); out.originalPath=imagePath; out.recognitionPerformed=true;
        out.result.success=true; out.result.value=0.73; out.result.unit="MPa"; out.result.confidence=0.91;
        const auto currentTemplate = repository.loadTemplate(device.defaultProfileId);
        out.profileSnapshot=QJsonObject{{"profiles",QJsonArray{currentTemplate.definition}},
            {"_database_template_id",currentTemplate.id},{"_database_template_revision",static_cast<double>(currentTemplate.revision)}};
        auto envelope=MySqlRepository::inspectionEnvelope(task,out); repository.saveInspection(envelope); repository.saveInspection(envelope);
        require(database.scalar("SELECT COUNT(*) FROM inspection_history")==1,"Idempotency failed");
        auto original=repository.original(task.taskId);
        require(original.bytes==imageBytes && original.inspection.cameraId==device.cameraId,"Original BLOB round trip failed");
        const auto begin=QDateTime::currentDateTimeUtc().addDays(-1), end=QDateTime::currentDateTimeUtc().addDays(1);
        auto history=repository.history(device.cameraId,begin,end,0,1);
        require(history.size()==1 && history[0].success && history[0].value==0.73 && history[0].hasOriginal,"History metadata failed");
        require(repository.history("' OR 1=1 --",begin,end,0).isEmpty(),"Query parameter binding failed");
        require(repository.history(device.cameraId,begin,end,1,1).isEmpty(),"Pagination failed");
        require(throws([&]{repository.history(device.cameraId,end,begin,0);}),"Invalid dates accepted");
        repository.addReview(task.taskId,"reviewer-1","corrected","0.75","人工复核");
        require(database.scalar("SELECT COUNT(*) FROM reading_reviews")==1,"Review not saved");
        require(repository.history(device.cameraId,begin,end,0)[0].value==0.73,"Review overwrote algorithm value");
        require(throws([&]{repository.addReview(task.taskId,"","corrected","nan","");}),"Invalid review accepted");

        device.customName="档案修改后名称"; registry["devices"]=QJsonArray{deviceToJson(device,true)};
        repository.saveRegistry(registry);
        require(repository.history(device.cameraId,begin,end,0)[0].device.customName=="测试油压表","History lost frozen archive");
        require(repository.history(device.cameraId,begin,end,0)[0].latestReview["decision"].toString()=="corrected", "Latest review query failed");
        auto changedDefinition = currentTemplate.definition; changedDefinition["max_val"] = 9;
        repository.saveTemplate(changedDefinition,currentTemplate.revision);
        require(repository.importTemplates(imported)==0 && repository.loadTemplate(currentTemplate.id).definition["max_val"].toDouble()==9,
                "Migration overwrote a database edit");
        require(repository.original(out.taskId).inspection.profile==out.profileSnapshot, "Template update changed historical snapshot");
        // Exercise the actual application's authoritative DB load and asynchronous save.
        const auto registryPath=folder.path()+"/devices.json";
        QFile registryFile(registryPath); require(registryFile.open(QIODevice::WriteOnly),"Local credential file failed");
        registryFile.write(QJsonDocument(registry).toJson()); registryFile.close();
        auto& manager=DeviceConfigManager::instance();
        require(manager.load(registryPath),"Device manager database load failed");
        // A missing template in a pending registry must recover after migration,
        // without erasing the existing database archives or pending changes.
        auto pendingRegistry = registry;
        auto pendingDevice = deviceToJson(device,true);
        pendingDevice["default_profile_id"] = "RECOVERED_TEMPLATE";
        pendingRegistry["devices"] = QJsonArray{pendingDevice};
        MySqlRepository::queueRegistry(pendingRegistry);
        require(!manager.load(registryPath) && manager.lastError().contains("RECOVERED_TEMPLATE"),
                "Missing template regression was not detected");
        auto recoveredTemplate = GaugeTemplateStore::defaults(); recoveredTemplate["gauge_id"] = "RECOVERED_TEMPLATE";
        repository.saveTemplate(recoveredTemplate);
        require(manager.load(registryPath) && manager.getDevice(device.cameraId).defaultProfileId=="RECOVERED_TEMPLATE",
                "Migrated template did not unblock registry restoration");
        repository.saveRegistry(registry);
        require(manager.load(registryPath), "Registry reset failed");
        require(manager.getDevice(device.cameraId).password=="secret" && manager.getDevice(device.cameraId).username=="operator", "Local credentials were not restored");
        auto edited=manager.getDevice(device.cameraId); edited.customName="界面保存档案";
        manager.addOrUpdateDevice(edited); manager.waitForDatabaseWrites();
        require(repository.loadRegistry()["devices"].toArray()[0].toObject()["custom_name"].toString()==edited.customName,"Asynchronous archive save failed");
        // Refresh discovers a camera: automatic registration must survive a DB-backed restart.
        DiscoveredCameraItem discoveredCamera;
        discoveredCamera.cameraId = "CAM_ONVIF_192_0_2_25";
        discoveredCamera.ipOrSn = "192.0.2.25";
        discoveredCamera.protocolType = CameraProtocolType::ONVIF_RTSP_STREAM;
        discoveredCamera.customName = "新车间摄像头";
        discoveredCamera.isOnline = true;
        const auto addedCamera = manager.registerDiscoveredDevice(discoveredCamera);
        manager.waitForDatabaseWrites();
        require(repository.loadRegistry()["devices"].toArray().size()==2,"Discovered camera was not registered in DB");
        manager.registerDiscoveredDevice(discoveredCamera);
        manager.waitForDatabaseWrites();
        require(repository.loadRegistry()["devices"].toArray().size()==2,"Repeated discovery duplicated DB device");
        manager.setLastActiveDeviceId(addedCamera.cameraId);
        manager.waitForDatabaseWrites();
        require(manager.load(registryPath) && manager.hasDevice(addedCamera.cameraId),"DB restart did not restore discovered camera");
        require(manager.getLastActiveDeviceId()==addedCamera.cameraId,"DB restart lost last connected device");
        require(!manager.getDevice(addedCamera.cameraId).isOnline,"DB restart reused stale online state");
        auto bad=envelope; bad["task_id"]=QUuid::createUuid().toString(QUuid::WithoutBraces);
        bad["original_path"]=folder.path()+"/missing.png";
        require(throws([&]{repository.saveInspection(bad);}),"Missing original accepted");
        require(database.scalar("SELECT COUNT(*) FROM inspection_history")==1,"Failed evidence did not roll back transaction");

        require(QFile::remove(imagePath),"Remove local original failed");
        task.inputKind=InspectionInputKind::HistoricalImage; task.localImagePath.clear();
        task.parentTaskId=task.databaseOriginalTaskId=out.taskId; task.originalCapturedAt=out.capturedAt; task.returnImages=true;
        task.device.customName="当前档案名与历史不同";
        int factoryCalls=0; InspectionService service(nullptr,1,[&](const InspectionTask&)->CameraSourcePtr{ ++factoryCalls; return nullptr; });
        const auto reread=run(service,task);
        require(reread.errorCode.isEmpty() && reread.databaseSaved,"DB historical service failed");
        require(factoryCalls==0 && cv::norm(reread.capturedFrame,pixels,cv::NORM_INF)==0,"History used wrong image or opened camera");
        require(reread.profileSnapshot==out.profileSnapshot, "Historical reanalysis used current template");
        require(repository.original(reread.taskId).bytes==imageBytes,"Reanalysis changed original bytes");
        require(repository.original(reread.taskId).inspection.device.customName=="测试油压表", "Reanalysis did not preserve historical config identity");
        require(database.scalar("SELECT COUNT(*) FROM image_blobs")==1,"Original was duplicated");
        auto recalibrated=task; recalibrated.useHistoricalConfiguration=false;
        const auto configured=run(service,recalibrated);
        require(configured.databaseSaved && repository.original(configured.taskId).inspection.device.customName==task.device.customName,
            "Explicit updated configuration was ignored");
        require(configured.profileSnapshot["profiles"].toArray()[0].toObject()["max_val"].toDouble()==9,
                "New task did not read current database template");
        auto wrong=task; wrong.device.cameraId="CAM_WRONG";
        auto rejected=run(service,wrong); require(!rejected.errorCode.isEmpty() && rejected.capturedFrame.empty(),"Wrong camera history accepted");

        // The GUI must query metadata first, then fetch the original after a click.
        bool viewerOpened=false, imageDisplayed=false;
        {
            DatabaseHistoryDialog dialog(nullptr,[](InspectionTask){}); dialog.show();
            QEventLoop loop; QTimer probe, timeout; timeout.setSingleShot(true);
            bool clicked=false;
            QObject::connect(&probe,&QTimer::timeout,&loop,[&]{
                auto* table=dialog.findChild<QTableWidget*>("historyTable");
                auto* queryButton=dialog.findChild<QPushButton*>("historyQueryButton");
                auto* view=dialog.findChild<QPushButton*>("historyOriginalButton");
                if (!clicked && queryButton->isEnabled() && table->rowCount()>0) {
                    for (int row=0;row<table->rowCount();++row) if (table->item(row,9)->text()==out.taskId) {
                        table->selectRow(row); clicked=true; view->click(); break;
                    }
                }
                for (auto* widget:QApplication::topLevelWidgets()) if (widget->objectName()=="databaseOriginalViewer") {
                    viewerOpened=true; auto* label=widget->findChild<QLabel*>("databaseOriginalImage");
                    imageDisplayed=label && label->pixmap() && !label->pixmap()->isNull();
                    static_cast<QDialog*>(widget)->accept(); loop.quit(); break;
                }
            });
            QObject::connect(&timeout,&QTimer::timeout,&loop,&QEventLoop::quit);
            probe.start(10); timeout.start(15000); loop.exec(); dialog.close();
        }
        require(viewerOpened && imageDisplayed,"UI did not fetch and display database original");

        // A disconnected writer keeps an atomic outbox file and retries idempotently.
        task.inputKind=InspectionInputKind::LocalImage; task.databaseOriginalTaskId.clear(); task.parentTaskId.clear();
        task.localImagePath=repository.original(out.taskId).bytes.isEmpty()?QString():folder.path()+"/restored.png";
        require(cv::imwrite(task.localImagePath.toStdString(),pixels),"Restore image failed");
        qputenv("GAUGE_MYSQL_PORT","33378");
        auto pending=run(service,task);
        require(pending.databasePending && !pending.databaseSaved && !pending.databaseError.isEmpty(),"Disconnected writer lost outbox state");
        require(pending.errorCode=="TEMPLATE_DATABASE_FAILED", "Database outage silently used a local template");
        qputenv("GAUGE_MYSQL_PORT","33379");
        require(MySqlRepository::retryPending(task.outputDirectory).isEmpty(),"Outbox replay failed");
        require(repository.original(pending.taskId).bytes==imageBytes,"Outbox original changed");
        require(MySqlRepository::retryPending(task.outputDirectory).isEmpty(),"Idempotent replay failed");
        const auto queue=QDir(task.outputDirectory+"/.mysql_pending").entryList({"*.json"},QDir::Files);
        require(queue.isEmpty(),"Acknowledged outbox file retained");

        MySqlRepository::queueRegistry(registry);
        require(MySqlRepository::retryPending(QString()).isEmpty(),"Archive outbox replay failed");
        registry["devices"]=QJsonArray(); repository.saveRegistry(registry);
        require(repository.loadRegistry()["devices"].toArray().isEmpty(),"Soft deleted device visible");
        require(repository.original(out.taskId).bytes==imageBytes,"Soft deletion erased history evidence");

        database.execute("UPDATE image_blobs SET image_bytes=CONCAT(0x00,SUBSTRING(image_bytes,2))");
        require(throws([&]{repository.original(out.taskId);}),"Corrupted original passed integrity check");
        QTextStream(stdout)<<"MySQL integration passed: schema, archives, transactions, BLOB identity, history, UI, reviews, outbox and integrity\n";
        return 0;
    } catch (const std::exception& e) { QTextStream(stderr)<<e.what()<<"\n"; return 1; }
}
