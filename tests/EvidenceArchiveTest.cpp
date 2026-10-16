#include "EvidenceArchive.hpp"
#include "ICameraSource.hpp"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QEventLoop>
#include <QTimer>
#include <iostream>

class EncodedSource : public ICameraSource {
public:
    FrameCaptureResult result;
    bool open() override { return true; }
    bool isOpened() const override { return true; }
    bool grabFrame(cv::Mat& frame) override { frame=result.frame; return true; }
    FrameCaptureResult captureSnapshot() override { return result; }
    void close() override {}
    std::string getSourceName() const override { return "encoded-test"; }
};
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    InspectionOutcome normal;
    normal.recognitionPerformed=true; normal.result.success=true; normal.result.confidence=0.9;
    normal.result.recognitionQuality=QJsonObject{{"status","trusted"}};
    if(EvidenceArchive::abnormal(normal)) return 1;
    auto warning=normal; warning.result.centerInconsistent=true;
    if(!EvidenceArchive::abnormal(warning)) return 1;
    warning=normal; warning.result.recognitionQuality["status"]="review";
    if(!EvidenceArchive::abnormal(warning)) return 1;
    warning=normal; warning.result.confidence=0.3;
    if(!EvidenceArchive::abnormal(warning)) return 1;
    const auto policy=EvidenceArchive::Policy::fromEnvironment();
    if(policy.normalDays!=7 || policy.abnormalDays!=90) return 1;
    QTemporaryDir folder;
    auto source=std::make_shared<EncodedSource>();
    cv::Mat pixels(48,64,CV_8UC3,cv::Scalar(20,70,140));
    cv::line(pixels,{0,0},{63,47},cv::Scalar(240,240,240),3);
    cv::imencode(".jpg",pixels,source->result.encodedOriginal,{cv::IMWRITE_JPEG_QUALITY,95});
    source->result.frame=cv::imdecode(source->result.encodedOriginal,cv::IMREAD_COLOR);
    source->result.capturedAt=std::chrono::system_clock::now();
    InspectionService service(nullptr,1,[source](const InspectionTask&){return source;});
    InspectionTask task; task.device.cameraId="archive-test"; task.device.protocolType=CameraProtocolType::ONVIF_RTSP_STREAM; task.captureOnly=true; task.outputDirectory=folder.path();
    InspectionOutcome outcome;
    QEventLoop loop; QTimer timeout; timeout.setSingleShot(true);
    QObject::connect(&timeout,&QTimer::timeout,&loop,&QEventLoop::quit);
    QObject::connect(&service,&InspectionService::taskFinished,[&](const InspectionOutcome& result){outcome=result;});
    QObject::connect(&service,&InspectionService::finished,&loop,&QEventLoop::quit);
    service.start({task}); timeout.start(5000); loop.exec();
    if(service.isRunning() || !outcome.errorCode.isEmpty() || !outcome.originalPath.endsWith("original.jpg") || !outcome.encodedOriginal.empty()) return 1;
    QFile original(outcome.originalPath); if(!original.open(QIODevice::ReadOnly)) return 1;
    const auto bytes=original.readAll(); original.close();
    if(bytes!=QByteArray(reinterpret_cast<const char*>(source->result.encodedOriginal.data()),int(source->result.encodedOriginal.size()))) return 1;
    QFile metadata(QDir(folder.path()).filePath(outcome.taskId+"/result.json"));
    if(!metadata.open(QIODevice::ReadOnly)) return 1;
    const auto record=QJsonDocument::fromJson(metadata.readAll()).object(); metadata.close();
    if(record["archive"].toObject()["original_encoding"].toString()!="source_bytes" || !record["archive"].toObject()["anomaly"].toBool()) return 1;
    if(!record.contains("device") || !record.contains("profile_snapshot") || !record.contains("pointer_angle_deg")) return 1;
    if(!EvidenceArchive::expireLocalImages(folder.path(),outcome.taskId,QDateTime::currentDateTimeUtc()) || QFile::exists(outcome.originalPath) || !metadata.exists()) return 1;
    if(EvidenceArchive::expireLocalImages(folder.path(),"../outside",QDateTime::currentDateTimeUtc())) return 1;
    std::cout<<"Exact JPEG archive, sample classification, metadata survival and bounded local expiry passed\n";
    return 0;
}
