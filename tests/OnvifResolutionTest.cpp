#include "ONVIFDeviceManager.hpp"
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
    QTcpServer server;
    if (!server.listen(QHostAddress::LocalHost)) { std::cerr << server.errorString().toStdString(); return 1; }
    int requests = 0;
    bool authenticated = true;
    bool fault = false;
    bool malformed = false;
    bool tokenEscaped = true;
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
            ++requests;
            const QByteArray body = buffer->mid(split + 4, length);
            authenticated &= body.contains("PasswordDigest") && body.contains("<wsse:Username>operator</wsse:Username>") && !body.contains("secret-value");
            QByteArray response;
            if (fault) response = "<s:Fault><s:Reason>Unauthorized</s:Reason></s:Fault>";
            else if (body.contains("GetCapabilities")) response = "<Capabilities><Media><XAddr>" + endpoint.toUtf8() + "</XAddr></Media></Capabilities>";
            else if (body.contains("GetProfiles")) response = "<Profiles token=\"main&amp;one\"/>";
            else if (body.contains("GetStreamUri")) {
                tokenEscaped &= body.contains("main&amp;one");
                response = malformed ? "<Uri>http://127.0.0.1/not-video</Uri>" : "<Uri>rtsp://127.0.0.1:8554/actual/path?x=1&amp;y=2</Uri>";
            }
            const QByteArray xml = "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\"><s:Body>" + response + "</s:Body></s:Envelope>";
            socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/soap+xml\r\nConnection: close\r\nContent-Length: " + QByteArray::number(xml.size()) + "\r\n\r\n" + xml);
            socket->disconnectFromHost();
        });
    });
    auto resolve = [&]() {
        QFutureWatcher<std::pair<std::string, std::string>> watcher;
        QEventLoop loop;
        QTimer deadline;
        deadline.setSingleShot(true);
        QObject::connect(&watcher, &QFutureWatcherBase::finished, &loop, &QEventLoop::quit);
        QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
        watcher.setFuture(QtConcurrent::run([&]() {
            ONVIFDeviceInfo info;
            info.ipAddress = "127.0.0.1";
            info.port = server.serverPort();
            info.username = "operator";
            info.password = "secret-value";
            ONVIFDeviceManager manager(info);
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
    fault = true;
    const auto failed = resolve();
    if (!failed.first.empty() || failed.second.empty()) return 1;
    fault = false; malformed = true;
    const auto invalid = resolve();
    if (!invalid.first.empty() || invalid.second.empty()) return 1;
    std::cout << "ONVIF service discovery, profile token, WSSE, stream URI and failure handling passed\n";
    return 0;
}
