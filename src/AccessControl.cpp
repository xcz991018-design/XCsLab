#include "UiTheme.hpp"
#include "AccessControl.hpp"
#include "MySqlRepository.hpp"
#include "DatabaseConnectionDialog.hpp"
#include <QDialog>
#include <QFormLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QComboBox>
#include <QPushButton>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QFutureWatcher>
#include <QEventLoop>
#include <QtConcurrent>
#include <QDialogButtonBox>
#include <QPalette>
#include <QColor>

namespace {
// Explicitly style every account dialog, including the parentless login window.
// Child message boxes and combo popups inherit this palette and stylesheet.
void applyAccountDialogStyle(QDialog& dialog) {
    auto palette=dialog.palette();
    palette.setColor(QPalette::Window,QColor("#161822"));
    palette.setColor(QPalette::WindowText,QColor("#d1d5db"));
    palette.setColor(QPalette::Base,QColor("#1f2230"));
    palette.setColor(QPalette::AlternateBase,QColor("#272a3a"));
    palette.setColor(QPalette::Text,QColor("#f5f7fa"));
    palette.setColor(QPalette::Button,QColor("#272a3a"));
    palette.setColor(QPalette::ButtonText,QColor("#ffffff"));
    palette.setColor(QPalette::Highlight,QColor("#005b64"));
    palette.setColor(QPalette::HighlightedText,QColor("#ffffff"));
    palette.setColor(QPalette::ToolTipBase,QColor("#1f2230"));
    palette.setColor(QPalette::ToolTipText,QColor("#f5f7fa"));
    palette.setColor(QPalette::Disabled,QPalette::Text,QColor("#9299ab"));
    palette.setColor(QPalette::Disabled,QPalette::WindowText,QColor("#9299ab"));
    palette.setColor(QPalette::Disabled,QPalette::ButtonText,QColor("#9299ab"));
#if QT_VERSION >= QT_VERSION_CHECK(5, 12, 0)
    palette.setColor(QPalette::PlaceholderText,QColor("#9299ab"));
#endif
    dialog.setPalette(palette);
    dialog.setStyleSheet(R"(
        QDialog, QMessageBox { background-color: #161822; color: #d1d5db; }
        QWidget { color: #d1d5db; font-family: 'Segoe UI', Microsoft YaHei, sans-serif; font-size: 13px; }
        QLabel { background-color: transparent; color: #d1d5db; }
        QLineEdit, QComboBox, QListWidget {
            background-color: #1f2230; color: #f5f7fa;
            border: 1px solid #33384c; border-radius: 4px; padding: 5px;
            selection-background-color: #005b64; selection-color: #ffffff;
        }
        QLineEdit:focus, QComboBox:focus, QListWidget:focus { border-color: #00e5ff; }
        QComboBox QAbstractItemView {
            background-color: #1f2230; color: #f5f7fa; border: 1px solid #4b526b;
            selection-background-color: #005b64; selection-color: #ffffff;
        }
        QListWidget::item { padding: 7px 5px; }
        QListWidget::item:selected { background-color: #005b64; color: #ffffff; }
        QPushButton {
            background-color: #272a3a; color: #ffffff; border: 1px solid #3a3f58;
            border-radius: 4px; padding: 7px 14px; min-height: 20px;
        }
        QPushButton:hover { background-color: #35394e; border-color: #00e5ff; }
        QPushButton:pressed { background-color: #005b64; }
        QPushButton:default, QPushButton:focus { border-color: #00e5ff; }
        QPushButton:disabled, QLineEdit:disabled, QComboBox:disabled, QListWidget:disabled {
            background-color: #15161f; color: #9299ab; border-color: #272a38;
        }
        QToolTip { background-color: #1f2230; color: #f5f7fa; border: 1px solid #4b526b; }
    )");
    dialog.setStyleSheet(dialog.styleSheet() + UiTheme::scrollAndTableStyle());
}
struct LoginAvailability { bool hasAdministrator{false}; QString error; };
QString databaseWork(QWidget* owner, std::function<void()> work) {
    owner->setEnabled(false);
    QFutureWatcher<QString> watcher; QEventLoop loop;
    QObject::connect(&watcher,&QFutureWatcher<QString>::finished,&loop,&QEventLoop::quit);
    watcher.setFuture(QtConcurrent::run([work] {
        try { work(); return QString(); }
        catch(const std::exception& e) { return QString::fromUtf8(e.what()); }
    }));
    loop.exec(); owner->setEnabled(true);
    return watcher.result();
}
void error(QWidget* owner,const QString& message) { QMessageBox::warning(owner,QStringLiteral(u"账号与权限"),message); }
}
bool authenticateApplication(QWidget* parent) {
    AccessControl::beginGui();
    QDialog dialog(parent); dialog.setWindowTitle(QStringLiteral(u"检测平台登陆"));
    applyAccountDialogStyle(dialog);
    dialog.setObjectName("applicationLoginDialog");
    dialog.setWindowModality(Qt::ApplicationModal);
    auto* layout=new QFormLayout(&dialog);
    auto* hint=new QLabel(QStringLiteral(u"正在检查登录状态…"),&dialog);
    hint->setObjectName("loginStatusHint");
    auto* username=new QLineEdit(&dialog); username->setObjectName("loginUsername");
    auto* password=new QLineEdit(&dialog); password->setEchoMode(QLineEdit::Password); password->setObjectName("loginPassword");
    auto* confirmation=new QLineEdit(&dialog); confirmation->setEchoMode(QLineEdit::Password); confirmation->setObjectName("bootstrapPasswordConfirmation");
    auto* login=new QPushButton(QStringLiteral(u"登录"),&dialog); login->setObjectName("loginButton");
    auto* setup=new QPushButton(QStringLiteral(u"首次创建管理员"),&dialog); setup->setObjectName("bootstrapAdministratorButton");
    auto* connection=new QPushButton(QStringLiteral(u"数据库连接配置"),&dialog);
    layout->addRow(hint); layout->addRow(QStringLiteral(u"用户名"),username);
    layout->addRow(QStringLiteral(u"密码"),password);
    layout->addRow(QStringLiteral(u"初始化时确认密码"),confirmation);
    layout->addRow(login); layout->addRow(setup); layout->addRow(connection);
    auto* exit = new QPushButton(QStringLiteral(u"退出软件"), &dialog);
    exit->setObjectName("exitSoftwareButton");
    layout->addRow(exit);
    QObject::connect(exit, &QPushButton::clicked, &dialog, [&] { dialog.done(QDialog::Rejected); });
    QObject::connect(connection,&QPushButton::clicked,&dialog,[&] {
        bool exists=true;
        auto result=databaseWork(&dialog,[&]{exists=MySqlRepository().hasApplicationAccounts();});
        if(!result.isEmpty()) exists=false; // Unconfigured/unreachable database: allow connection repair.
        if(exists) { error(&dialog,QStringLiteral(u"已有账号，请登录后由管理员配置数据库")); return; }
        DatabaseConnectionDialog settings(&dialog);
        if (settings.exec()==QDialog::Accepted) QMessageBox::information(&dialog,QStringLiteral(u"数据库配置"),QStringLiteral(u"配置已保存，请重新启动软件后创建管理员或登录。"));
    });
    QObject::connect(setup,&QPushButton::clicked,&dialog,[&] {
        if(password->text()!=confirmation->text()) { error(&dialog,QStringLiteral(u"两次密码不一致")); return; }
        const auto user=username->text(), secret=password->text();
        auto result=databaseWork(&dialog,[=]{MySqlRepository().createApplicationAccount(user,secret,"administrator",true);});
        if(!result.isEmpty()) { error(&dialog,result); return; }
        AccessControl::login(AccessControl::Administrator,user); dialog.accept();
    });
    QObject::connect(login,&QPushButton::clicked,&dialog,[&] {
        const auto user=username->text(), secret=password->text(); QString role;
        auto result=databaseWork(&dialog,[&]{role=MySqlRepository().authenticateAccount(user,secret);});
        if(!result.isEmpty()) { error(&dialog,result); return; }
        if(role!="administrator" && role!="operator") { error(&dialog,QStringLiteral(u"无效角色")); return; }
        AccessControl::login(role=="administrator" ? AccessControl::Administrator : AccessControl::Operator,user); dialog.accept();
    });
    QObject::connect(password,&QLineEdit::returnPressed,login,&QPushButton::click);
    auto* confirmationLabel=layout->labelForField(confirmation);
    confirmation->hide(); confirmationLabel->hide(); setup->hide(); connection->hide();
    login->setEnabled(false);
    auto* availability=new QFutureWatcher<LoginAvailability>(&dialog);
    QObject::connect(availability,&QFutureWatcher<LoginAvailability>::finished,&dialog,[&] {
        const auto state=availability->result();
        const bool bootstrap=state.error.isEmpty() && !state.hasAdministrator;
        confirmation->setVisible(bootstrap); confirmationLabel->setVisible(bootstrap); setup->setVisible(bootstrap);
        connection->setVisible(bootstrap || !state.error.isEmpty());
        if(!state.error.isEmpty()) {
            hint->setText(QStringLiteral(u"无法检查数据库，请检查连接配置后重新启动软件。\n%1").arg(state.error));
            hint->setWordWrap(true);
        } else if(bootstrap) {
            hint->setText(QStringLiteral(u"首次使用请创建管理员账号，密码至少8位。"));
        } else {
            hint->hide();
        }
        login->setEnabled(state.error.isEmpty());
        username->setFocus();
        dialog.adjustSize();
    });
    availability->setFuture(QtConcurrent::run([] {
        LoginAvailability state;
        try { state.hasAdministrator=MySqlRepository().hasAdministratorAccount(); }
        catch(const std::exception& e) { state.error=QString::fromUtf8(e.what()); }
        return state;
    }));
    const auto result=dialog.exec();
    availability->waitForFinished();
    return result==QDialog::Accepted;
}
namespace {
void addAccountDialog(QWidget* parent) {
    if(!AccessControl::administrator()) return;
    QDialog dialog(parent); dialog.setWindowTitle(QStringLiteral(u"增加账号")); dialog.setObjectName("addAccountDialog");
    applyAccountDialogStyle(dialog);
    auto* layout=new QFormLayout(&dialog);
    auto* username=new QLineEdit(&dialog); username->setObjectName("newAccountUsername");
    auto* password=new QLineEdit(&dialog); password->setEchoMode(QLineEdit::Password);
    auto* confirm=new QLineEdit(&dialog); confirm->setEchoMode(QLineEdit::Password);
    auto* role=new QComboBox(&dialog); role->addItem(QStringLiteral(u"管理员"),"administrator"); role->addItem(QStringLiteral(u"操作员"),"operator");
    layout->addRow(QStringLiteral(u"账号"),username); layout->addRow(QStringLiteral(u"密码（至少8位）"),password);
    layout->addRow(QStringLiteral(u"确认密码"),confirm); layout->addRow(QStringLiteral(u"角色"),role);
    auto* buttons=new QDialogButtonBox(QDialogButtonBox::Save|QDialogButtonBox::Cancel,&dialog); layout->addRow(buttons);
    QObject::connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    QObject::connect(buttons,&QDialogButtonBox::accepted,&dialog,[&] {
        if(password->text()!=confirm->text()) { error(&dialog,QStringLiteral(u"两次密码不一致")); return; }
        const auto user=username->text(), secret=password->text(), code=role->currentData().toString();
        auto result=databaseWork(&dialog,[=]{MySqlRepository().createApplicationAccount(user,secret,code);});
        if(!result.isEmpty()) { error(&dialog,result); return; }
        dialog.accept();
    });
    dialog.exec();
}
void deleteAccountDialog(QWidget* parent) {
    if(!AccessControl::administrator()) return;
    QDialog dialog(parent); dialog.setWindowTitle(QStringLiteral(u"删除账号")); dialog.setObjectName("deleteAccountDialog"); dialog.resize(400,340);
    applyAccountDialogStyle(dialog);
    auto* layout=new QVBoxLayout(&dialog); auto* list=new QListWidget(&dialog); layout->addWidget(list);
    auto* remove=new QPushButton(QStringLiteral(u"删除选中账号"),&dialog); layout->addWidget(remove);
    auto refresh=[&] {
        QList<ApplicationAccount> accounts;
        auto result=databaseWork(&dialog,[&]{accounts=MySqlRepository().applicationAccounts();});
        if(!result.isEmpty()) { error(&dialog,result); return; }
        list->clear();
        for(const auto& account:accounts) {
            auto* item=new QListWidgetItem(account.username+" — "+(account.role=="administrator" ? QStringLiteral(u"管理员") : QStringLiteral(u"操作员")),list);
            item->setData(Qt::UserRole,account.username);
        }
    };
    QObject::connect(remove,&QPushButton::clicked,&dialog,[&] {
        if(!list->currentItem()) { error(&dialog,QStringLiteral(u"请选择要删除的账号")); return; }
        const auto user=list->currentItem()->data(Qt::UserRole).toString();
        if(QMessageBox::question(&dialog,QStringLiteral(u"删除账号"),QStringLiteral(u"确定删除账号 %1？").arg(user))!=QMessageBox::Yes) return;
        auto result=databaseWork(&dialog,[=]{MySqlRepository().deleteApplicationAccount(user);});
        if(!result.isEmpty()) error(&dialog,result);
        refresh();
    });
    refresh(); dialog.exec();
}
void changePasswordDialog(QWidget* parent) {
    QDialog dialog(parent); dialog.setWindowTitle(QStringLiteral(u"修改密码")); dialog.setObjectName("changePasswordDialog");
    applyAccountDialogStyle(dialog);
    auto* layout=new QFormLayout(&dialog);
    auto* oldPassword=new QLineEdit(&dialog); auto* password=new QLineEdit(&dialog); auto* confirm=new QLineEdit(&dialog);
    for(auto* editor:{oldPassword,password,confirm}) editor->setEchoMode(QLineEdit::Password);
    layout->addRow(QStringLiteral(u"当前账号"),new QLabel(AccessControl::username(),&dialog));
    layout->addRow(QStringLiteral(u"原密码"),oldPassword); layout->addRow(QStringLiteral(u"新密码（至少8位）"),password); layout->addRow(QStringLiteral(u"确认新密码"),confirm);
    auto* buttons=new QDialogButtonBox(QDialogButtonBox::Save|QDialogButtonBox::Cancel,&dialog); layout->addRow(buttons);
    QObject::connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::reject);
    QObject::connect(buttons,&QDialogButtonBox::accepted,&dialog,[&] {
        if(password->text()!=confirm->text()) { error(&dialog,QStringLiteral(u"两次密码不一致")); return; }
        const auto old=oldPassword->text(), secret=password->text();
        auto result=databaseWork(&dialog,[=]{MySqlRepository().changeApplicationPassword(old,secret);});
        if(!result.isEmpty()) { error(&dialog,result); return; }
        QMessageBox::information(&dialog,QStringLiteral(u"修改密码"),QStringLiteral(u"密码已修改，下次登录请使用新密码。"));
        dialog.accept();
    });
    dialog.exec();
}
}
bool manageApplicationAccounts(QWidget* parent) {
    if(AccessControl::role()!=AccessControl::Administrator && AccessControl::role()!=AccessControl::Operator) return false;
    QDialog dialog(parent); dialog.setWindowTitle(QStringLiteral(u"角色与账号管理")); dialog.setObjectName("accountManagementDialog"); dialog.resize(320,220);
    applyAccountDialogStyle(dialog);
    auto* layout=new QVBoxLayout(&dialog);
    layout->addWidget(new QLabel(QStringLiteral(u"当前账号：%1（%2）").arg(AccessControl::username(),AccessControl::administrator()?QStringLiteral(u"管理员"):QStringLiteral(u"操作员")),&dialog));
    if(AccessControl::administrator()) {
        auto* add=new QPushButton(QStringLiteral(u"增加账号"),&dialog); add->setObjectName("addAccountButton"); layout->addWidget(add);
        auto* remove=new QPushButton(QStringLiteral(u"删除账号"),&dialog); remove->setObjectName("deleteAccountButton"); layout->addWidget(remove);
        QObject::connect(add,&QPushButton::clicked,&dialog,[&]{addAccountDialog(&dialog);});
        QObject::connect(remove,&QPushButton::clicked,&dialog,[&]{deleteAccountDialog(&dialog);});
    }
    auto* password=new QPushButton(QStringLiteral(u"修改密码"),&dialog); password->setObjectName("changePasswordButton"); layout->addWidget(password);
    auto* logout=new QPushButton(QStringLiteral(u"退出登录"),&dialog); logout->setObjectName("logoutButton"); layout->addWidget(logout);
    QObject::connect(password,&QPushButton::clicked,&dialog,[&]{changePasswordDialog(&dialog);});
    QObject::connect(logout,&QPushButton::clicked,&dialog,[&] {
        QDialog confirmation(&dialog);
        confirmation.setWindowTitle(QStringLiteral(u"退出登录"));
        confirmation.setObjectName("logoutConfirmationDialog");
        applyAccountDialogStyle(confirmation);
        auto* content=new QVBoxLayout(&confirmation);
        auto* question=new QLabel(QStringLiteral(u"确定退出当前账号“%1”吗？").arg(AccessControl::username()),&confirmation);
        question->setTextFormat(Qt::PlainText);
        question->setWordWrap(true);
        content->addWidget(question);
        content->addWidget(new QLabel(QStringLiteral(u"退出后将停止当前会话任务并返回登录窗口。"),&confirmation));
        auto* buttons=new QDialogButtonBox(&confirmation);
        auto* confirm=buttons->addButton(QStringLiteral(u"确认退出"),QDialogButtonBox::AcceptRole);
        confirm->setObjectName("confirmLogoutButton");
        confirm->setAutoDefault(false);
        auto* cancel=buttons->addButton(QStringLiteral(u"取消"),QDialogButtonBox::RejectRole);
        cancel->setObjectName("cancelLogoutButton");
        cancel->setDefault(true);
        cancel->setFocus();
        content->addWidget(buttons);
        QObject::connect(buttons,&QDialogButtonBox::accepted,&confirmation,&QDialog::accept);
        QObject::connect(buttons,&QDialogButtonBox::rejected,&confirmation,&QDialog::reject);
        if(confirmation.exec()==QDialog::Accepted) dialog.accept();
    });
    return dialog.exec()==QDialog::Accepted;
}
