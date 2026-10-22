#pragma once
#include "AutoCameraDiscoveryService.hpp"
#include <QUrl>
#include <QTimer>
#include <QNetworkProxy>
#include <functional>
#include <memory>

enum class CameraHealthProtocol { Http, Rtsp };
struct CameraHealthEndpoint {
    QString host;
    quint16 port;
    CameraHealthProtocol protocol{CameraHealthProtocol::Http};
};

struct CameraHealthAttempt {
    CameraHealthEndpoint endpoint;
    QString actualIp;
    bool online{false};
    QString reason;
    QString description() const {
        const auto protocol=endpoint.protocol==CameraHealthProtocol::Http ? QStringLiteral("HTTP") : QStringLiteral("RTSP");
        const auto target=actualIp.isEmpty() ? endpoint.host : actualIp;
        auto result=QStringLiteral(u"%1 %2:%3 %4：%5")
            .arg(protocol,target).arg(endpoint.port).arg(online?QStringLiteral(u"成功"):QStringLiteral(u"失败"),reason);
        if (!actualIp.isEmpty() && actualIp!=endpoint.host) result+=QStringLiteral(u"（配置主机：%1）").arg(endpoint.host);
        return result;
    }
};

// ONVIF snapshots can work even when the RTSP listener is unavailable.
// Probe only the registered services; never enumerate the network or open video.
inline QList<CameraHealthEndpoint> cameraHealthEndpoints(const DiscoveredCameraItem& camera) {
    QList<CameraHealthEndpoint> endpoints;
    const auto append = [&](const QString& host, int port, CameraHealthProtocol protocol) {
        if (host.trimmed().isEmpty() || port <= 0 || port > 65535) return;
        for (const auto& endpoint : endpoints)
            if (endpoint.host == host.trimmed() && endpoint.port == port && endpoint.protocol == protocol) return;
        endpoints.append({host.trimmed(), static_cast<quint16>(port), protocol});
    };
    if (camera.protocolType == CameraProtocolType::ONVIF_RTSP_STREAM) {
        append(camera.ipOrSn, camera.port, CameraHealthProtocol::Http);
        const QUrl stream(camera.rtspUrl);
        append(stream.host().isEmpty() ? camera.ipOrSn : stream.host(),
               stream.port(camera.rtspPort > 0 ? camera.rtspPort : 554), CameraHealthProtocol::Rtsp);
    }
    return endpoints;
}

inline QString cameraHealthEndpointKey(const DiscoveredCameraItem& camera) {
    QStringList keys;
    for (const auto& endpoint : cameraHealthEndpoints(camera))
        keys.append((endpoint.protocol == CameraHealthProtocol::Http ? "http://" : "rtsp://") + endpoint.host + ":" + QString::number(endpoint.port));
    return keys.join("|");
}

