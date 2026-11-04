#include "IsolatedCapture.hpp"
#include "DeviceCredentialStore.hpp"
#include "DeviceCodec.hpp"
#include "MySqlRepository.hpp"
#include <QCommandLineParser>
#include <QElapsedTimer>
#include <QProcess>
#include <QTemporaryDir>
#include <QSaveFile>
#include <QTextStream>

// Read-only live acquisition benchmark. No InspectionService::start(), inference,
// calibration writes, device clock changes, or database writes are performed.
static FrameCaptureResult oneShot(const InspectionTask& task) {
    QTemporaryDir directory;FrameCaptureResult result;
    if(!directory.isValid()){result.failure=CaptureFailure::WorkerFailed;return result;}
    QProcess child;child.setStandardOutputFile(QProcess::nullDevice());child.setStandardErrorFile(QProcess::nullDevice());
    child.start(QCoreApplication::applicationFilePath(),{"--capture-worker",directory.path()});
    if(!child.waitForStarted(5000)){result.failure=CaptureFailure::WorkerFailed;return result;}
    auto request=deviceToJson(task.device,true);
    request["_prefer_onvif_snapshot"]=task.preferOnvifSnapshot;request["_require_onvif_snapshot"]=task.requireOnvifSnapshot;request["_allow_lower_resolution_http"]=task.allowLowerResolutionHttpSnapshot;
    child.write(QJsonDocument(request).toJson(QJsonDocument::Compact));child.closeWriteChannel();
    if(!child.waitForFinished(task.captureTimeoutMs)){child.kill();child.waitForFinished(1000);result.failure=CaptureFailure::Timeout;return result;}
    QFile metadata(directory.filePath("result.json"));
    if(child.exitCode()!=0 || !metadata.open(QIODevice::ReadOnly)){result.failure=CaptureFailure::WorkerFailed;return result;}
    const auto json=QJsonDocument::fromJson(metadata.readAll()).object();
    result.failure=static_cast<CaptureFailure>(json["failure"].toInt(int(CaptureFailure::WorkerFailed)));
    result.message=json["message"].toString().toStdString();result.timings=json["timings"].toObject();
    if(result.failure!=CaptureFailure::None)return result;
    const auto suffix=json["original_suffix"].toString();
    if(suffix!=".jpg" && suffix!=".png"){result.failure=CaptureFailure::WorkerFailed;return result;}
    QFile image(directory.filePath("frame"+suffix));
    if(!image.open(QIODevice::ReadOnly)){result.failure=CaptureFailure::NoFrame;return result;}
    const auto bytes=image.readAll();std::vector<uchar> encoded(bytes.begin(),bytes.end());
    result.frame=cv::imdecode(encoded,cv::IMREAD_COLOR);
    if(json["original_preserved"].toBool())result.encodedOriginal=std::move(encoded);
    return result;
}

