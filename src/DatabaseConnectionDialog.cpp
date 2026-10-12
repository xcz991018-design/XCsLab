#include "DatabaseConnectionDialog.hpp"
#include <QLineEdit>
#include <QSpinBox>
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QFormLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrent>

DatabaseConnectionDialog::DatabaseConnectionDialog(QWidget* parent,const QString& path)
    : QDialog(parent),m_path(path.isEmpty()?DatabaseSettingsStore::defaultPath():path) {
    setWindowTitle(tr("MySQL 连接配置")); setObjectName("mysqlConnectionDialog"); resize(580,530);
    m_pool.setMaxThreadCount(1);
    const auto current=path.isEmpty()?DatabaseSettings::fromEnvironment():DatabaseSettingsStore::load(path);
    auto* layout=new QVBoxLayout(this);
    m_enabled=new QCheckBox(tr("启用 MySQL 存储"),this);
    m_enabled->setObjectName("mysqlEnabled"); m_enabled->setChecked(current.enabled || !QFileInfo::exists(m_path));
    layout->addWidget(m_enabled);
    m_form=new QWidget(this); auto* form=new QFormLayout(m_form);
    m_host=new QLineEdit(current.host,m_form); m_host->setObjectName("mysqlHost");
    m_port=new QSpinBox(m_form); m_port->setRange(1,65535); m_port->setValue(current.port?current.port:3306); m_port->setObjectName("mysqlPort");
    m_database=new QLineEdit(current.database,m_form); m_database->setObjectName("mysqlDatabase");
    m_user=new QLineEdit(current.user,m_form); m_user->setObjectName("mysqlUser");
    m_password=new QLineEdit(current.password,m_form); m_password->setObjectName("mysqlPassword"); m_password->setEchoMode(QLineEdit::Password);
    m_tls=new QComboBox(m_form);
    m_tls->addItem(tr("加密连接"),"required"); m_tls->addItem(tr("加密并验证服务器身份"),"verify_identity");
    m_tls->addItem(tr("不使用加密连接"),"disabled"); m_tls->setCurrentIndex(qMax(0,m_tls->findData(current.tlsMode)));
    m_ca=new QLineEdit(current.caFile,m_form); m_ca->setPlaceholderText(tr("验证服务器身份时填写 CA 证书路径"));
    form->addRow(tr("服务器地址"),m_host); form->addRow(tr("端口"),m_port);
    form->addRow(tr("数据库名"),m_database); form->addRow(tr("账号"),m_user); form->addRow(tr("密码"),m_password);
    form->addRow(tr("连接加密"),m_tls); form->addRow(tr("CA 证书"),m_ca); layout->addWidget(m_form);
    auto* explanation=new QLabel(tr("配置保存在当前 Windows 用户的数据目录，密码使用 Windows 用户级加密。\n"
        "保存后重启程序生效，不影响当前正在执行的采集任务。"),this);
    explanation->setWordWrap(true); layout->addWidget(explanation);
    auto* file=new QLabel(tr("配置文件: %1").arg(m_path),this); file->setWordWrap(true); file->setTextInteractionFlags(Qt::TextSelectableByMouse); layout->addWidget(file);
    if (!qEnvironmentVariable("GAUGE_MYSQL_DATABASE").isEmpty()) {
        auto* overrideMessage=new QLabel(tr("当前启动环境包含 MySQL 配置，它优先于保存的配置。保存后请关闭程序，"
            "通过不带这些环境变量的方式启动（例如双击程序）。"),this);
        overrideMessage->setWordWrap(true); layout->addWidget(overrideMessage);
    }
    m_status=new QLabel(current.configurationError,this); m_status->setObjectName("mysqlConnectionStatus");
    m_status->setTextFormat(Qt::PlainText);
    m_status->setWordWrap(true); m_status->setTextInteractionFlags(Qt::TextSelectableByMouse); layout->addWidget(m_status);
    auto* actions=new QHBoxLayout;
    m_test=new QPushButton(tr("测试连接"),this); m_test->setObjectName("mysqlTestConnection");
    m_save=new QPushButton(tr("保存配置"),this); m_save->setObjectName("mysqlSaveConfiguration");
    auto* close=new QPushButton(tr("关闭"),this);
    actions->addWidget(m_test); actions->addWidget(m_save); actions->addStretch(); actions->addWidget(close); layout->addLayout(actions);
    connect(m_test,&QPushButton::clicked,this,[this]{testConnection();});
    connect(m_save,&QPushButton::clicked,this,[this]{saveSettings();});
    connect(close,&QPushButton::clicked,this,&QDialog::reject);
}
DatabaseConnectionDialog::~DatabaseConnectionDialog() { m_pool.waitForDone(); }
DatabaseSettings DatabaseConnectionDialog::settings() const {
    DatabaseSettings s; s.enabled=m_enabled->isChecked(); s.host=m_host->text().trimmed(); s.port=m_port->value();
    s.database=m_database->text().trimmed(); s.user=m_user->text().trimmed(); s.password=m_password->text();
    s.tlsMode=m_tls->currentData().toString(); s.caFile=m_ca->text().trimmed(); return s;
}
void DatabaseConnectionDialog::setBusy(bool busy) {
    m_form->setEnabled(!busy); m_enabled->setEnabled(!busy); m_test->setEnabled(!busy); m_save->setEnabled(!busy);
}
void DatabaseConnectionDialog::testConnection() {
    auto s=settings(); s.enabled=true; const auto error=DatabaseSettingsStore::validate(s);
    if (!error.isEmpty()) { m_status->setText(error); return; }
    setBusy(true); m_status->setText(tr("正在后台测试连接…"));
    auto* watcher=new QFutureWatcher<QString>(this);
    connect(watcher,&QFutureWatcher<QString>::finished,this,[this,watcher]{
        m_status->setText(watcher->result()); watcher->deleteLater(); setBusy(false);
    });
    watcher->setFuture(QtConcurrent::run(&m_pool,[s]{
        try { return MySqlRepository(s).testConnection(); }
        catch (const std::exception& e) { return QObject::tr("连接失败: %1").arg(QString::fromUtf8(e.what())); }
    }));
}
void DatabaseConnectionDialog::saveSettings() {
    const auto s=settings(); const auto error=DatabaseSettingsStore::validate(s);
    if (!error.isEmpty()) { m_status->setText(error); return; }
    setBusy(true); m_status->setText(tr("正在保存配置…"));
    auto* watcher=new QFutureWatcher<QString>(this);
    connect(watcher,&QFutureWatcher<QString>::finished,this,[this,watcher]{
        const auto error=watcher->result(); watcher->deleteLater(); setBusy(false);
        m_status->setText(error.isEmpty()?tr("配置已保存。请关闭并重新启动程序后使用新配置。") : error);
    });
    watcher->setFuture(QtConcurrent::run(&m_pool,[s,path=m_path]{return DatabaseSettingsStore::save(s,path);}));
}
