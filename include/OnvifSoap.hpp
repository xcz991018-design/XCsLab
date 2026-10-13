#pragma once
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QAuthenticator>
#include <QEventLoop>
#include <QTimer>
#include <QXmlStreamReader>
#include <QCryptographicHash>
#include <QDateTime>
#include <QUuid>
#include <QUrl>

namespace OnvifSoap {
inline QString escape(const QString& text) { return text.toHtmlEscaped(); }
inline QByteArray request(const QUrl& endpoint, const QString& action, const QString& body,
                          const QString& user, const QString& password, QString& error) {
    error.clear();
    if (!endpoint.isValid() || endpoint.host().isEmpty() ||
        (endpoint.scheme() != "http" && endpoint.scheme() != "https")) {
        error = "Invalid ONVIF service address"; return {};
    }
    QString security;
    if (!user.isEmpty()) {
        const QByteArray nonce = QUuid::createUuid().toRfc4122();
        const QString created = QDateTime::currentDateTimeUtc().toString("yyyy-MM-ddTHH:mm:ss.zzzZ");
        const auto digest = QCryptographicHash::hash(nonce + created.toUtf8() + password.toUtf8(), QCryptographicHash::Sha1).toBase64();
        security = QString("<wsse:Security s:mustUnderstand=\"1\" xmlns:wsse=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd\" xmlns:wsu=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd\"><wsse:UsernameToken><wsse:Username>%1</wsse:Username><wsse:Password Type=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-username-token-profile-1.0#PasswordDigest\">%2</wsse:Password><wsse:Nonce EncodingType=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-soap-message-security-1.0#Base64Binary\">%3</wsse:Nonce><wsu:Created>%4</wsu:Created></wsse:UsernameToken></wsse:Security>")
            .arg(escape(user), QString::fromLatin1(digest), QString::fromLatin1(nonce.toBase64()), created);
    }
    const QByteArray xml = QString("<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\" xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\" xmlns:tt=\"http://www.onvif.org/ver10/schema\"><s:Header>%1</s:Header><s:Body>%2</s:Body></s:Envelope>").arg(security, body).toUtf8();
    QNetworkAccessManager manager;
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
    if (reply->error() != QNetworkReply::NoError) {
        error = "ONVIF HTTP request failed: " + reply->errorString(); return {};
    }
    QXmlStreamReader reader(data);
    while (!reader.atEnd()) {
        reader.readNext();
        if (reader.isStartElement() && reader.name() == "Fault") {
            error = "ONVIF SOAP fault: check ONVIF account, device clock and service support"; return {};
        }
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
}
