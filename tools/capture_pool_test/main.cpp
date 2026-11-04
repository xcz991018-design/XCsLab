#include "IsolatedCapture.hpp"
#include <QCoreApplication>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QFile>
#include <cstdlib>
#include <future>
#include <set>
#include <iostream>

static void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
template<class T> T wait(std::future<T>& future) {
    QElapsedTimer timer;timer.start();
    while(future.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready) {
        QCoreApplication::processEvents(QEventLoop::AllEvents,10);QThread::msleep(1);
        if(timer.elapsed()>30000)throw std::runtime_error("test deadline");
    }
    return future.get();
}
int main(int argc,char** argv) {
    const int worker=runCaptureWorker(argc,argv);if(worker>=0)return worker;
    QCoreApplication app(argc,argv);
    CapturePoolLifetime capturePoolLifetime;
    if(argc==2 && QString::fromLocal8Bit(argv[1])=="--abrupt-parent") {
        InspectionTask task;task.device.protocolType=CameraProtocolType::CUSTOM_SDK;
        auto result=isolatedCapture(task,std::make_shared<std::atomic<bool>>(false));
        // Bypass all C++/Qt destructors to exercise Windows kill-on-close jobs.
        std::_Exit(result.failure==CaptureFailure::UnsupportedProtocol?0:1);
    }
    try {
        QTcpServer server;require(server.listen(QHostAddress::LocalHost,0),"listen");
        cv::Mat frame(240,320,CV_8UC3);cv::randu(frame,0,256);
        std::vector<uchar> jpeg;cv::imencode(".jpg",frame,jpeg);
        int profiles=0,snapshots=0,delay=0,snapshotUris=0,advertisedWidth=320,advertisedHeight=240;bool singleUse=false;
        QObject::connect(&server,&QTcpServer::newConnection,[&]{
            while(server.hasPendingConnections()) {
                auto* socket=server.nextPendingConnection();
                QObject::connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
                auto buffer=std::make_shared<QByteArray>();auto handled=std::make_shared<bool>(false);
                QObject::connect(socket,&QTcpSocket::readyRead,socket,[&,socket,buffer,handled]{
                    *buffer+=socket->readAll();if(*handled)return;
                    int split=buffer->indexOf("\r\n\r\n");if(split<0)return;
                    const auto match=QRegularExpression("Content-Length: ([0-9]+)",QRegularExpression::CaseInsensitiveOption).match(QString::fromLatin1(buffer->left(split)));
                    if(buffer->size()<split+4+match.captured(1).toInt())return;
                    *handled=true;
                    const auto base=QString("http://127.0.0.1:%1").arg(server.serverPort());
                    QByteArray body;QString type="application/soap+xml";int latency=0;
                    if(buffer->startsWith("GET /image")) {
                        ++snapshots;type="image/jpeg";body=QByteArray(reinterpret_cast<const char*>(jpeg.data()),int(jpeg.size()));latency=delay;
                    } else {
                        QString xml;
                        if(buffer->contains("GetCapabilities"))xml="<Capabilities><Media><XAddr>"+base+"/media</XAddr></Media></Capabilities>";
                        else if(buffer->contains("GetProfiles")){++profiles;xml=QString("<Profiles token=\"main\"><VideoEncoderConfiguration><Resolution><Width>%1</Width><Height>%2</Height></Resolution></VideoEncoderConfiguration></Profiles>").arg(advertisedWidth).arg(advertisedHeight);}
                        else if(buffer->contains("GetSnapshotUri")){++snapshotUris;xml="<MediaUri><Uri>"+base+"/image</Uri><InvalidAfterConnect>"+(singleUse?QString("true"):QString("false"))+"</InvalidAfterConnect><Timeout>PT600S</Timeout></MediaUri>";}
                        else if(buffer->contains("GetStreamUri"))xml="<MediaUri><Uri>rtsp://127.0.0.1:554/main</Uri></MediaUri>";
                        body=("<Envelope><Body>"+xml+"</Body></Envelope>").toUtf8();
                    }
                    QTimer::singleShot(latency,socket,[socket,body,type]{
                        socket->write("HTTP/1.1 200 OK\r\nContent-Type: "+type.toUtf8()+"\r\nContent-Length: "+QByteArray::number(body.size())+"\r\nConnection: close\r\n\r\n"+body);
                        socket->disconnectFromHost();
                    });
                });
            }
        });
        InspectionTask task;task.device.protocolType=CameraProtocolType::ONVIF_RTSP_STREAM;
        task.device.ipOrSn="127.0.0.1";task.device.port=server.serverPort();task.device.username="camera";
        task.preferOnvifSnapshot=true;task.captureTimeoutMs=8000;
        {
            auto direct=task;direct.preferOnvifSnapshot=false;direct.requireOnvifSnapshot=true;
            auto source=InspectionService::createCameraSource(direct);
            const auto info=dynamic_cast<ONVIFDeviceManager*>(source.get())->getDeviceInfo();
            require(!info.preferHttpSnapshot && !info.requireHttpSnapshot,
                "requiring ONVIF must not re-enable HTTP snapshots for production RTSP capture");
        }
        auto cancel=std::make_shared<std::atomic<bool>>(false);
        auto acquire=[&](InspectionTask value){return std::async(std::launch::async,[value,cancel]{return isolatedCapture(value,cancel);});};
        auto warm=std::async(std::launch::async,[&]{prewarmIsolatedCapture(task,cancel);return true;});wait(warm);
        require(profiles==1 && snapshots==0,"prewarm resolves metadata without grabbing images");
        auto one=acquire(task);auto first=wait(one);
        require(first.ok() && first.frame.size()==frame.size() && first.encodedOriginal==jpeg,"full resolution original preserved");
        require(first.timings["metadata_cache_hit"].toBool() && profiles==1 && snapshots==1,"warm capture uses metadata but requests a fresh image");
        auto two=acquire(task);auto second=wait(two);
        require(second.ok() && profiles==1 && snapshots==2 && first.timings["worker_pid"]==second.timings["worker_pid"],"process and metadata reused");
        task.device.password="changed";auto changed=acquire(task);require(wait(changed).ok() && profiles==2,"credential change invalidates metadata");
        std::set<qint64> pids;int largestCache=0;
        for(int batch=0;batch<75;++batch) {
            std::vector<std::future<FrameCaptureResult>> requests;
            for(int n=0;n<8 && batch*8+n<600;++n){auto device=task;device.device.username=QString("device_%1").arg(batch*8+n);requests.push_back(acquire(device));}
            for(auto& request:requests){auto result=wait(request);require(result.ok(),result.message.c_str());pids.insert(qint64(result.timings["worker_pid"].toDouble()));largestCache=std::max(largestCache,result.timings["metadata_cache_entries"].toInt());require(result.timings["metadata_cache_entries"].toInt()<=128,"cache bounded");}
        }
        require(pids.size()<=4 && snapshots==603 && largestCache==128,"600 devices use at most four processes and each gets a fresh image");
        {
            auto patrol=task;patrol.device.username="low-resolution-case";patrol.allowLowerResolutionHttpSnapshot=true;
            advertisedWidth=640;advertisedHeight=480;
            auto low=acquire(patrol);auto accepted=wait(low);
            require(accepted.ok() && accepted.frame.size()==frame.size() && accepted.message=="ONVIF_HTTP",
                "patrol accepts lower-resolution HTTP image");
            patrol.allowLowerResolutionHttpSnapshot=false;
            auto strict=acquire(patrol);auto rejected=wait(strict);
            require(!rejected.ok() && !rejected.timings["metadata_cache_hit"].toBool(),
                "strict capture must not reuse patrol's relaxed cached source");
            const int imagesBefore=snapshots,urisBefore=snapshotUris;
            patrol.requireOnvifSnapshot=true; // Strict RTSP overrides the legacy HTTP preference.
            auto direct=acquire(patrol);wait(direct);
            require(snapshots==imagesBefore && snapshotUris==urisBefore,"direct RTSP never queries HTTP snapshot URI or image");
            advertisedWidth=320;advertisedHeight=240;
        }
        singleUse=true;task.device.password="one-shot-uri";
        auto singleFirst=acquire(task);require(wait(singleFirst).ok(),"single-use snapshot first request");
        const int uriCount=snapshotUris;
        auto singleSecond=acquire(task);require(wait(singleSecond).ok() && snapshotUris==uriCount+1,"single-use URI is refreshed");
        singleUse=false;
        delay=5000;task.captureTimeoutMs=150;
        auto timed=acquire(task);auto timeout=wait(timed);require(timeout.failure==CaptureFailure::Timeout && timeout.frame.empty(),"deadline kills worker without stale frame");
        delay=0;task.captureTimeoutMs=8000;auto recovered=acquire(task);require(wait(recovered).ok(),"worker recovers after timeout");
        delay=5000;auto cancelled=acquire(task);QTimer::singleShot(50,[cancel]{cancel->store(true);});
        require(wait(cancelled).failure==CaptureFailure::Cancelled,"active cancellation");
        cancel->store(false);delay=0;auto again=acquire(task);require(wait(again).ok(),"worker recovers after cancellation");
        {
            QTemporaryDir directory;require(directory.isValid(),"pipeline temporary workspace");
            QFile config(directory.filePath("database.json"));require(config.open(QIODevice::WriteOnly),"database test config");
            config.write("{\"version\":1,\"enabled\":false}");config.close();
            qputenv("GAUGE_MYSQL_DATABASE",QByteArray());qputenv("GAUGE_MYSQL_CONFIG_PATH",config.fileName().toUtf8());
            InspectionService service(nullptr,4);QList<InspectionTask> tasks;
            for(int n=0;n<24;++n){auto item=task;item.device.cameraId=QString("pipeline-%1").arg(n);item.device.username=item.device.cameraId;item.captureOnly=true;item.outputDirectory=directory.path();tasks.append(item);}
            QEventLoop loop;QTimer deadline;deadline.setSingleShot(true);
            int active=0,peak=0,completed=0;bool failed=false;
            QObject::connect(&service,&InspectionService::taskStarted,[&](const QString&,const QString&){peak=std::max(peak,++active);});
            QObject::connect(&service,&InspectionService::taskFinished,[&](const InspectionOutcome& out){--active;++completed;failed|=!out.errorCode.isEmpty();});
            QObject::connect(&service,&InspectionService::finished,&loop,&QEventLoop::quit);
            QObject::connect(&deadline,&QTimer::timeout,&loop,[&]{failed=true;service.stop();loop.quit();});
            const int initialSnapshots=snapshots;
            require(service.start(tasks),"pipeline start");deadline.start(20000);loop.exec();deadline.stop();
            require(!failed && completed==24 && peak<=4 && snapshots==initialSnapshots+24,
                "scheduler keeps bounded in-flight frames and speculative work never grabs images");
        }
        std::cout<<"PASS: 600 devices, bounded pool/cache, prewarm, freshness, credential invalidation, timeout and cancellation recovery"<<std::endl;
    }catch(const std::exception& error){std::cerr<<error.what()<<std::endl;return 1;}
}
