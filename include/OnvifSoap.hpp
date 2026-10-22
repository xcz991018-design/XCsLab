#pragma once
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkProxy>
#include <QAuthenticator>
#include <QEventLoop>
#include <QTimer>
#include <QXmlStreamReader>
#include <QCryptographicHash>
#include <QDateTime>
#include <QUuid>
#include <QUrl>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <cstdlib>

namespace OnvifSoap {
inline QString escape(const QString& text) { return text.toHtmlEscaped(); }
inline QByteArray requestRaw(const QUrl& endpoint, const QString& action, const QString& body,
                          const QString& user, const QString& password, QString& error, qint64 offsetMs = 0) {
    error.clear();
    if (!endpoint.isValid() || endpoint.host().isEmpty() ||
        (endpoint.scheme() != "http" && endpoint.scheme() != "https")) {
        error = "Invalid ONVIF service address"; return {};
    }
    QString security;
    if (!user.isEmpty()) {
        const QByteArray nonce = QUuid::createUuid().toRfc4122();
        const QString created = QDateTime::currentDateTimeUtc().addMSecs(offsetMs).toString("yyyy-MM-ddTHH:mm:ss.zzzZ");
        const auto digest = QCryptographicHash::hash(nonce + created.toUtf8() + password.toUtf8(), QCryptographicHash::Sha1).toBase64();
        security = QString("<wsse:Security s:mustUnderstand=\"1\" xmlns:wsse=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd\" xmlns:wsu=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd\"><wsse:UsernameToken><wsse:Username>%1</wsse:Username><wsse:Password Type=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-username-token-profile-1.0#PasswordDigest\">%2</wsse:Password><wsse:Nonce EncodingType=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-soap-message-security-1.0#Base64Binary\">%3</wsse:Nonce><wsu:Created>%4</wsu:Created></wsse:UsernameToken></wsse:Security>")
            .arg(escape(user), QString::fromLatin1(digest), QString::fromLatin1(nonce.toBase64()), created);
    }
    const QByteArray xml = QString("<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\" xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\" xmlns:tt=\"http://www.onvif.org/ver10/schema\"><s:Header>%1</s:Header><s:Body>%2</s:Body></s:Envelope>").arg(security, body).toUtf8();
    QNetworkAccessManager manager;
    manager.setProxy(QNetworkProxy::NoProxy);
    int authAttempts = 0;
    QObject::connect(&manager, &QNetworkAccessManager::authenticationRequired,
                     [&authAttempts, &user, &password](QNetworkReply*, QAuthenticator* auth) {
        if (++authAttempts == 1) { auth->setUser(user); auth->setPassword(password); }
    });
    QNetworkRequest req(endpoint);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/soap+xml; charset=utf-8; action=\"" + action + "\"");
    req.setRawHeader("SOAPAction", action.toUtf8());
    auto* reply = manager.post(req, xml);
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    timeout.start(1800);
    loop.exec();
    if (!reply->isFinished()) { reply->abort(); error = "ONVIF request timed out"; return {}; }
    const auto data = reply->readAll();
    QXmlStreamReader reader(data);
    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isStartElement() && reader.name() == "Fault") {
            error = "ONVIF SOAP fault: check ONVIF account, device clock and service support"; return {};
        }
    }
    if (reply->error() != QNetworkReply::NoError) {
        error = (reply->error()==QNetworkReply::AuthenticationRequiredError
            ? "ONVIF authentication failed: " : "ONVIF HTTP request failed: ") + reply->errorString(); return {};
    }
    if (reader.hasError()) { error = "Invalid ONVIF XML response"; return {}; }
    return data;
}
inline QString element(const QByteArray& data, const QString& name) {
    QXmlStreamReader reader(data);
    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isStartElement() && reader.name() == name) return reader.readElementText();
    }
    return {};
}
struct ClockInfo {
    QDateTime utc;
    QString type, zone;
    bool daylight{false};
};
inline ClockInfo parseClock(const QByteArray& xml) {
    ClockInfo result;
    result.type=element(xml,"DateTimeType");
    result.zone=element(xml,"TZ");
    result.daylight=element(xml,"DaylightSavings")=="true" || element(xml,"DaylightSavings")=="1";
    QXmlStreamReader reader(xml);
    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isStartElement() && reader.name()=="UTCDateTime") {
            int year=0,month=0,day=0,hour=-1,minute=-1,second=-1;
            while (!reader.atEnd()) {
                reader.readNext();
                if (reader.isEndElement() && reader.name()=="UTCDateTime") break;
                if (!reader.isStartElement()) continue;
                const auto name=reader.name().toString();
                if (name=="Year") year=reader.readElementText().toInt();
                else if (name=="Month") month=reader.readElementText().toInt();
                else if (name=="Day") day=reader.readElementText().toInt();
                else if (name=="Hour") hour=reader.readElementText().toInt();
                else if (name=="Minute") minute=reader.readElementText().toInt();
                else if (name=="Second") second=reader.readElementText().toInt();
            }
            result.utc=QDateTime(QDate(year,month,day),QTime(hour,minute,second),Qt::UTC);
            break;
        }
    }
    return result;
}
struct OffsetEntry { qint64 offset=0; QDateTime measured; };
inline QMutex& clockMutex() { static QMutex value; return value; }
inline QHash<QString,OffsetEntry>& offsets() { static QHash<QString,OffsetEntry> value; return value; }
inline QString clockKey(const QUrl& url,const QString& user) {
    return url.scheme()+"://"+url.host().toLower()+":"+QString::number(url.port(url.scheme()=="https"?443:80))+"/"+user;
}
inline void storeOffset(const QUrl& endpoint,const QString& user,qint64 offset) {
    QMutexLocker lock(&clockMutex());
    offsets()[clockKey(endpoint,user)]={offset,QDateTime::currentDateTimeUtc()};
}
inline ClockInfo readClock(const QUrl& endpoint,const QString& user,const QString& password,QString& error) {
    const auto before=QDateTime::currentDateTimeUtc();
    auto xml=requestRaw(endpoint,"http://www.onvif.org/ver10/device/wsdl/GetSystemDateAndTime",
        "<tds:GetSystemDateAndTime/>",{}, {},error);
    if (xml.isEmpty()) {
        qint64 offset=0;
        { QMutexLocker lock(&clockMutex()); offset=offsets().value(clockKey(endpoint,user)).offset; }
        xml=requestRaw(endpoint,"http://www.onvif.org/ver10/device/wsdl/GetSystemDateAndTime",
            "<tds:GetSystemDateAndTime/>",user,password,error,offset);
    }
    const auto clock=parseClock(xml);
    if (clock.utc.isValid()) {
        const auto midpoint=before.addMSecs(before.msecsTo(QDateTime::currentDateTimeUtc())/2);
        storeOffset(endpoint,user,midpoint.msecsTo(clock.utc));
        error.clear();
    } else if (error.isEmpty()) error="Camera returned no valid UTC time";
    return clock;
}
inline QByteArray request(const QUrl& endpoint,const QString& action,const QString& body,
        const QString& user,const QString& password,QString& error) {
    OffsetEntry entry;
    { QMutexLocker lock(&clockMutex()); entry=offsets().value(clockKey(endpoint,user)); }
    if (!user.isEmpty() && (!entry.measured.isValid() || entry.measured.secsTo(QDateTime::currentDateTimeUtc())>60)) {
        QUrl device=endpoint;
        if (!action.contains("/device/wsdl/")) device.setPath("/onvif/device_service");
        QString ignored;
        readClock(device,user,password,ignored);
        QMutexLocker lock(&clockMutex());
        entry=offsets().value(clockKey(endpoint,user));
        // Cache unsupported clock queries briefly as well, avoiding repeated connection delays.
        if (!entry.measured.isValid()) offsets()[clockKey(endpoint,user)]={0,QDateTime::currentDateTimeUtc()};
    }
    auto response=requestRaw(endpoint,action,body,user,password,error,entry.offset);
    // Another client (or the connection-time synchronization) may have changed the clock.
    if (response.isEmpty() && !user.isEmpty() && (error.startsWith("ONVIF SOAP fault") || error.startsWith("ONVIF authentication failed"))) {
        QUrl device=endpoint;
        if (!action.contains("/device/wsdl/")) device.setPath("/onvif/device_service");
        QString clockError;
        if (readClock(device,user,password,clockError).utc.isValid()) {
            { QMutexLocker lock(&clockMutex()); entry=offsets().value(clockKey(endpoint,user)); }
            response=requestRaw(endpoint,action,body,user,password,error,entry.offset);
        }
    }
    return response;
}
struct ClockSyncResult {
    bool correctionAttempted{false};
    bool beforeKnown{false}, afterKnown{false};
    qint64 beforeOffsetMs{0}, afterOffsetMs{0}; // Camera UTC minus computer UTC.
    QString error;