// A TUN proxy can accept TCP even when the real camera is disconnected.
// Require an application-protocol response; no video, credentials or state-changing requests.
inline QTcpSocket* startCameraHealthProbe(const QList<CameraHealthEndpoint>& endpoints,
    QObject* parent, std::function<void(bool)> completed, int timeoutMs = 2000,
    std::function<void(const CameraHealthAttempt&)> attemptCompleted = {}) {
    auto* socket = new QTcpSocket(parent);
    socket->setProxy(QNetworkProxy::NoProxy);
    auto* timer = new QTimer(socket); timer->setSingleShot(true);
    const auto index = std::make_shared<int>(0);
    const auto finished = std::make_shared<bool>(false);
    const auto pending = std::make_shared<bool>(false);
    const auto connected = std::make_shared<bool>(false);
    const auto response = std::make_shared<QByteArray>();
    const auto report=[socket,endpoints,index,attemptCompleted](bool online,const QString& reason) {
        if (!attemptCompleted || *index>=endpoints.size()) return;
        CameraHealthAttempt result;
        result.endpoint=endpoints[*index];
        if (!socket->peerAddress().isNull()) result.actualIp=socket->peerAddress().toString();
        result.online=online; result.reason=reason;
        attemptCompleted(result);
    };
    const auto finish = [timer, finished, pending, completed](bool online) {
        if (*finished) return;
        *finished = true; *pending = false;
        timer->stop(); completed(online);
    };
    const auto attempt = [socket, timer, endpoints, index, pending, connected, response, timeoutMs, finish]() {
        if (*index >= endpoints.size()) { finish(false); return; }
        response->clear(); *pending = true; *connected = false;
        const auto endpoint = endpoints[*index];
        timer->start(timeoutMs);
        socket->connectToHost(endpoint.host, endpoint.port);
    };
    const auto failed = [socket, timer, index, finished, pending, attempt, report](const QString& reason) {
        if (*finished || !*pending) return;
        *pending = false; timer->stop();
        report(false,reason); // Keep the peer address before abort clears it.
        socket->abort(); ++*index;
        QTimer::singleShot(0, socket, attempt);
    };
    QObject::connect(socket, &QTcpSocket::connected, socket, [socket,endpoints,index,finished,pending,connected,failed]() {
        if (*finished || !*pending) return;
        *connected=true;
        const auto endpoint=endpoints[*index];
        QUrl address; address.setHost(endpoint.host); address.setPort(endpoint.port);
        QByteArray request;
        if (endpoint.protocol==CameraHealthProtocol::Rtsp) {
            address.setScheme("rtsp"); address.setPath("/");
            request="OPTIONS "+address.toEncoded()+" RTSP/1.0\r\nCSeq: 1\r\nUser-Agent: GaugeReaderHealth\r\n\r\n";
        } else {
            request="GET /onvif/device_service HTTP/1.1\r\nHost: "+address.authority().toUtf8()+
                "\r\nConnection: close\r\nUser-Agent: GaugeReaderHealth\r\n\r\n";
        }
        if (socket->write(request)<0) failed(QStringLiteral(u"发送探测请求失败：%1").arg(socket->errorString()));
    });
    QObject::connect(socket,&QTcpSocket::readyRead,socket,[socket,endpoints,index,finished,pending,response,finish,failed,report]() {
        if (*finished || !*pending) return;
        response->append(socket->readAll());
        const auto end=response->indexOf('\n');
        if (end<0) { if(response->size()>4096) failed(QStringLiteral(u"响应头超过4096字节，未收到有效状态行")); return; }
        const auto fields=response->left(end).trimmed().split(' ');
        const bool http=endpoints[*index].protocol==CameraHealthProtocol::Http;
        const auto protocol=http?QStringLiteral("HTTP"):QStringLiteral("RTSP");
        bool parsed=false; const int status=fields.size()>1 ? fields[1].toInt(&parsed) : 0;
        const bool version=http ? (fields.value(0)=="HTTP/1.0" || fields.value(0)=="HTTP/1.1")
                                : (fields.value(0)=="RTSP/1.0" || fields.value(0)=="RTSP/2.0");
        if (!version || !parsed) { failed(QStringLiteral(u"未收到有效 %1 状态行，响应格式或协议不匹配").arg(protocol)); return; }
        if (status>=200 && status<500) {
            const auto reason=(status==401 || status==403)
                ? QStringLiteral(u"服务已响应，要求认证或授权（%1 %2）").arg(protocol).arg(status)
                : QStringLiteral(u"收到有效 %1 %2 响应").arg(protocol).arg(status);
            report(true,reason); finish(true);
        } else {
            failed(QStringLiteral(u"%1 状态码 %2，未通过有效响应校验%3").arg(protocol).arg(status)
                .arg(status>=500?QStringLiteral(u"（服务或代理网关错误）"):QString()));
        }
    });
    QObject::connect(socket, QOverload<QAbstractSocket::SocketError>::of(&QTcpSocket::error), socket,
        [socket,failed](QAbstractSocket::SocketError code) {
            QString reason;
            switch(code) {
            case QAbstractSocket::ConnectionRefusedError: reason=QStringLiteral(u"连接被拒绝"); break;
            case QAbstractSocket::HostNotFoundError: reason=QStringLiteral(u"主机地址解析失败"); break;
            case QAbstractSocket::SocketTimeoutError: reason=QStringLiteral(u"连接超时"); break;
            case QAbstractSocket::RemoteHostClosedError: reason=QStringLiteral(u"对端关闭连接，未返回有效响应"); break;
            case QAbstractSocket::SocketAccessError: reason=QStringLiteral(u"网络访问被拒绝"); break;
            case QAbstractSocket::NetworkError: reason=QStringLiteral(u"网络不可达或连接中断"); break;
            default: reason=QStringLiteral(u"网络连接失败"); break;
            }
            failed(reason+QStringLiteral(u"（%1）").arg(socket->errorString()));
        });
    QObject::connect(timer, &QTimer::timeout, socket, [failed,connected,timeoutMs] {
        failed((*connected ? QStringLiteral(u"TCP已连接，但在 %1 ms 内未收到有效协议响应")
                           : QStringLiteral(u"连接超时，%1 ms 内未建立TCP连接")).arg(timeoutMs));
    });
    QTimer::singleShot(0, socket, attempt);
    return socket;
}
