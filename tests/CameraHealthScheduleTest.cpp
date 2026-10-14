#include "CameraHealthSchedule.hpp"
#include "AutoCameraDiscoveryService.hpp"
#include "DeviceConfigManager.hpp"
#include <QCoreApplication>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include <QEventLoop>
#include <QTimer>
#include <iostream>
#include <stdexcept>
static void require(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
static void drain() {
    QEventLoop loop; QTimer::singleShot(50,&loop,&QEventLoop::quit); loop.exec();
    QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    try {
        CameraHealthSchedule schedule;
        schedule.configure("camera","host:8554"); bool changed=false;
        auto token=schedule.begin("camera");
        require(schedule.complete("camera",token,false,0,changed)&&changed,"Initial offline state missing");
        for(const auto delay:{60000,120000,240000,300000,300000}) {
            require(schedule.states["camera"].due==delay,"Backoff incorrect");
            schedule.complete("camera",schedule.begin("camera"),false,0,changed);
            require(!changed,"Unchanged offline status would spam logs");
        }
        token=schedule.begin("camera"); schedule.update("camera",true,100,true);
        require(!schedule.complete("camera",token,false,101,changed)&&schedule.states["camera"].online,"Late probe overrode live frame");
        require(schedule.states["camera"].due==30100&&schedule.states["camera"].failures==0,"Live frame failed to reset/defer health");
        token=schedule.begin("camera"); schedule.configure("camera","changed:554");
        require(!schedule.complete("camera",token,true,102,changed),"Old endpoint result accepted");
        schedule.prune({}); require(schedule.states.isEmpty(),"Removed camera state leaked");
        for(int i=0;i<1000;++i) schedule.configure(QString::number(i),"endpoint");
        int firstChecks=0,cursor=0;
        for(int second=0;second<63;++second) {
            int started=0;
            for(int visited=0;visited<1000&&started<16;++visited) {
                const auto id=QString::number(cursor++%1000); auto& state=schedule.states[id];
                if(state.due>second*1000) continue;
                if(!state.known) ++firstChecks;
                schedule.complete(id,schedule.begin(id),false,second*1000,changed); ++started;
            }
        }
        require(firstChecks==1000,"1,000-camera registry starved devices");
        QTemporaryDir folder(QDir::currentPath()+"/health-test-XXXXXX"); require(folder.isValid(),"Temporary directory failed");
        QFile disabled(folder.path()+"/database.json"); require(disabled.open(QIODevice::WriteOnly),"Disabled database fixture failed");
        disabled.write("{\"version\":1,\"enabled\":false}"); disabled.close();
        qputenv("GAUGE_MYSQL_DATABASE",""); qputenv("GAUGE_MYSQL_CONFIG_PATH",disabled.fileName().toUtf8());
        QFile registry(folder.path()+"/devices.json"); require(registry.open(QIODevice::WriteOnly),"Registry fixture failed");
        registry.write("{\"devices\":[],\"system\":{}}"); registry.close();
        auto& manager=DeviceConfigManager::instance(); require(manager.load(registry.fileName()),"Registry load failed");
        QTcpServer server; require(server.listen(QHostAddress::LocalHost,0),"Local listener failed"); int connections=0;
        QObject::connect(&server,&QTcpServer::newConnection,&app,[&] {
            while(server.hasPendingConnections()) {++connections;auto* socket=server.nextPendingConnection();socket->close();socket->deleteLater();}
        });
        for(int i=0;i<40;++i) {
            DiscoveredCameraItem camera; camera.cameraId=QString("CAM_%1").arg(i,3,10,QChar('0'));
            camera.protocolType=CameraProtocolType::ONVIF_RTSP_STREAM; camera.ipOrSn="127.0.0.1";
            camera.rtspPort=server.serverPort(); camera.port=1; manager.addOrUpdateDevice(camera,false);
        }
        AutoCameraDiscoveryService service; int logs=0;
        QObject::connect(&service,&AutoCameraDiscoveryService::logGenerated,&app,[&](const QString&){++logs;});
        for(int i=0;i<20;++i) service.recordCaptureState(QString("CAM_%1").arg(i,3,10,QChar('0')),true);
        require(QMetaObject::invokeMethod(&service,"pollHealth",Qt::DirectConnection),"Health polling unavailable");
        int activeSockets=0;
        for(auto* socket:service.findChildren<QTcpSocket*>())
            if(socket->state()!=QAbstractSocket::UnconnectedState) ++activeSockets;
        require(activeSockets<=16,"Socket concurrency exceeded 16");
        QMetaObject::invokeMethod(&service,"pollHealth",Qt::DirectConnection); drain();
        require(connections==16,"First batch or frame reuse failed");
        QMetaObject::invokeMethod(&service,"pollHealth",Qt::DirectConnection); drain();
        require(connections==20,"Remaining devices missed in second batch");
        const int previousLogs=logs;
        service.recordCaptureState("CAM_000",true);
        service.recordInspectionState("CAM_001","RECOGNITION_FAILED",true);
        service.recordInspectionState("CAM_002","CALIBRATION_REQUIRED",false);
        QMetaObject::invokeMethod(&service,"pollHealth",Qt::DirectConnection); drain();
        require(connections==20&&logs==previousLogs,"Unchanged frames caused duplicate probes/logs");
        auto pending=manager.getDevice("CAM_039"); pending.cameraId="CAM_PENDING";
        manager.addOrUpdateDevice(pending,false);
        QMetaObject::invokeMethod(&service,"pollHealth",Qt::DirectConnection);
        service.recordCaptureState(pending.cameraId,true); // Cancels a probe before its callback arrives.
        const int afterLiveLog=logs; drain();
        require(logs==afterLiveLog,"Cancelled probe changed a newer live state");
        pending.cameraId="CAM_STOP_PENDING"; manager.addOrUpdateDevice(pending,false);
        QMetaObject::invokeMethod(&service,"pollHealth",Qt::DirectConnection);
        service.stopAutoScan(); const int stoppedLogs=logs; drain();
        require(service.findChildren<QTcpSocket*>().isEmpty()&&logs==stoppedLogs,"Stop did not cancel pending sockets");
        const int stoppedConnections=connections;
        QMetaObject::invokeMethod(&service,"pollHealth",Qt::DirectConnection); drain();
        require(connections==stoppedConnections,"Stopped health checks opened sockets");
        std::cout<<"Backoff, 1,000-camera fairness, stale results, 16-socket bound, RTSP port, frame reuse and quiet logs passed\n";
        return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<"\n";return 1;}
}
