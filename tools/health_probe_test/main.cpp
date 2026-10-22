#include "CameraHealthProbe.hpp"
#include <QCoreApplication>
#include <QEventLoop>
#include <QTcpServer>
#include <iostream>
#include <stdexcept>

static void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

static void respond(QTcpServer& server) {
    QObject::connect(&server,&QTcpServer::newConnection,&server,[&server] {
        while(server.hasPendingConnections()) {
            auto* socket=server.nextPendingConnection();
            QObject::connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
            QObject::connect(socket,&QTcpSocket::readyRead,socket,[socket] {
                const auto request=socket->readAll();
                socket->write(request.startsWith("OPTIONS ") ? "RTSP/1.0 200 OK\r\nCSeq: 1\r\n\r\n" :
                    "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                socket->disconnectFromHost();
            });
        }
    });
}

static bool probe(const QList<CameraHealthEndpoint>& endpoints, int timeoutMs = 100, QStringList* descriptions = nullptr) {
    QEventLoop loop;
    QObject owner;
    bool result = false;
    int completed = 0;
    auto* socket = startCameraHealthProbe(endpoints, &owner, [&](bool online) {
        result = online; ++completed; loop.quit();
    }, timeoutMs, [descriptions](const CameraHealthAttempt& attempt) {
        if(descriptions) descriptions->append(attempt.description());
    });
    QObject::connect(socket, QOverload<QAbstractSocket::SocketError>::of(&QTcpSocket::error), &owner,
        [socket](QAbstractSocket::SocketError code) {
            std::cerr << "Socket error " << int(code) << ": " << socket->errorString().toStdString() << "\n";
        });
    QTimer::singleShot((endpoints.size() + 1) * timeoutMs + 500, &loop, &QEventLoop::quit);
    loop.exec();
    socket->abort();
    require(completed == 1, "probe must complete exactly once");
    return result;
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    try {
        // Reproduce the startup failure with an unsuitable inherited HTTP proxy.
        // The shared camera probe must still connect directly to its endpoint.
        QNetworkProxy::setApplicationProxy(QNetworkProxy(QNetworkProxy::HttpProxy, "127.0.0.1", 9));
        if (argc == 4) {
            bool onvifOk = false, rtspOk = false;
            const int onvif = QString::fromLocal8Bit(argv[2]).toInt(&onvifOk);
            const int rtsp = QString::fromLocal8Bit(argv[3]).toInt(&rtspOk);
            require(onvifOk && rtspOk && onvif > 0 && onvif <= 65535 && rtsp > 0 && rtsp <= 65535,
                    "usage: health_probe_test [host onvif-port rtsp-port]");
            require(probe({{QString::fromLocal8Bit(argv[1]), static_cast<quint16>(onvif)},
                           {QString::fromLocal8Bit(argv[1]), static_cast<quint16>(rtsp), CameraHealthProtocol::Rtsp}}, 2000),
                    "registered camera is unreachable");
            std::cout << "PASS: registered camera reachable through shared heartbeat probe\n";
            return 0;
        }
        QTcpServer available, closed;
        respond(available);
        require(available.listen(QHostAddress::LocalHost, 0), "loopback listener unavailable");
        require(closed.listen(QHostAddress::LocalHost, 0), "loopback port unavailable");
        const auto closedPort = closed.serverPort(); closed.close();
        const CameraHealthEndpoint up{"127.0.0.1", available.serverPort()};
        const CameraHealthEndpoint down{"127.0.0.1", closedPort};
        require(probe({up, down}), "ONVIF-only camera must remain online");
        require(probe({down, up}), "RTSP fallback must remain online");
        require(!probe({down, down}), "both services unavailable must fail");
        require(!probe({}), "empty endpoint list must fail once");

        // Reproduce a TUN proxy: TCP connects, but no device responds at the application layer.
        QTcpServer silentProxy; require(silentProxy.listen(QHostAddress::LocalHost,0),"silent proxy fixture");
        QStringList diagnoses;
        require(!probe({{"127.0.0.1",silentProxy.serverPort()}},100,&diagnoses),"TCP-only TUN connection must not count as a live camera");
        require(diagnoses.size()==1 && diagnoses[0].contains("127.0.0.1:"+QString::number(silentProxy.serverPort())) &&
            diagnoses[0].contains("HTTP") && diagnoses[0].contains(QStringLiteral(u"TCP已连接")),
            "timeout diagnostics must identify actual endpoint, protocol and connected-without-response cause");
        QTcpServer gatewayError; require(gatewayError.listen(QHostAddress::LocalHost,0),"gateway fixture");
        QObject::connect(&gatewayError,&QTcpServer::newConnection,&gatewayError,[&] {
            auto* socket=gatewayError.nextPendingConnection();
            QObject::connect(socket,&QTcpSocket::readyRead,socket,[socket] {
                socket->readAll(); socket->write("HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\n\r\n");socket->disconnectFromHost();
            });
            QObject::connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
        });
        diagnoses.clear();
        require(!probe({{"127.0.0.1",gatewayError.serverPort()}},100,&diagnoses),"proxy gateway errors must not count as online");
        require(diagnoses.size()==1 && diagnoses[0].contains("502"),"gateway error diagnostics must preserve status code");
        diagnoses.clear();
        QTcpServer fallbackClosed;
        require(fallbackClosed.listen(QHostAddress::LocalHost,0),"reserve fallback fixture port");
        const CameraHealthEndpoint refused{"127.0.0.1",fallbackClosed.serverPort()}; fallbackClosed.close();
        const bool fallbackOnline=probe({refused,up},100,&diagnoses);
        for(const auto& diagnostic:diagnoses) std::cerr<<"DIAGNOSTIC: "<<diagnostic.toUtf8().constData()<<"\n";
        require(fallbackOnline && diagnoses.size()==2 &&
            diagnoses[0].contains(QStringLiteral(u"失败")) && diagnoses[1].contains("200"),
            "fallback diagnostics must preserve both failed and successful endpoint results");
        require(probe({{"127.0.0.1",available.serverPort(),CameraHealthProtocol::Rtsp}}),"RTSP OPTIONS response must count as online");

        DiscoveredCameraItem camera;
        camera.protocolType = CameraProtocolType::ONVIF_RTSP_STREAM;
        camera.ipOrSn = "127.0.0.1"; camera.port = up.port;
        camera.rtspUrl = "rtsp://user:secret@stream.example:8554/live";
        auto targets = cameraHealthEndpoints(camera);
        require(targets.size() == 2 && targets[0].port == up.port &&
            targets[1].host == "stream.example" && targets[1].port == 8554,
            "ONVIF primary and explicit stream fallback must use registered endpoints");
        require(!cameraHealthEndpointKey(camera).contains("secret"), "key must not contain credentials");
        camera.rtspUrl.clear(); camera.rtspPort = camera.port;
        require(cameraHealthEndpoints(camera).size() == 2, "same port with different protocols must still be tried");

        CameraHealthSchedule schedule;
        schedule.configure("camera", "endpoint");
        bool changed = false;
        auto revision = schedule.begin("camera");
        require(schedule.complete("camera", revision, false, 1000, changed) && changed &&
            schedule.states["camera"].known && !schedule.states["camera"].online,
            "first completed startup failure must publish offline instead of staying yellow");
        revision = schedule.begin("camera");
        require(schedule.complete("camera", revision, true, 3000, changed) && changed &&
            schedule.states["camera"].online, "startup retry success must publish online");
        revision = schedule.begin("camera");
        schedule.update("camera", true, 4000, true);
        require(!schedule.complete("camera", revision, false, 4100, changed),
            "stale failure must not override newer capture/discovery evidence");
        schedule.configure("offline", "endpoint");
        revision = schedule.begin("offline");
        require(schedule.complete("offline", revision, false, 1000, changed) && changed &&
            !schedule.states["offline"].online,"startup with disconnected cable must publish red on first probe");
        revision = schedule.begin("offline");
        require(schedule.complete("offline", revision, false, 3000, changed) && !changed &&
            !schedule.states["offline"].online, "confirmed startup failures must publish offline");

        CameraHealthSchedule active;
        active.configure("active", "endpoint");
        active.update("active",true,0,true);
        require(active.states["active"].due==15000,"online heartbeat must be due after fifteen seconds");
        for (int now=100;now<15000;now+=100) active.update("active",true,now,true);
        require(active.states["active"].due==15000,"repeated live evidence must not postpone heartbeat forever");
        require(active.deferForCapture("active",16000),"first capture may grant a bounded grace period");
        const auto captureDue=active.states["active"].due;
        require(!active.deferForCapture("active",19000) && !active.deferForCapture("active",24000) &&
            active.states["active"].due==captureDue && captureDue<=21000,
            "continuous captures must not repeatedly defer offline detection");
        revision=active.begin("active");
        require(active.complete("active",revision,false,24000,changed) && changed && !active.states["active"].online,
            "loss during capture must still publish offline");

        CameraHealthSchedule unpluggedStartup;
        unpluggedStartup.configure("unplugged", "endpoint");
        auto startupRevision=unpluggedStartup.begin("unplugged");
        require(unpluggedStartup.complete("unplugged",startupRevision,probe({down,down}),0,changed) && changed &&
            unpluggedStartup.states["unplugged"].known && !unpluggedStartup.states["unplugged"].online,
            "actual failed startup probe must publish confirmed offline immediately");

        // Exercise actual TCP loss/recovery with the production probe and
        // scheduler. Advance monotonic time to deadlines instead of sleeping;
        // no discovery/manual refresh is involved at any point.
        QTcpServer recovering;
        respond(recovering);
        require(recovering.listen(QHostAddress::LocalHost, 0), "recovery listener unavailable");
        const auto recoveryPort = recovering.serverPort();
        const QList<CameraHealthEndpoint> recoveryEndpoints{{"127.0.0.1", recoveryPort}};
        CameraHealthSchedule recoverySchedule;
        recoverySchedule.configure("recovering", "endpoint");
        QList<bool> published;
        const auto check = [&](qint64 now) {
            const auto probeRevision = recoverySchedule.begin("recovering");
            bool stateChanged = false;
            if (recoverySchedule.complete("recovering", probeRevision,
                    probe(recoveryEndpoints), now, stateChanged) && stateChanged)
                published.append(recoverySchedule.states["recovering"].online);
        };
        check(0);
        require(recoverySchedule.states["recovering"].online, "initial listener must be online");
        recovering.close();
        qint64 now = recoverySchedule.states["recovering"].due;
        check(now);
        require(!recoverySchedule.states["recovering"].online &&
                recoverySchedule.states["recovering"].due - now == 30000,
                "lost device must be offline and retried after thirty seconds");
        for (int failure = 0; failure < 6; ++failure) {
            now = recoverySchedule.states["recovering"].due;
            check(now);
            require(recoverySchedule.states["recovering"].due - now == 30000,
                    "repeated offline checks must not delay reconnection detection");
        }
        require(recovering.listen(QHostAddress::LocalHost, recoveryPort), "cannot restore listener");
        check(recoverySchedule.states["recovering"].due);
        require(recoverySchedule.states["recovering"].online &&
                recoverySchedule.states["recovering"].failures == 0 &&
                published == QList<bool>({true, false, true}),
                "heartbeat must publish online/offline/online automatically without discovery");
        std::cout << "PASS: automatic online -> offline -> online recovery; fixed 30s offline retry\n";
        std::cout << "PASS: service fallback, endpoint selection, initial retry, offline confirmation, stale results\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
