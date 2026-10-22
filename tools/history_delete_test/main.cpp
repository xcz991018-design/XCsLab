#include "MySqlRepository.hpp"
#include <QCoreApplication>
#include <QUuid>
#include <QRegularExpression>
#include <mysql.h>
#include <iostream>
#include <stdexcept>

static void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
static void sql(MYSQL* db, const QByteArray& statement) {
    if (mysql_real_query(db, statement.constData(), static_cast<unsigned long>(statement.size())))
        throw std::runtime_error(mysql_error(db));
}
static qint64 scalar(MYSQL* db, const QByteArray& statement) {
    sql(db,statement); auto* result=mysql_store_result(db);
    require(result!=nullptr,"query did not return rows");
    auto row=mysql_fetch_row(result);
    const auto value=row && row[0] ? QByteArray(row[0]).toLongLong() : -1;
    mysql_free_result(result); return value;
}
int main(int argc, char** argv) {
    QCoreApplication app(argc,argv);
    MYSQL* connection=nullptr;
    QString scratch;
    bool created=false;
    try {
        auto settings=DatabaseSettings::fromEnvironment();
        connection=mysql_init(nullptr); require(connection,"MySQL initialization failed");
        mysql_ssl_mode ssl=settings.tlsMode=="disabled" ? SSL_MODE_DISABLED :
            settings.tlsMode=="verify_identity" ? SSL_MODE_VERIFY_IDENTITY : SSL_MODE_REQUIRED;
        mysql_options(connection,MYSQL_OPT_SSL_MODE,&ssl);
        if (!settings.caFile.isEmpty()) mysql_options(connection,MYSQL_OPT_SSL_CA,settings.caFile.toUtf8().constData());
        require(mysql_real_connect(connection,settings.host.toUtf8().constData(),settings.user.toUtf8().constData(),
            settings.password.toUtf8().constData(),nullptr,settings.port,nullptr,0)!=nullptr,"test MySQL connection failed");
        scratch="gauge_history_test_"+QUuid::createUuid().toString(QUuid::WithoutBraces).remove('-');
        require(QRegularExpression("^gauge_history_test_[0-9a-f]{32}$").match(scratch).hasMatch() &&
            scratch!=settings.database,"unsafe test database name");
        sql(connection,"CREATE DATABASE `"+scratch.toUtf8()+"` CHARACTER SET utf8mb4"); created=true;
        require(mysql_select_db(connection,scratch.toUtf8().constData())==0,"cannot select isolated test database");
        settings.database=scratch;
        MySqlRepository repository(settings); repository.checkConnection();
        sql(connection,"INSERT INTO camera_archives(camera_id,display_name,protocol,endpoint,created_at,updated_at) VALUES('fixture','fixture','ONVIF','localhost',NOW(3),NOW(3))");
        sql(connection,"INSERT INTO camera_config_versions(camera_id,version_number,calibration,recognition_profile,archive_snapshot,created_at) VALUES('fixture',1,'{}','{}','{}',NOW(3))");
        const QString a=QUuid::createUuid().toString(QUuid::WithoutBraces),
            b=QUuid::createUuid().toString(QUuid::WithoutBraces), c=QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto addTask=[&](const QString& id,const QString& parent) {
            sql(connection,"INSERT INTO inspection_history(task_id,camera_id,config_id,parent_task_id,input_kind,run_mode,processed_at,recognition_performed,recognition_success,quality_status,process_status) VALUES('"+
                id.toUtf8()+"','fixture',1,"+(parent.isEmpty()?QByteArray("NULL"):"'"+parent.toUtf8()+"'")+",'historical_image','evidence',NOW(3),1,1,'trusted','done')");
        };
        addTask(a,{}); addTask(b,a); addTask(c,{});
        sql(connection,"INSERT INTO image_blobs(mime_type,pixel_width,pixel_height,sha256,byte_size,image_bytes,created_at) VALUES('image/jpeg',1,1,REPEAT('a',64),3,'abc',NOW(3)),('image/jpeg',1,1,REPEAT('b',64),4,'abcd',NOW(3))");
        const auto addImage=[&](const QString& id,int image,const char* kind) {
            sql(connection,"INSERT INTO inspection_evidence(task_id,evidence_kind,storage_backend,image_id,sha256,byte_size,availability,created_at) SELECT '"+
                id.toUtf8()+"','"+kind+"','mysql',image_id,sha256,byte_size,'available',NOW(3) FROM image_blobs WHERE image_id="+QByteArray::number(image));
        };
        addImage(a,1,"original"); addImage(a,2,"rectified"); addImage(b,1,"original"); addImage(c,1,"original");
        sql(connection,"INSERT INTO reading_reviews(task_id,reviewer_id,decision,reviewed_at) VALUES('"+a.toUtf8()+"','fixture','confirmed',NOW(3))");
        bool blocked=false;
        try { repository.deleteHistory({a}); } catch (const std::exception&) { blocked=true; }
        require(blocked && scalar(connection,"SELECT COUNT(*) FROM inspection_history")==3 &&
            scalar(connection,"SELECT COUNT(*) FROM inspection_evidence")==4,"unselected child must protect parent and roll back");
        blocked=false;
        try { repository.deleteHistory({b,QUuid::createUuid().toString(QUuid::WithoutBraces)}); }
        catch (const std::exception&) { blocked=true; }
        require(blocked && scalar(connection,"SELECT COUNT(*) FROM inspection_history")==3,"missing selection must not partially delete");
        const auto first=repository.deleteHistory({a,b});
        require(first.tasks==2 && first.images==1 && first.imageBytes==4,"delete parent+child and only exclusive image");
        require(scalar(connection,"SELECT COUNT(*) FROM inspection_history")==1 &&
            scalar(connection,"SELECT COUNT(*) FROM image_blobs")==1 &&
            scalar(connection,"SELECT COUNT(*) FROM reading_reviews")==0,"shared image and unrelated record must survive");
        const auto last=repository.deleteHistory({c});
        require(last.tasks==1 && last.images==1 && last.imageBytes==3,"last image reference must reclaim shared blob");
        require(scalar(connection,"SELECT COUNT(*) FROM camera_archives")==1 &&
            scalar(connection,"SELECT COUNT(*) FROM camera_config_versions")==1,"camera archive and calibration must survive");
        // Cross-page selection must use device/time filters, not the 50-row page.
        QStringList bulk;
        for (int i=0;i<625;++i) {
            auto id=QUuid::createUuid().toString(QUuid::WithoutBraces);
            bulk.append(id); addTask(id,{});
        }
        sql(connection,"UPDATE inspection_history SET processed_at='2026-01-02 12:00:00'");
        const auto old=QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto upper=QUuid::createUuid().toString(QUuid::WithoutBraces);
        addTask(old,{}); addTask(upper,{});
        sql(connection,"UPDATE inspection_history SET processed_at='2026-01-01 00:00:00' WHERE task_id='"+old.toUtf8()+"'");
        sql(connection,"UPDATE inspection_history SET processed_at='2026-01-03 00:00:00' WHERE task_id='"+upper.toUtf8()+"'");
        sql(connection,"INSERT INTO camera_archives(camera_id,display_name,protocol,endpoint,created_at,updated_at) VALUES('other','other','ONVIF','localhost',NOW(3),NOW(3))");
        sql(connection,"INSERT INTO camera_config_versions(camera_id,version_number,calibration,recognition_profile,archive_snapshot,created_at) VALUES('other',1,'{}','{}','{}',NOW(3))");
        const auto other=QUuid::createUuid().toString(QUuid::WithoutBraces);
        sql(connection,"INSERT INTO inspection_history(task_id,camera_id,config_id,input_kind,run_mode,processed_at,recognition_performed,recognition_success,quality_status,process_status) VALUES('"+other.toUtf8()+"','other',2,'historical_image','evidence','2026-01-02 12:00:00',1,1,'trusted','done')");
        const QDateTime from(QDate(2026,1,2),QTime(0,0),Qt::UTC), until(QDate(2026,1,3),QTime(0,0),Qt::UTC);
        require(repository.history("fixture",from,until,0).size()==50,"history page must still be bounded");
        const auto selection=repository.historyTaskIds("fixture",from,until);
        require(selection.size()==625 && !selection.contains(other) && !selection.contains(old) && !selection.contains(upper),
            "all pages must respect device and half-open time range");
        const auto late=QUuid::createUuid().toString(QUuid::WithoutBraces); addTask(late,{});
        sql(connection,"UPDATE inspection_history SET processed_at='2026-01-02 12:00:00' WHERE task_id='"+late.toUtf8()+"'");
        const auto bulkDeleted=repository.deleteHistory(selection);
        require(bulkDeleted.tasks==625 && scalar(connection,"SELECT COUNT(*) FROM inspection_history")==4,
            "bulk deletion must span pages, preserve excluded records and freeze confirmation IDs");
        require(repository.historyTaskIds("fixture",from,until)==QStringList{late} &&
            repository.historyTaskIds("",from,until).size()==2,"blank device filter must include all devices");
        bool invalid=false;
        try { repository.historyTaskIds("",until,from); } catch(const std::exception&) { invalid=true; }
        require(invalid,"invalid time range must be rejected");
        std::cout<<"PASS: 625 records across pages, device/time boundaries, confirmation snapshot, all-device filter\n";
        sql(connection,"DROP DATABASE `"+scratch.toUtf8()+"`"); created=false;
        mysql_close(connection); connection=nullptr;
        std::cout<<"PASS: transactional history deletion, child protection, shared images, reviews, archives and calibration\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr<<e.what()<<'\n';
        if (connection && created && QRegularExpression("^gauge_history_test_[0-9a-f]{32}$").match(scratch).hasMatch()) {
            try { sql(connection,"DROP DATABASE `"+scratch.toUtf8()+"`"); }
            catch (...) { std::cerr<<"Isolated test database cleanup failed: "<<scratch.toStdString()<<'\n'; }
        }
        if (connection) mysql_close(connection);
        return 1;
    }
}
