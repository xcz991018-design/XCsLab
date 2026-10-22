#include "AutoCameraDiscoveryService.hpp"
#include "DeviceConfigManager.hpp"
#include <QCoreApplication>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QFile>
#include <QEventLoop>
#include <QTimer>
#include <iostream>
#include <stdexcept>
void require(bool ok,const char* error) { if(!ok) throw std::runtime_error(error); }
void waitUntil(std::function<bool()> condition,int timeout) {
    QEventLoop loop; QTimer poll,limit; poll.setInterval(10);limit.setSingleShot(true);
    QObject::connect(&poll,&QTimer::timeout,&loop,[&]{if(condition())loop.quit();});
    QObject::connect(&limit,&QTimer::timeout,&loop,&QEventLoop::quit);
    poll.start();limit.start(timeout);loop.exec();require(condition(),"connection check timed out");
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    try {
        QTemporaryDir files; QFile config(files.filePath("database.json")); require(config.open(QIODevice::WriteOnly),"config");
        config.write("{\"version\":1,\"enabled\":false}");config.close();
        qputenv("GAUGE_MYSQL_DATABASE",QByteArray());qputenv("GAUGE_MYSQL_CONFIG_PATH",config.fileName().toUtf8());
        bool responsive=true; QTcpServer camera; require(camera.listen(QHostAddress::LocalHost,0),"listener");
        QObject::connect(&camera,&QTcpServer::newConnection,&camera,[&] {
            while(camera.hasPendingConnections()) {
                auto* socket=camera.nextPendingConnection();
                QObject::connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
                QObject::connect(socket,&QTcpSocket::readyRead,socket,[&,socket] {
                    const auto request=socket->readAll(); if(!responsive)return;
                    socket->write(request.startsWith("OPTIONS")?"RTSP/1.0 200 OK\r\nCSeq: 1\r\n\r\n":
                        "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");socket->disconnectFromHost();
                });
            }
        });
        DiscoveredCameraItem device;device.cameraId="capture-failure";device.ipOrSn="127.0.0.1";
        device.protocolType=CameraProtocolType::ONVIF_RTSP_STREAM;device.port=camera.serverPort();device.rtspPort=device.port;
        DeviceConfigManager::instance().addOrUpdateDevice(device,false);
        AutoCameraDiscoveryService service; QList<bool> states; QStringList logs;
        QObject::connect(&service,&AutoCameraDiscoveryService::cameraOnlineStateChanged,&app,[&](const QString&,bool online){states.append(online);});
        QObject::connect(&service,&AutoCameraDiscoveryService::logGenerated,&app,[&](const QString& line){logs.append(line);});
        service.startHealthMonitoring();waitUntil([&]{return !states.isEmpty()&&states.last();},3000);
        // A failed recognition must never start a network outage check.
        service.recordInspectionState(device.cameraId,"RECOGNITION_FAILED",false);
        require(logs.filter(QStringLiteral(u"立即复核")).isEmpty(),"recognition failure must not trigger connectivity checks");
        // Capture/decoder failure while the camera responds must leave connectivity online.
        service.deferHealthForCapture(device.cameraId);
        service.recordInspectionState(device.cameraId,"READ_FAILED",false);
        waitUntil([&]{return logs.filter(QStringLiteral(u"立即复核")).size()==1;},1000);
        QEventLoop settle;QTimer::singleShot(250,&settle,&QEventLoop::quit);settle.exec();
        require(states==QList<bool>{true},"decoder failure must not falsely mark reachable camera offline");
        service.stopHealthMonitoring();
        // New session gives a fresh normal 15-second deadline, then the cable is removed.
        AutoCameraDiscoveryService disconnected;states.clear();logs.clear();
        QObject::connect(&disconnected,&AutoCameraDiscoveryService::cameraOnlineStateChanged,&app,[&](const QString&,bool online){states.append(online);});
        QObject::connect(&disconnected,&AutoCameraDiscoveryService::logGenerated,&app,[&](const QString& line){logs.append(line);});
        disconnected.startHealthMonitoring();waitUntil([&]{return !states.isEmpty()&&states.last();},3000);
        responsive=false;disconnected.deferHealthForCapture(device.cameraId);
        QElapsedTimer elapsed;elapsed.start();
        for(int i=0;i<20;++i) disconnected.recordInspectionState(device.cameraId,"CAPTURE_TIMEOUT",false);
        waitUntil([&]{return !states.last();},7000);
        require(elapsed.elapsed()<7000,"capture failure must bypass the 15-second interval and capture grace");
        require(logs.filter(QStringLiteral(u"立即复核")).size()==1,"burst failures must share one in-flight probe");
        disconnected.recordInspectionState(device.cameraId,"READ_FAILED",false);
        require(logs.filter(QStringLiteral(u"立即复核")).size()==1,"known offline devices must retain the 30-second retry interval");
        disconnected.stopHealthMonitoring();
        std::cout<<"PASS: capture failure triggers immediate bounded probe; reachable decoder failure stays online; burst failures coalesce; offline retry preserved\n";
        return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
