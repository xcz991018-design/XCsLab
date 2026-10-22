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
        scratch="gauge_access_test_"+QUuid::createUuid().toString(QUuid::WithoutBraces).remove('-');
        require(QRegularExpression("^gauge_access_test_[0-9a-f]{32}$").match(scratch).hasMatch() &&
            scratch!=settings.database,"unsafe test database name");
        sql(connection,"CREATE DATABASE `"+scratch.toUtf8()+"` CHARACTER SET utf8mb4"); created=true;
        require(mysql_select_db(connection,scratch.toUtf8().constData())==0,"cannot select isolated test database");
        settings.database=scratch;
        MySqlRepository repository(settings); repository.checkConnection();
        AccessControl::beginGui();
        require(!repository.hasAdministratorAccount(),"new database has no administrator account despite seeded role definition");
        require(!repository.hasApplicationAccounts(),"new database should require setup");
        repository.createApplicationAccount("admin","test-password-123","administrator",true);
        require(repository.hasAdministratorAccount(),"existing admin must switch login to returning-user mode");
        require(repository.authenticateAccount("admin","test-password-123")=="administrator","admin login");
        auto denied=[&](auto operation) { bool blocked=false; try { operation(); } catch(const std::exception&) { blocked=true; } require(blocked,"operation must be denied"); };
        denied([&]{repository.authenticateAccount("admin","incorrect");});
        denied([&]{repository.createApplicationAccount("intruder","test-password-123","administrator",true);});
        denied([&]{repository.applicationAccounts();});
        AccessControl::login(AccessControl::Administrator,"admin");
        repository.createApplicationAccount("worker","test-password-456","operator");
        require(repository.authenticateAccount("worker","test-password-456")=="operator","operator login");
        require(repository.applicationAccounts().size()==2,"accounts persisted");
        denied([&]{repository.deleteApplicationAccount("admin");});
        AccessControl::login(AccessControl::Operator,"worker");
        denied([&]{repository.deleteApplicationAccount("worker");});
        denied([&]{repository.createApplicationAccount("new","test-password-456","administrator");});
        denied([&]{repository.deleteHistory({"fixture"});});
        denied([&]{repository.deleteTemplate("fixture",1);});
        denied([&]{repository.changeApplicationPassword("incorrect","new-password-789");});
        denied([&]{repository.changeApplicationPassword("test-password-456","short");});
        repository.changeApplicationPassword("test-password-456","new-password-789");
        require(repository.authenticateAccount("worker","new-password-789")=="operator","new password must work");
        denied([&]{repository.authenticateAccount("worker","test-password-456");});
        require(repository.authenticateAccount("admin","test-password-123")=="administrator","password update must not change another account");
        AccessControl::beginGui();
        require(AccessControl::username().isEmpty() && AccessControl::role()==AccessControl::Anonymous,"logout clears identity");
        denied([&]{repository.changeApplicationPassword("new-password-789","other-password-123");});
        AccessControl::login(AccessControl::Operator,"worker");
        require(repository.maintainEvidence(QString()).isEmpty(),"operator automatic deletion must be skipped");
        // A registry snapshot omitting a stored archive must roll back, including soft deletion.
        sql(connection,"INSERT INTO camera_archives(camera_id,display_name,protocol,endpoint,created_at,updated_at) VALUES('fixture','fixture','ONVIF','localhost',NOW(3),NOW(3))");
        denied([&]{repository.saveRegistry(QJsonObject());});
        require(scalar(connection,"SELECT COUNT(*) FROM camera_archives WHERE deleted_at IS NULL")==1,"operator cannot soft-delete archive");
        AccessControl::login(AccessControl::Administrator,"admin");
        denied([&]{repository.deleteApplicationAccount("admin");});
        repository.changeApplicationPassword("test-password-123","admin-password-789");
        require(repository.authenticateAccount("admin","admin-password-789")=="administrator","admin password update");
        repository.deleteApplicationAccount("worker");
        require(repository.applicationAccounts().size()==1,"administrator deletion");
        denied([&]{repository.authenticateAccount("worker","test-password-456");});
        sql(connection,"DROP DATABASE `"+scratch.toUtf8()+"`"); created=false;
        mysql_close(connection);
        std::cout << "PASS: account persistence, password verification, bootstrap protection, last admin, operator deletion restrictions, password changes and logout identity\n";
        return 0;
    } catch(const std::exception& e) {
        std::cerr << e.what() << '\n';
        if(connection && created) { try { sql(connection,"DROP DATABASE `"+scratch.toUtf8()+"`"); } catch(...) {} }
        if(connection) mysql_close(connection);
        return 1;
    }
}
