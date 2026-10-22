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

void require(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
void waitUntil(std::function<bool()> condition,const char* message) {
    if(condition()) return;
    QEventLoop loop; QTimer poll,deadline;
    poll.setInterval(10); deadline.setSingleShot(true);
    QObject::connect(&poll,&QTimer::timeout,&loop,[&]{if(condition()) loop.quit();});
    QObject::connect(&deadline,&QTimer::timeout,&loop,&QEventLoop::quit);
    poll.start(); deadline.start(int(CameraHealthSchedule::offlineIntervalMs) + 10000); loop.exec(); require(condition(),message);
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    try {
        QTemporaryDir files; require(files.isValid(),"temporary directory");
        QFile config(files.filePath("database.json")); require(config.open(QIODevice::WriteOnly),"test config");
        config.write("{\"version\":1,\"enabled\":false}");config.close();
        qputenv("GAUGE_MYSQL_DATABASE",QByteArray());qputenv("GAUGE_MYSQL_CONFIG_PATH",config.fileName().toUtf8());
        qputenv("GAUGE_DEVICE_CONFIG",files.filePath("devices.json").toUtf8());
        QTcpServer camera; require(camera.listen(QHostAddress::LocalHost,0),"mock listener");
        const auto port=camera.serverPort();
        bool cameraResponsive=true;
        QObject::connect(&camera,&QTcpServer::newConnection,&camera,[&]{
            while(camera.hasPendingConnections()) {
                auto* socket=camera.nextPendingConnection();
                QObject::connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
                QObject::connect(socket,&QTcpSocket::readyRead,socket,[socket,&cameraResponsive] {
                    const auto request=socket->readAll();
                    if(!cameraResponsive) return; // TUN accepts TCP while the disconnected camera cannot answer.
                    socket->write(request.startsWith("OPTIONS ") ? "RTSP/1.0 200 OK\r\nCSeq: 1\r\n\r\n" :
                        "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                    socket->disconnectFromHost();
                });
            }
        });
        DiscoveredCameraItem device; device.cameraId="connection-fixture";
        device.protocolType=CameraProtocolType::ONVIF_RTSP_STREAM; device.ipOrSn="127.0.0.1";
        device.port=port;device.rtspPort=port;device.isOnline=true;
        DeviceConfigManager::instance().addOrUpdateDevice(device,false);
        {
            AutoCameraDiscoveryService service; QList<bool> states; QStringList logs;
            QObject::connect(&service,&AutoCameraDiscoveryService::logGenerated,&app,[&](const QString& line){logs.append(line);});
            QObject::connect(&service,&AutoCameraDiscoveryService::cameraOnlineStateChanged,&app,[&](const QString& id,bool online){
                require(id==device.cameraId,"unexpected device"); states.append(online);
                DeviceConfigManager::instance().markDeviceOnline(id,online);
            });
            service.startHealthMonitoring();
            waitUntil([&]{return !states.isEmpty() && states.last();},"running: initial online missing");
            // Buffered preview frames can keep arriving after the physical link is lost.
            QTimer bufferedFrames; bufferedFrames.setInterval(40);
            QObject::connect(&bufferedFrames,&QTimer::timeout,&service,[&]{service.recordInspectionState(device.cameraId,QString(),true);});
            bufferedFrames.start();
            cameraResponsive=false; // Keep accepting TCP, reproducing the actual Mihomo TUN condition.
            waitUntil([&]{return !states.last();},"running: TCP-accepting proxy hid offline camera while buffered frames arrive");
            const auto offlineLog=logs.join("\n");
            require(offlineLog.contains("HTTP 127.0.0.1:"+QString::number(port)) &&
                offlineLog.contains("RTSP 127.0.0.1:"+QString::number(port)) &&
                offlineLog.contains(QStringLiteral(u"TCP已连接")),"service log must include each endpoint and actual failure cause");
            cameraResponsive=true;
            // Decoder errors during recovery must not continually override actual reachability.
            QTimer decodeErrors; decodeErrors.setInterval(50);
            QObject::connect(&decodeErrors,&QTimer::timeout,&service,[&]{service.recordInspectionState(device.cameraId,"READ_FAILED",false);});
            decodeErrors.start();
            waitUntil([&]{return states.last();},"running: cable restore did not publish online/yellow");
            decodeErrors.stop(); service.stopHealthMonitoring();
            require(states==QList<bool>({true,false,true}),"running connectivity transitions must be stable");
            std::cout<<"PASS: TUN accepts TCP but camera disconnected/red -> restored/yellow, including buffered frames and decoder failures\n"<<std::flush;
        }
        camera.close();
        // Simulate a stale online flag persisted by the previous session.
        DeviceConfigManager::instance().markDeviceOnline(device.cameraId,true);
        {
            AutoCameraDiscoveryService service; QList<bool> states;
            QObject::connect(&service,&AutoCameraDiscoveryService::cameraOnlineStateChanged,&app,[&](const QString& id,bool online){
                states.append(online); DeviceConfigManager::instance().markDeviceOnline(id,online);
            });
            service.startHealthMonitoring();
            waitUntil([&]{return !states.isEmpty();},"startup: unplugged camera remained pending/yellow");
            require(!states.first(),"startup: persisted online flag must not override actual offline/red");
            bool online=true;
            require(service.knownOnlineState(device.cameraId,online) && !online,"verified offline must be available to reloaded list");
            require(camera.listen(QHostAddress::LocalHost,port),"restore startup listener");
            waitUntil([&]{return states.last();},"startup: restored camera did not publish online/yellow");
            service.stopHealthMonitoring();
            require(states==QList<bool>({false,true}),"startup connectivity transitions");
            std::cout<<"PASS: start unplugged/red -> restored/yellow, stale saved online flag ignored\n";
        }
        return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
