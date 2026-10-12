#include "DatabaseSettingsStore.hpp"
#include "DatabaseConnectionDialog.hpp"
#include <QApplication>
#include <QTemporaryDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QCheckBox>
#include <QEventLoop>
#include <QTimer>
#include <QTextStream>
#include <stdexcept>

static void require(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
int main(int argc,char** argv) {
    QApplication app(argc,argv);
    try {
        QTemporaryDir folder; require(folder.isValid(),"Temporary folder failed");
        const auto path=folder.path()+"/config/mysql.json";
        qunsetenv("GAUGE_MYSQL_DATABASE"); qputenv("GAUGE_MYSQL_CONFIG_PATH",path.toUtf8());
        require(DatabaseSettings::fromEnvironment().enabled && DatabaseSettings::fromEnvironment().host=="127.0.0.1" && DatabaseSettings::fromEnvironment().database=="BYS_GaugeReaderDB", "Default local connection missing");
        DatabaseSettings s; s.enabled=true; s.host="127.0.0.1"; s.port=3307;
        s.database="gauge_reader"; s.user="gauge_runtime"; s.password="private-test-密钥-9374"; s.tlsMode="required";
        require(DatabaseSettingsStore::save(s,path).isEmpty(),"Encrypted save failed");
        auto loaded=DatabaseSettingsStore::load(path);
        require(loaded.enabled && loaded.configurationError.isEmpty() && loaded.host==s.host && loaded.port==s.port &&
            loaded.database==s.database && loaded.user==s.user && loaded.password==s.password,"Saved configuration round trip failed");
        QFile file(path); require(file.open(QIODevice::ReadOnly),"Settings file missing"); const auto bytes=file.readAll(); file.close();
        require(!bytes.contains(s.password.toUtf8()) && !QJsonDocument::fromJson(bytes).object().contains("password"),"Password stored in plaintext");
        auto invalid=s; invalid.port=0;
        require(!DatabaseSettingsStore::save(invalid,path).isEmpty(),"Invalid port saved");
        require(DatabaseSettingsStore::load(path).password==s.password,"Failed save destroyed previous configuration");
        require(DatabaseSettings::fromEnvironment().database=="BYS_GaugeReaderDB","Saved configuration changed active process unexpectedly");
        qputenv("GAUGE_MYSQL_DATABASE","environment_override"); qputenv("GAUGE_MYSQL_USER","env_user");
        require(DatabaseSettings::fromEnvironment().database=="environment_override","Environment override failed");
        qunsetenv("GAUGE_MYSQL_DATABASE"); qunsetenv("GAUGE_MYSQL_USER");

        {
            DatabaseConnectionDialog dialog(nullptr,path);
            auto* password=dialog.findChild<QLineEdit*>("mysqlPassword");
            require(password && password->echoMode()==QLineEdit::Password && password->text()==s.password,"Password field not masked or not restored");
            auto* database=dialog.findChild<QLineEdit*>("mysqlDatabase"); database->clear();
            dialog.findChild<QPushButton*>("mysqlTestConnection")->click();
            require(dialog.findChild<QLabel*>("mysqlConnectionStatus")->text()==QStringLiteral("\u8bf7\u586b\u5199\u670d\u52a1\u5668\u5730\u5740\u3001\u6570\u636e\u5e93\u540d\u548c\u8d26\u53f7\u3002"),"Incomplete test connection not rejected");
            database->setText("ui_saved_database"); dialog.findChild<QSpinBox*>("mysqlPort")->setValue(3310);
            auto* save=dialog.findChild<QPushButton*>("mysqlSaveConfiguration"); save->click();
            require(!save->isEnabled(),"Save did not start asynchronously");
            QEventLoop loop; QTimer poll,timeout; timeout.setSingleShot(true); bool done=false;
            QObject::connect(&poll,&QTimer::timeout,&loop,[&]{if(save->isEnabled()){done=true;loop.quit();}});
            QObject::connect(&timeout,&QTimer::timeout,&loop,&QEventLoop::quit); poll.start(1); timeout.start(5000); loop.exec();
            require(done && dialog.findChild<QLabel*>("mysqlConnectionStatus")->text().contains("已保存"),"UI did not complete save");
        }
        loaded=DatabaseSettingsStore::load(path);
        require(loaded.database=="ui_saved_database" && loaded.port==3310 && loaded.password==s.password,"UI fields not persisted");
        auto corrupt=QJsonDocument::fromJson(bytes).object(); corrupt["password_dpapi"]="aW52YWxpZA==";
        require(file.open(QIODevice::WriteOnly|QIODevice::Truncate),"Unable to test corrupt password");
        file.write(QJsonDocument(corrupt).toJson()); file.close();
        loaded=DatabaseSettingsStore::load(path);
        require(!loaded.enabled && !loaded.configurationError.isEmpty() && loaded.password.isEmpty(),"Corrupt ciphertext accepted");
        s.enabled=false; s.password.clear();
        require(DatabaseSettingsStore::save(s,path).isEmpty() && !DatabaseSettingsStore::load(path).enabled,"Disabled configuration not saved");
        QTextStream(stdout)<<"Configuration tests passed: DPAPI, validation, environment override, process freeze and asynchronous GUI save\n";
        return 0;
    } catch (const std::exception& e) { QTextStream(stderr)<<e.what()<<"\n"; return 1; }
}
