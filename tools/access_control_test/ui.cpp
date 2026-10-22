#include "AccessControl.hpp"
#include <QApplication>
#include <QDialog>
#include <QPushButton>
#include <QLineEdit>
#include <QTimer>
#include <iostream>
#include <stdexcept>

QString failure;
void check(bool value,const char* message) { if(!value && failure.isEmpty()) failure=message; }
void management(AccessControl::Role role,const QString& action,const QString& childName,bool logout=false,bool confirmLogout=true) {
    std::cerr<<"Checking "<<action.toStdString()<<" role "<<role<<std::endl;
    AccessControl::login(role,"fixture");
    QTimer::singleShot(0,[=] {
        auto* manager=qobject_cast<QDialog*>(QApplication::activeModalWidget());
        check(manager && manager->objectName()=="accountManagementDialog","management must be modal");
        if(!manager) return;
        check(bool(manager->findChild<QPushButton*>("addAccountButton"))==(role==AccessControl::Administrator),"add button role restriction");
        check(bool(manager->findChild<QPushButton*>("deleteAccountButton"))==(role==AccessControl::Administrator),"delete button role restriction");
        check(manager->findChild<QPushButton*>("changePasswordButton") && manager->findChild<QPushButton*>("logoutButton"),"both roles need password and logout");
        auto* button=manager->findChild<QPushButton*>(action);
        if(!button) { check(false,"requested action missing"); manager->reject(); return; }
        if(!childName.isEmpty()) QTimer::singleShot(0,[=] {
            auto* child=qobject_cast<QDialog*>(QApplication::activeModalWidget());
            check(child && child!=manager && child->objectName()==childName,"action must open its own dialog");
            if(child && child!=manager) child->reject();
        });
        if(action=="logoutButton") QTimer::singleShot(0,[=] {
            auto* confirmation=qobject_cast<QDialog*>(QApplication::activeModalWidget());
            check(confirmation && confirmation->objectName()=="logoutConfirmationDialog","logout must ask for confirmation");
            if(!confirmation) return;
            auto* target=confirmation->findChild<QPushButton*>(confirmLogout?"confirmLogoutButton":"cancelLogoutButton");
            check(target!=nullptr,"confirmation choice must be available");
            if(!confirmLogout && target) check(target->isDefault(),"cancel must be the default action");
            if(target) target->click();
        });
        button->click();
        if(action=="logoutButton" && !confirmLogout) {
            check(manager->isVisible(),"cancel logout must keep management open");
            check(AccessControl::role()==role && AccessControl::username()=="fixture","cancel must preserve identity");
        }
        if(!logout) manager->reject();
    });
    check(manageApplicationAccounts(nullptr)==logout,"logout request result");
}
int main(int argc,char** argv) {
    std::cerr<<"Starting Qt UI test"<<std::endl;
    QApplication app(argc,argv); app.setQuitOnLastWindowClosed(false);
    QTimer watchdog; watchdog.setSingleShot(true);
    QObject::connect(&watchdog,&QTimer::timeout,[] { std::cerr<<"FAIL: UI test timed out"<<std::endl; std::exit(2); });
    watchdog.start(10000);
    management(AccessControl::Administrator,"addAccountButton","addAccountDialog");
    management(AccessControl::Administrator,"changePasswordButton","changePasswordDialog");
    management(AccessControl::Operator,"changePasswordButton","changePasswordDialog");
    management(AccessControl::Administrator,"logoutButton",{},true);
    management(AccessControl::Operator,"logoutButton",{},true);
    management(AccessControl::Administrator,"logoutButton",{},false,false);
    management(AccessControl::Operator,"logoutButton",{},false,false);
    QTimer::singleShot(0,[] {
        auto* login=qobject_cast<QDialog*>(QApplication::activeModalWidget());
        check(login && login->objectName()=="applicationLoginDialog","login must appear when anonymous");
        if(!login) return;
        auto* exit=login->findChild<QPushButton*>("exitSoftwareButton");
        check(exit!=nullptr,"login must offer explicit software exit");
        check(login->close(),"login window close must be accepted");
        check(!login->isVisible(),"closing login must dismiss the dialog");
    });
    check(!authenticateApplication(),"closing login must exit without authenticating");
    check(AccessControl::role()==AccessControl::Anonymous && AccessControl::username().isEmpty(),"login clears previous session");
    if(!failure.isEmpty()) { std::cerr<<failure.toStdString()<<'\n'; return 1; }
    std::cout<<"PASS: role-specific management buttons, separate action dialogs, logout requests, mandatory login\n";
    return 0;
}