    QString logMessage(const QString& cameraId) const {
        if (!correctionAttempted && error.isEmpty()) return {}; // Normal checks stay silent.
        if (!correctionAttempted)
            return QStringLiteral(u"设备 %1 时间检查失败：%2").arg(cameraId,error);
        const auto offset=[](bool known,qint64 value) {
            return known ? (value>=0 ? QStringLiteral("+") : QString())+QString::number(value)+QStringLiteral(" ms")
                         : QStringLiteral(u"未获取");
        };
        const auto status=error.isEmpty() ? QStringLiteral(u"已执行校时并通过验证")
                                          : QStringLiteral(u"已尝试校时但未通过验证");
        auto message=QStringLiteral(u"设备 %1 %2，校正前偏差：%3，校正后偏差：%4（设备时间－电脑时间）")
            .arg(cameraId,status,offset(beforeKnown,beforeOffsetMs),offset(afterKnown,afterOffsetMs));
        if (!error.isEmpty()) message+=QStringLiteral(u"；原因：%1").arg(error);
        return message;
    }
};
// Automatic correction may select Manual when an NTP clock is inaccurate.
inline ClockSyncResult synchronizeClockDetailed(const QUrl& endpoint,const QString& user,const QString& password, bool allowManual = true) {
    ClockSyncResult result;
    QString error;
    auto clock=readClock(endpoint,user,password,error);
    if (!clock.utc.isValid()) { result.error=error; return result; }
    result.beforeKnown=true;
    result.beforeOffsetMs=QDateTime::currentDateTimeUtc().msecsTo(clock.utc);
    if (std::llabs(result.beforeOffsetMs)<=5000) return result;
    if (clock.type=="NTP" && !allowManual) {
        result.error="Camera NTP clock differs from this computer; check camera NTP server and network"; return result;
    }
    if (clock.type!="Manual" && clock.type!="NTP") { result.error="Camera returned an unsupported clock mode"; return result; }
    const auto now=QDateTime::currentDateTimeUtc();
    const auto date=now.date(); const auto time=now.time();
    const auto zone=clock.zone.isEmpty()?QString():QString("<tds:TimeZone><tt:TZ>%1</tt:TZ></tds:TimeZone>").arg(escape(clock.zone));
    const auto body=QString("<tds:SetSystemDateAndTime><tds:DateTimeType>Manual</tds:DateTimeType><tds:DaylightSavings>%1</tds:DaylightSavings>%2<tds:UTCDateTime><tt:Time><tt:Hour>%3</tt:Hour><tt:Minute>%4</tt:Minute><tt:Second>%5</tt:Second></tt:Time><tt:Date><tt:Year>%6</tt:Year><tt:Month>%7</tt:Month><tt:Day>%8</tt:Day></tt:Date></tds:UTCDateTime></tds:SetSystemDateAndTime>")
        .arg(clock.daylight?"true":"false").arg(zone).arg(time.hour()).arg(time.minute()).arg(time.second()).arg(date.year()).arg(date.month()).arg(date.day());
    qint64 offset=0;
    { QMutexLocker lock(&clockMutex()); offset=offsets().value(clockKey(endpoint,user)).offset; }
    result.correctionAttempted=true;
    const auto response=requestRaw(endpoint,"http://www.onvif.org/ver10/device/wsdl/SetSystemDateAndTime",body,user,password,error,offset);
    const auto writeError=error;
    // Even a failed/timeout response may have changed the clock; refresh authentication offset.
    clock=readClock(endpoint,user,password,error);
    if (!clock.utc.isValid()) { result.error=error; return result; }
    result.afterKnown=true;
    result.afterOffsetMs=QDateTime::currentDateTimeUtc().msecsTo(clock.utc);
    if (std::llabs(result.afterOffsetMs)>5000)
        result.error=writeError.isEmpty()?QString("Camera time verification failed after synchronization"):writeError;
    else if (allowManual && clock.type!="Manual") result.error="Camera did not switch to Manual clock mode";
    else if (response.isEmpty()) result.error="Time is now correct, but camera did not acknowledge synchronization";
    return result;
}
// Compatibility for callers that only need an error status.
inline QString synchronizeClock(const QUrl& endpoint,const QString& user,const QString& password,bool allowManual = true) {
    return synchronizeClockDetailed(endpoint,user,password,allowManual).error;
}


}
