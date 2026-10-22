#include "InspectionService.hpp"
#include "CameraPreviewService.hpp"
#include "DeviceConnection.hpp"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QFile>
#include <QTimer>
#include <QEventLoop>
#include <QThread>
#include <QMutex>
#include <QMutexLocker>
#include <iostream>
#include <stdexcept>

void require(bool ok,const char* message) { if(!ok) throw std::runtime_error(message); }
class Source : public ICameraSource {
    QString host; bool opened{false};
public:
    explicit Source(QString value):host(std::move(value)) {}
    bool open() override { opened=true; return true; }
    void close() override { opened=false; }
    bool isOpened() const override { return opened; }
    bool grabFrame(cv::Mat&) override { return false; }
    std::string getSourceName() const override { return host.toStdString(); }
    FrameCaptureResult readFrame() override {
        QThread::msleep(host.endsWith(".11")?180:10);
        FrameCaptureResult result; result.failure=CaptureFailure::ReadFailed;
        result.message=host.toStdString(); return result;
    }
};
DiscoveredCameraItem camera(QString id,QString host) {
    DiscoveredCameraItem device; device.cameraId=id; device.hardwareId=id;
    device.protocolType=CameraProtocolType::ONVIF_RTSP_STREAM;
    device.ipOrSn=host; device.port=80; device.rtspPort=554;
    device.rtspUrl="rtsp://"+host+":554/vendor/stream";
    device.username="fixture"; device.password="secret";
    device.deviceTag="preserved-archive"; device.hasCalibration=true;
    return device;
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    try {
        QTemporaryDir files; require(files.isValid(),"temporary directory");
        QFile config(files.filePath("database.json")); require(config.open(QIODevice::WriteOnly),"test config");
        config.write("{\"version\":1,\"enabled\":false}"); config.close();
        qputenv("GAUGE_MYSQL_DATABASE",QByteArray()); qputenv("GAUGE_MYSQL_CONFIG_PATH",config.fileName().toUtf8());
        auto a=camera("A","192.168.110.11"), b=camera("B","192.168.110.11");
        auto nextA=camera("A","192.168.110.13"), latestA=camera("A","192.168.110.14");
        auto nextB=camera("B","192.168.110.13");
        latestA.deviceTag="must-not-replace-frozen-archive";
        QMutex mutex; QStringList opened;
        auto factory=[&](const InspectionTask& task)->CameraSourcePtr {
            QMutexLocker lock(&mutex); opened.append(task.device.cameraId+"@"+task.device.ipOrSn);
            return std::make_shared<Source>(task.device.ipOrSn);
        };
        InspectionService service(nullptr,1,factory);
        QList<InspectionOutcome> outcomes; int completed=0,total=0; bool cancelled=true,timedOut=false;
        QEventLoop loop; QTimer deadline; deadline.setSingleShot(true);
        QObject::connect(&deadline,&QTimer::timeout,&loop,[&]{timedOut=true;service.stop();loop.quit();});
        QObject::connect(&service,&InspectionService::taskFinished,&loop,[&](const InspectionOutcome& result){outcomes.append(result);});
        QObject::connect(&service,&InspectionService::progressChanged,&loop,[&](int count,int size){completed=count;total=size;});
        QObject::connect(&service,&InspectionService::finished,&loop,[&](bool stopped){cancelled=stopped;loop.quit();});
        InspectionTask first; first.device=a; first.captureOnly=true; first.outputDirectory=files.path();
        InspectionTask second=first; second.device=b;
        InspectionTask historical=first; historical.inputKind=InspectionInputKind::HistoricalImage;
        historical.localImagePath=files.filePath("not-present.jpg");
        require(service.start({first,second,historical}),"start inspection");
        QTimer::singleShot(30,&service,[&]{service.updateDeviceConnection(nextA); service.updateDeviceConnection(nextB);});
        QTimer::singleShot(60,&service,[&]{service.updateDeviceConnection(latestA);});
        deadline.start(5000); loop.exec(); deadline.stop();
        require(!timedOut && !cancelled && outcomes.size()==3 && completed==3 && total==3,"migration must not change progress or duplicate results");
        require(outcomes[0].cameraId=="A" && outcomes[0].deviceSnapshot["ip_or_sn"].toString()==latestA.ipOrSn,
            "active old-address task must retry with the latest endpoint");
        require(outcomes[1].cameraId=="B" && outcomes[1].deviceSnapshot["ip_or_sn"].toString()==nextB.ipOrSn,
            "queued task must use updated endpoint");
        require(outcomes[2].inputKind==InspectionInputKind::HistoricalImage && outcomes[2].deviceSnapshot["ip_or_sn"].toString()==a.ipOrSn,
            "historical inputs must not be rewritten");
        require(opened==QStringList({"A@192.168.110.11","A@192.168.110.14","B@192.168.110.13"}),
            "no continued requests to old endpoint after migration");
        require(outcomes[0].deviceSnapshot["device_tag"].toString()=="preserved-archive","migration must preserve frozen archive fields");
        // Stopping a migrating batch must cancel its retry and allow a later fresh round.
        outcomes.clear(); cancelled=false;
        require(service.start({first,second}),"start cancellation round");
        QTimer::singleShot(30,&service,[&]{service.updateDeviceConnection(nextA);service.stop();});
        deadline.start(5000); loop.exec(); deadline.stop();
        require(!timedOut && cancelled && outcomes.isEmpty() && !service.isRunning(),"stop must suppress endpoint retries");
        first.device=latestA;
        require(service.start({first}),"restart after cancellation");
        deadline.start(5000); loop.exec(); deadline.stop();
        require(!timedOut && !cancelled && outcomes.size()==1,"fresh round must not retain cancellation state");
        first.device=a;
        // Same UUID: preview generations must reject the old connection's late frame/status.
        CameraPreviewService preview(nullptr,factory); QEventLoop previewLoop; QStringList reported;
        QObject::connect(&preview,&CameraPreviewService::frameReady,&previewLoop,[&](const QString&,const FrameCaptureResult& frame){
            reported.append(QString::fromStdString(frame.message)); preview.stop(); previewLoop.quit();
        });
        InspectionTask previewTask=first; preview.select(previewTask);
        QTimer::singleShot(30,&preview,[&]{previewTask.device=nextA; preview.select(previewTask);});
        QTimer previewDeadline; previewDeadline.setSingleShot(true);
        QObject::connect(&previewDeadline,&QTimer::timeout,&previewLoop,&QEventLoop::quit);
        previewDeadline.start(3000); previewLoop.exec();
        require(reported==QStringList{nextA.ipOrSn},"same-UUID preview must emit only new endpoint results");
        std::cout<<"PASS: active capture restart, queued endpoint update, rapid IP changes, stable progress, frozen archives, historical inputs and preview generation\n";
        return 0;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
