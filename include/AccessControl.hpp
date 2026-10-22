#pragma once
#include <QString>
#include <QList>
#include <atomic>
#include <QMutex>
#include <QMutexLocker>
#include <stdexcept>

// Process-wide identity also applies to QtConcurrent database workers.
class AccessControl {
public:
    enum Role { System, Anonymous, Administrator, Operator };
    static Role role() { return static_cast<Role>(state().load()); }
    static bool administrator() { return role()==System || role()==Administrator; }
    static void requireAdministrator() {
        if (!administrator()) throw std::runtime_error("权限不足：此操作仅允许管理员执行");
    }
    static void beginGui() { login(Anonymous); }
    static void login(Role role, const QString& username = {}) {
        QMutexLocker lock(&identityMutex());
        identity() = username.trimmed(); state().store(role);
    }
    static QString username() { QMutexLocker lock(&identityMutex()); return identity(); }
private:
    static QMutex& identityMutex() { static QMutex mutex; return mutex; }
    static QString& identity() { static QString name; return name; }
    static std::atomic<int>& state() { static std::atomic<int> value{System}; return value; }
};
struct ApplicationAccount { QString username; QString role; };
class QWidget;
bool authenticateApplication(QWidget* parent = nullptr);
// Returns true when logout was requested; the session owner drains workers before clearing identity.
bool manageApplicationAccounts(QWidget* parent);