int main(int argc,char** argv) {
    const int worker=runCaptureWorker(argc,argv);if(worker>=0)return worker;
    QCoreApplication app(argc,argv);CapturePoolLifetime poolLifetime;
    QCommandLineParser parser;parser.addHelpOption();
    parser.addOptions({QCommandLineOption("camera","Registered camera ID","id"),
        QCommandLineOption("credentials","Existing local credential registry (read only)","path",DeviceCredentialStore::defaultPath()),
        QCommandLineOption("output","Timing report JSON","path"),
        QCommandLineOption("http-probe","Diagnose HTTP snapshot only; no RTSP fallback"),
        QCommandLineOption("count","Samples per pooled path including first cold capture","n","6"),
        QCommandLineOption("baseline-count","One-shot worker comparison samples","n","3")});
    parser.process(app);
    try {
        bool countOk=false,baselineOk=false;const int count=parser.value("count").toInt(&countOk),baseline=parser.value("baseline-count").toInt(&baselineOk);
        if(!parser.isSet("camera") || !parser.isSet("output") || !countOk || count<2 || count>20 || !baselineOk || baseline<0 || baseline>10)
            throw std::runtime_error("Provide camera/output; count 2-20, baseline-count 0-10");
        QJsonObject credentials;QString error;
        if(!DeviceCredentialStore::readRegistry(parser.value("credentials"),credentials,error))throw std::runtime_error("Cannot read existing camera credential registry");
        // Read current archive directly, avoiding DeviceConfigManager migration/sync writes.
        const auto archive=MySqlRepository().loadDevice(parser.value("camera"));
        InspectionTask task;task.device=deviceFromJson(DeviceCredentialStore::withCredentials(deviceToJson(archive),credentials["devices"].toArray()));
        if(task.device.protocolType!=CameraProtocolType::ONVIF_RTSP_STREAM)throw std::runtime_error("Selected camera is not ONVIF");
        task.preferOnvifSnapshot=false;task.requireOnvifSnapshot=true;task.captureTimeoutMs=12000;
        if(parser.isSet("http-probe")) {
            auto source=InspectionService::createCameraSource(task);
            auto* camera=dynamic_cast<ONVIFDeviceManager*>(source.get());
            if(!camera)throw std::runtime_error("ONVIF source unavailable");
            QElapsedTimer timer;timer.start();const bool prepared=camera->prepareSnapshot();const double prepareMs=timer.nsecsElapsed()/1e6;
            timer.restart();const auto result=camera->captureHttpSnapshot();
            QJsonObject diagnostic{{"prepared",prepared},{"metadata_prepare_ms",prepareMs},{"http_total_ms",timer.nsecsElapsed()/1e6},
                {"http_ok",result.ok()},{"reason",QString::fromStdString(result.message)},{"stages",result.timings}};
            const auto bytes=QJsonDocument(diagnostic).toJson();QSaveFile output(parser.value("output"));
            if(!output.open(QIODevice::WriteOnly)||output.write(bytes)!=bytes.size()||!output.commit())throw std::runtime_error("Cannot save diagnostic");
            QTextStream(stdout)<<bytes<<Qt::endl;return 0;
        }
        const auto cancelled=std::make_shared<std::atomic<bool>>(false);
        QJsonArray samples;
        QJsonObject report{{"camera_id",task.device.cameraId},{"host",task.device.ipOrSn},{"port",task.device.port},
            {"started_at",QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
            {"scope","Acquisition path only; excludes archive lookup, recognition/OCR, rendering and persistence"}};
        auto save=[&]{report["samples"]=samples;QSaveFile output(parser.value("output"));
            const auto bytes=QJsonDocument(report).toJson();if(!output.open(QIODevice::WriteOnly)||output.write(bytes)!=bytes.size()||!output.commit())throw std::runtime_error("Cannot write benchmark report");};
        QTextStream(stdout)<<"Camera "<<task.device.cameraId<<" at "<<task.device.ipOrSn<<":"<<task.device.port<<Qt::endl;
        for(const auto& mode:{QString("calibration"),QString("recognition"),QString("patrol"),QString("one_shot_baseline")}) {
            shutdownIsolatedCapturePool();task.captureOnly=mode=="calibration";
            const int iterations=mode=="one_shot_baseline"?baseline:count;
            for(int i=0;i<iterations;++i) {
                const auto requested=QDateTime::currentDateTimeUtc();QElapsedTimer timer;timer.start();
                auto result=mode=="one_shot_baseline"?oneShot(task):isolatedCapture(task,cancelled);
                const double acquisitionMs=timer.nsecsElapsed()/1e6;
                double copyMs=0;
                if(task.captureOnly && result.ok()){QElapsedTimer copy;copy.start();auto retained=result.frame.clone();copyMs=copy.nsecsElapsed()/1e6;}
                QJsonObject sample{{"path",mode},{"sample",i+1},{"cold",i==0 || mode=="one_shot_baseline"},
                    {"requested_at",requested.toString(Qt::ISODateWithMs)},{"acquisition_ms",acquisitionMs},{"calibration_copy_ms",copyMs},
                    {"frame_ready_ms",acquisitionMs+copyMs},{"ok",result.ok()},{"failure",captureFailureName(result.failure)},
                    {"width",result.frame.cols},{"height",result.frame.rows},{"encoded_bytes",double(result.encodedOriginal.size())},
                    {"method",result.message=="ONVIF_HTTP"?"ONVIF_HTTP":result.ok()?"RTSP":"FAILED"},{"stages",result.timings}};
                samples.append(sample);save();
                QTextStream(stdout)<<mode<<" #"<<i+1<<" "<<(result.ok()?"OK":"FAILED")<<" "<<acquisitionMs<<" ms "<<result.frame.cols<<"x"<<result.frame.rows<<Qt::endl;
                if(!result.ok())throw std::runtime_error("Live capture failed; partial timing report saved");
                QThread::msleep(250);
            }
        }
        report["finished_at"]=QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);save();return 0;
    } catch(const std::exception& error){QTextStream(stderr)<<error.what()<<Qt::endl;return 1;}
}
