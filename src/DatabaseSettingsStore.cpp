#include "DatabaseSettingsStore.hpp"
// Keep diagnostic literals as Unicode escapes so legacy Windows build encodings
// cannot corrupt configuration errors displayed by the GUI.
#include <QStandardPaths>
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFileInfo>
#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#include <wincrypt.h>
#endif

namespace {
QByteArray protect(const QByteArray& value, bool decrypt, QString& error) {
    if (value.isEmpty()) return {};
#ifdef Q_OS_WIN
    DATA_BLOB input{static_cast<DWORD>(value.size()), reinterpret_cast<BYTE*>(const_cast<char*>(value.constData()))};
    DATA_BLOB output{};
    const auto ok = decrypt ? CryptUnprotectData(&input,nullptr,nullptr,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&output)
        : CryptProtectData(&input,L"GaugeReader MySQL",nullptr,nullptr,nullptr,CRYPTPROTECT_UI_FORBIDDEN,&output);
    if (!ok) { error = decrypt ? QStringLiteral("\u65e0\u6cd5\u89e3\u5bc6\u6570\u636e\u5e93\u5bc6\u7801\uff0c\u8bf7\u4f7f\u7528\u4fdd\u5b58\u914d\u7f6e\u65f6\u7684 Windows \u8d26\u53f7\uff0c\u6216\u91cd\u65b0\u586b\u5199\u5bc6\u7801\u3002")
        : QStringLiteral("Windows \u65e0\u6cd5\u52a0\u5bc6\u6570\u636e\u5e93\u5bc6\u7801\uff0c\u914d\u7f6e\u672a\u4fdd\u5b58\u3002"); return {}; }
    QByteArray bytes(reinterpret_cast<char*>(output.pbData),static_cast<int>(output.cbData));
    SecureZeroMemory(output.pbData,output.cbData); LocalFree(output.pbData); return bytes;
#else
    error = QStringLiteral("\u5f53\u524d\u5e73\u53f0\u5c1a\u672a\u5b9e\u73b0\u6570\u636e\u5e93\u5bc6\u7801\u5b89\u5168\u5b58\u50a8\uff0c\u914d\u7f6e\u672a\u4fdd\u5b58\u3002");
    return {};
#endif
}
}
QString DatabaseSettingsStore::defaultPath() {
    return qEnvironmentVariable("GAUGE_MYSQL_CONFIG_PATH",
        QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)+"/GaugeReader/mysql_connection.json");
}
QString DatabaseSettingsStore::validate(const DatabaseSettings& s) {
    if (!s.enabled) return {};
    if (s.host.trimmed().isEmpty() || s.database.trimmed().isEmpty() || s.user.trimmed().isEmpty())
        return QStringLiteral("\u8bf7\u586b\u5199\u670d\u52a1\u5668\u5730\u5740\u3001\u6570\u636e\u5e93\u540d\u548c\u8d26\u53f7\u3002");
    if (s.port == 0 || s.port > 65535) return QStringLiteral("\u7aef\u53e3\u5fc5\u987b\u4ecb\u4e8e 1 \u548c 65535\u3002");
    if (s.tlsMode!="required" && s.tlsMode!="verify_identity" && s.tlsMode!="disabled")
        return QStringLiteral("\u65e0\u6548\u7684\u8fde\u63a5\u52a0\u5bc6\u8bbe\u7f6e\u3002");
    if (s.tlsMode=="verify_identity" && (s.caFile.isEmpty() || !QFileInfo::exists(s.caFile)))
        return QStringLiteral("\u9a8c\u8bc1\u670d\u52a1\u5668\u8eab\u4efd\u9700\u8981\u6307\u5b9a\u6709\u6548\u7684 CA \u8bc1\u4e66\u6587\u4ef6\u3002");
    return {};
}
DatabaseSettings DatabaseSettingsStore::load(const QString& requestedPath) {
    DatabaseSettings s; s.host="127.0.0.1"; s.tlsMode="required";
    QFile file(requestedPath.isEmpty()?defaultPath():requestedPath);
    if (!file.exists()) return s;
    if (!file.open(QIODevice::ReadOnly)) { s.configurationError=QStringLiteral("\u65e0\u6cd5\u8bfb\u53d6 MySQL \u914d\u7f6e\u6587\u4ef6\u3002"); return s; }
    QJsonParseError parse;
    const auto document=QJsonDocument::fromJson(file.readAll(),&parse);
    const auto obj=document.object();
    if (parse.error!=QJsonParseError::NoError || !document.isObject() || obj.value("version").toInt()!=1) {
        s.configurationError=QStringLiteral("MySQL \u914d\u7f6e\u6587\u4ef6\u683c\u5f0f\u65e0\u6548\uff0c\u8bf7\u91cd\u65b0\u4fdd\u5b58\u914d\u7f6e\u3002"); return s;
    }
    s.enabled=obj["enabled"].toBool();
    s.host=obj["host"].toString(); s.database=obj["database"].toString(); s.user=obj["user"].toString();
    const auto port=obj["port"].toInt(); s.port=port>0 && port<=65535?static_cast<unsigned int>(port):0;
    s.tlsMode=obj["tls_mode"].toString("required"); s.caFile=obj["ca_file"].toString();
    s.configurationError=validate(s);
    const auto encrypted=obj["password_dpapi"].toString().toLatin1();
    if (!encrypted.isEmpty() && s.configurationError.isEmpty()) {
        const auto bytes=QByteArray::fromBase64(encrypted);
        if (bytes.isEmpty()) s.configurationError=QStringLiteral("\u6570\u636e\u5e93\u5bc6\u7801\u5bc6\u6587\u65e0\u6548\uff0c\u8bf7\u91cd\u65b0\u4fdd\u5b58\u914d\u7f6e\u3002");
        else s.password=QString::fromUtf8(protect(bytes,true,s.configurationError));
    }
    if (!s.configurationError.isEmpty()) s.enabled=false;
    return s;
}
QString DatabaseSettingsStore::save(const DatabaseSettings& settings,const QString& requestedPath) {
    auto error=validate(settings); if (!error.isEmpty()) return error;
    const auto encrypted=protect(settings.password.toUtf8(),false,error); if (!error.isEmpty()) return error;
    QJsonObject obj;
    obj["version"]=1; obj["enabled"]=settings.enabled; obj["host"]=settings.host.trimmed();
    obj["port"]=static_cast<int>(settings.port); obj["database"]=settings.database.trimmed(); obj["user"]=settings.user.trimmed();
    obj["tls_mode"]=settings.tlsMode; obj["ca_file"]=settings.caFile;
    obj["password_dpapi"]=QString::fromLatin1(encrypted.toBase64());
    const auto path=requestedPath.isEmpty()?defaultPath():requestedPath;
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return QStringLiteral("\u65e0\u6cd5\u521b\u5efa MySQL \u914d\u7f6e\u76ee\u5f55\u3002");
    QSaveFile file(path); const auto bytes=QJsonDocument(obj).toJson();
    if (!file.open(QIODevice::WriteOnly)) return QStringLiteral("\u65e0\u6cd5\u5199\u5165 MySQL \u914d\u7f6e\u6587\u4ef6\u3002");
    file.setPermissions(QFileDevice::ReadOwner|QFileDevice::WriteOwner);
    if (file.write(bytes)!=bytes.size() || !file.commit()) return QStringLiteral("\u4fdd\u5b58 MySQL \u914d\u7f6e\u5931\u8d25\uff0c\u539f\u914d\u7f6e\u4fdd\u6301\u4e0d\u53d8\u3002");
    return {};
}
