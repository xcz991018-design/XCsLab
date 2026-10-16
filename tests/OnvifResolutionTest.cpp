#include "ONVIFDeviceManager.hpp"
#include "CameraIdentity.hpp"
#include "OnvifSoap.hpp"
#include <QCoreApplication>
#include <QTcpServer>
#include <QTcpSocket>
#include <QEventLoop>
#include <QTimer>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrent>
#include <iostream>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const QByteArray probe = "<Envelope><Header><Address>urn:uuid:99999999-9999-4999-8999-999999999999</Address></Header><Body><ProbeMatch><EndpointReference><Address>urn:uuid:ABCDEF12-1234-4321-8123-123456789ABC</Address></EndpointReference></ProbeMatch></Body></Envelope>";
    if (CameraIdentity::discoveryId(probe) != "abcdef12-1234-4321-8123-123456789abc" ||
        !CameraIdentity::normalize("192.168.1.1").isEmpty() ||
        !CameraIdentity::discoveryId("<Envelope><Address>urn:uuid:99999999-9999-4999-8999-999999999999</Address></Envelope>").isEmpty() ||
        !ONVIFDeviceInfo{}.serialNumber.empty()) return 1;
    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost)) { std::cerr << server.errorString().toStdString(); return 1; }
    int requests = 0;
    int timeWrites=0;
    qint64 cameraOffsetMs=-3600000;
    bool denyTimeWrite=false;
    QString clockMode="Manual";
    bool clockAuthValid=true;
    int staleRejections=0;

    bool authenticated = true;
    bool fault = false;
    bool malformed = false;
    bool tokenEscaped = true;
    bool badImage = false;
    int snapshotDownloads = 0;
    const QString endpoint = QString("http://127.0.0.1:%1/media").arg(server.serverPort());
    QObject::connect(&server, &QTcpServer::newConnection, [&]() {
        auto* socket = server.nextPendingConnection();
        auto buffer = std::make_shared<QByteArray>();
        QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        QObject::connect(socket, &QTcpSocket::readyRead, socket, [&, socket, buffer]() {
            buffer->append(socket->readAll());
            const int split = buffer->indexOf("\r\n\r\n");
            if (split < 0) return;
            int length = 0;
            for (auto line : buffer->left(split).split('\n'))
                if (line.toLower().startsWith("content-length:")) length = line.mid(15).trimmed().toInt();
            if (buffer->size() < split + 4 + length) return;
            if (buffer->startsWith("GET ")) {
                ++snapshotDownloads;
                cv::Mat image(32, 32, CV_8UC3, cv::Scalar(20, 20, 20));
                image(cv::Rect(0, 0, 16, 32)).setTo(cv::Scalar(220, 220, 220));
                std::vector<uchar> encoded;
                cv::imencode(".jpg", image, encoded);
                const QByteArray bytes = badImage ? QByteArray("not an image") : QByteArray(reinterpret_cast<const char*>(encoded.data()), int(encoded.size()));
                socket->write("HTTP/1.1 200 OK\r\nContent-Type: image/jpeg\r\nConnection: close\r\nContent-Length: " + QByteArray::number(bytes.size()) + "\r\n\r\n" + bytes);
                socket->disconnectFromHost();
                return;
            }
            const QByteArray body = buffer->mid(split + 4, length);
            const bool clockRead=body.contains("GetSystemDateAndTime");
            const bool clockWrite=body.contains("SetSystemDateAndTime");
            if (!clockRead) {
                const auto created=QDateTime::fromString(OnvifSoap::element(body,"Created"),Qt::ISODateWithMs);
                const bool valid=created.isValid() && std::llabs(created.msecsTo(QDateTime::currentDateTimeUtc().addMSecs(cameraOffsetMs)))<5000;
                if (!valid) {
                    ++staleRejections;
                    const QByteArray rejection="<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\"><s:Body><s:Fault><s:Reason>Expired timestamp</s:Reason></s:Fault></s:Body></s:Envelope>";
                    socket->write("HTTP/1.1 500 Internal Server Error\r\nConnection: close\r\nContent-Length: "+QByteArray::number(rejection.size())+"\r\n\r\n"+rejection);
                    socket->disconnectFromHost(); return;
                }
                if (clockWrite) {
                    ++timeWrites;
                    const auto written=OnvifSoap::parseClock(body).utc;
                    clockAuthValid &= written.isValid() && std::llabs(written.msecsTo(QDateTime::currentDateTimeUtc()))<5000;
                    if (!denyTimeWrite) cameraOffsetMs=0;
                } else ++requests;
            }
            if (!clockRead) authenticated &= body.contains("PasswordDigest") && body.contains("<wsse:Username>operator</wsse:Username>") && !body.contains("secret-value");
            QByteArray response;
            if (clockRead) {
                const auto now=QDateTime::currentDateTimeUtc().addMSecs(cameraOffsetMs);
                response=QString("<SystemDateAndTime><DateTimeType>%1</DateTimeType><DaylightSavings>false</DaylightSavings><TimeZone><TZ>CST-8</TZ></TimeZone><UTCDateTime><Time><Hour>%2</Hour><Minute>%3</Minute><Second>%4</Second></Time><Date><Year>%5</Year><Month>%6</Month><Day>%7</Day></Date></UTCDateTime></SystemDateAndTime>")
                    .arg(clockMode).arg(now.time().hour()).arg(now.time().minute()).arg(now.time().second()).arg(now.date().year()).arg(now.date().month()).arg(now.date().day()).toUtf8();
            }
            else if (clockWrite) {
                if (denyTimeWrite) response="<s:Fault><s:Reason>Denied</s:Reason></s:Fault>";
                else response="<SetSystemDateAndTimeResponse/>";
                if (!body.contains("<tt:TZ>CST-8</tt:TZ>") || !body.contains("<tds:DaylightSavings>false</tds:DaylightSavings>")) clockAuthValid=false;
            }
            else if (fault) response = "<s:Fault><s:Reason>Unauthorized</s:Reason></s:Fault>";
            else if (body.contains("GetCapabilities")) response = "<Capabilities><Media><XAddr>" + endpoint.toUtf8() + "</XAddr></Media></Capabilities>";
            else if (body.contains("GetProfiles")) response = "<Profiles token=\"main&amp;one\"/>";
            else if (body.contains("GetSnapshotUri")) {
                tokenEscaped &= body.contains("main&amp;one");
                response = "<Uri>" + endpoint.toUtf8() + "/snapshot</Uri>";
            }
            else if (body.contains("GetStreamUri")) {
                tokenEscaped &= body.contains("main&amp;one");
                response = malformed ? "<Uri>http://127.0.0.1/not-video</Uri>" : "<Uri>rtsp://127.0.0.1:8554/actual/path?x=1&amp;y=2</Uri>";
            }
            const QByteArray xml = "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\"><s:Body>" + response + "</s:Body></s:Envelope>";
            socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/soap+xml\r\nConnection: close\r\nContent-Length: " + QByteArray::number(xml.size()) + "\r\n\r\n" + xml);
            socket->disconnectFromHost();
        });
    });
    auto resolve = [&](bool snapshot = false, bool required = false) {
        QFutureWatcher<std::pair<std::string, std::string>> watcher;
        QEventLoop loop;
        QTimer deadline;
        deadline.setSingleShot(true);
        QObject::connect(&watcher, &QFutureWatcherBase::finished, &loop, &QEventLoop::quit);
        QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
        watcher.setFuture(QtConcurrent::run([&, snapshot]() {
            ONVIFDeviceInfo info;
            info.ipAddress = "127.0.0.1";
            info.port = server.serverPort();
            info.username = "operator";
            info.password = "secret-value";
            ONVIFDeviceManager manager(info);
            if (snapshot) {
                info.preferHttpSnapshot = true;
                info.requireHttpSnapshot = required;
                manager.setDeviceInfo(info);
                const auto result = manager.captureSnapshot();
                return std::make_pair(result.ok() && result.frame.cols == 32 && result.frame.rows == 32
                    && result.capturedAt.time_since_epoch().count() > 0 ? std::string("HTTP_OK") : std::string(), result.message);
            }
            const auto uri = manager.fetchStreamUri();
            return std::make_pair(uri, manager.lastError());
        }));
        deadline.start(7000);
        loop.exec();
        if (!watcher.isFinished()) std::exit(2);
        return watcher.result();
    };
    const auto success = resolve();
    if (success.first != "rtsp://127.0.0.1:8554/actual/path?x=1&y=2" || !success.second.empty() || requests != 3 || !authenticated || !tokenEscaped) {
        std::cerr << "Resolution failed: uri=" << success.first << " error=" << success.second << " requests=" << requests << " auth=" << authenticated << " token=" << tokenEscaped << "\n"; return 1;
    }
    auto synchronize=[&]() {
        QFutureWatcher<QString> watcher;
        QEventLoop loop;
        QObject::connect(&watcher,&QFutureWatcherBase::finished,&loop,&QEventLoop::quit);
        watcher.setFuture(QtConcurrent::run([&]() {
            QUrl device(endpoint); device.setPath("/onvif/device_service");
            return OnvifSoap::synchronizeClock(device,"operator","secret-value");
        }));
        loop.exec(); return watcher.result();
    };
    if (!clockAuthValid || !synchronize().isEmpty() || timeWrites!=1 || !clockAuthValid) {
        std::cerr<<"Offset authentication or synchronization failed\n"; return 1;
    }
    if (!synchronize().isEmpty() || timeWrites!=1) return 1; // Correct time must not be rewritten.
    cameraOffsetMs=-7200000; denyTimeWrite=true;
    if (synchronize().isEmpty() || timeWrites!=2 || !clockAuthValid) return 1;
    if (resolve().first.empty()) return 1; // A failed write still leaves compensated authentication usable.
    denyTimeWrite=false; clockMode="NTP";
    if (synchronize().isEmpty() || timeWrites!=2) return 1; // Preserve NTP.
    clockMode="Manual";
    if (!synchronize().isEmpty() || timeWrites!=3 || !clockAuthValid) return 1;
    cameraOffsetMs=-1800000;
    // Simulate an external clock change: an HTTP 500 SOAP fault refreshes the cached offset.
    if (resolve().first.empty() || !clockAuthValid || staleRejections!=1) return 1;
    const auto http = resolve(true);
    if (http.first != "HTTP_OK" || http.second != "ONVIF_HTTP" || snapshotDownloads != 1 || !authenticated || !tokenEscaped) return 1;
    badImage = true;
    const auto strict=resolve(true,true);
    if (!strict.first.empty() || strict.second!="ONVIF HTTP snapshot unavailable") return 1;
    const auto fallback = resolve(true);
    if (!fallback.first.empty() || fallback.second != "Unable to open configured camera stream") return 1;
    badImage = false;
    fault = true;
    const auto failed = resolve();
    if (!failed.first.empty() || failed.second.empty()) return 1;
    fault = false; malformed = true;
    const auto invalid = resolve();
    if (!invalid.first.empty() || invalid.second.empty()) return 1;
    std::cout << "ONVIF service discovery, profile token, WSSE, stream URI and failure handling passed\n";
    return 0;
}
