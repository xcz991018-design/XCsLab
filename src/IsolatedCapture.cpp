#include "IsolatedCapture.hpp"
#include "DeviceCodec.hpp"
#include "EvidenceArchive.hpp"
#include <QCoreApplication>
#include <QProcess>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>
#include <QThread>
#include <cstdio>
#include <QCryptographicHash>
#include <array>
#include <list>
#include <mutex>
#include <condition_variable>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
FrameCaptureResult readWorkerResult(const QString& folder, const QJsonObject& json) {
    FrameCaptureResult result;
    result.timings=json["timings"].toObject();
    const int failure = json.value("failure").toInt(-1);
    if (failure < 0 || failure > static_cast<int>(CaptureFailure::WorkerFailed)) {
        result.failure = CaptureFailure::WorkerFailed;
        return result;
    }
    result.failure = static_cast<CaptureFailure>(failure);
    result.message = json.value("message").toString().toStdString();
    if (result.failure == CaptureFailure::None) {
        const int sequence = json.value("sequence").toInt();
        const auto suffix = json["original_suffix"].toString(".png");
        if (suffix!=".png" && suffix!=".jpg") { result.failure=CaptureFailure::WorkerFailed; return result; }
        QFile image(folder + (sequence > 0 ? QString("/frame_%1").arg(sequence) : QString("/frame")) + suffix);
        if (image.open(QIODevice::ReadOnly)) {
            const auto bytes = image.readAll();
            result.frame = cv::imdecode(std::vector<uchar>(bytes.begin(), bytes.end()), cv::IMREAD_COLOR);
            if (json["original_preserved"].toBool()) result.encodedOriginal.assign(bytes.begin(), bytes.end());
        }
        if (result.frame.empty()) result.failure = CaptureFailure::NoFrame;
        result.capturedAt = std::chrono::system_clock::time_point(std::chrono::milliseconds(
            json.value("captured_at_ms").toVariant().toLongLong()));
    }
    return result;
}
bool saveWorkerResult(const QString& folder, const FrameCaptureResult& result, int sequence) {
    const auto suffix = EvidenceArchive::imageSuffix(result.encodedOriginal);
    const bool preserved = !suffix.isEmpty();
    if (result.ok()) {
        std::vector<uchar> bytes = preserved ? result.encodedOriginal : std::vector<uchar>();
        if (!preserved && !cv::imencode(".png", result.frame, bytes,{cv::IMWRITE_PNG_COMPRESSION,1})) return false;
        QSaveFile image(folder + (sequence > 0 ? QString("/frame_%1").arg(sequence) : QString("/frame")) + (preserved ? suffix : ".png"));
        if (!image.open(QIODevice::WriteOnly) || image.write(reinterpret_cast<const char*>(bytes.data()), bytes.size()) != qint64(bytes.size()) || !image.commit()) return false;
    }
    QJsonObject json;
    json["timings"] = result.timings;
    json["original_suffix"] = preserved ? suffix : ".png";
    json["original_preserved"] = preserved;
    json["sequence"] = sequence;
    json["failure"] = static_cast<int>(result.failure);
    json["message"] = QString::fromStdString(result.message);
    json["captured_at_ms"] = static_cast<double>(std::chrono::duration_cast<std::chrono::milliseconds>(result.capturedAt.time_since_epoch()).count());
    QSaveFile output(folder + (sequence > 0 ? QString("/result_%1.json").arg(sequence) : QString("/result.json")));
    const auto bytes = QJsonDocument(json).toJson();
    return output.open(QIODevice::WriteOnly) && output.write(bytes) == bytes.size() && output.commit();
}
}

struct IsolatedPreview::State {
    QTemporaryDir directory;
    QProcess worker;
    int sequence{0};
    ~State() { if (worker.state() != QProcess::NotRunning) { worker.kill(); worker.waitForFinished(1000); } }
};
IsolatedPreview::IsolatedPreview() : m_state(std::make_unique<State>()) {}
IsolatedPreview::~IsolatedPreview() = default;
FrameCaptureResult IsolatedPreview::read(const InspectionTask& task, const std::shared_ptr<std::atomic<bool>>& cancel) {
    FrameCaptureResult result;
    auto& state = *m_state;
    QElapsedTimer timer;
    timer.start();
    const int deadline = qBound(100, task.captureTimeoutMs, 120000);
    auto fail = [&](CaptureFailure failure, const char* message) {
        state.worker.kill();
        state.worker.waitForFinished(1000);
        result.failure = failure;
        result.message = message;
        return result;
    };
    if (cancel->load()) return fail(CaptureFailure::Cancelled, "Preview cancelled");
    if (!state.directory.isValid()) return fail(CaptureFailure::WorkerFailed, "Preview workspace unavailable");
    if (state.sequence == 0) {
        state.worker.setProgram(QCoreApplication::applicationFilePath());
        state.worker.setArguments({"--capture-preview-worker", state.directory.path()});
        state.worker.setStandardOutputFile(QProcess::nullDevice());
        state.worker.setStandardErrorFile(QProcess::nullDevice());
        state.worker.start();
        while (state.worker.state() == QProcess::Starting) {
            if (cancel->load()) return fail(CaptureFailure::Cancelled, "Preview cancelled");
            if (timer.elapsed() >= deadline) return fail(CaptureFailure::Timeout, "Preview startup deadline exceeded");
            state.worker.waitForStarted(25);
        }
        if (state.worker.state() != QProcess::Running) return fail(CaptureFailure::WorkerFailed, "Preview worker failed to start");
        state.worker.write(QJsonDocument(deviceToJson(task.device, true)).toJson(QJsonDocument::Compact) + '\n');
    }
    const int sequence = ++state.sequence;
    state.worker.write(QByteArray::number(sequence) + '\n');
    while (timer.elapsed() < deadline) {
        if (cancel->load()) return fail(CaptureFailure::Cancelled, "Preview cancelled");
        const QString metadataPath = state.directory.filePath(QString("result_%1.json").arg(sequence));
        QFile metadata(metadataPath);
        if (metadata.open(QIODevice::ReadOnly)) {
            const auto json = QJsonDocument::fromJson(metadata.readAll()).object();
            if (json.value("sequence").toInt() == sequence) {
                result = readWorkerResult(state.directory.path(), json);
                metadata.close();
                QFile::remove(metadataPath);
                const auto suffix=json["original_suffix"].toString(".png");
                if (suffix==".jpg" || suffix==".png") QFile::remove(state.directory.filePath(QString("frame_%1").arg(sequence)+suffix));
                return result;
            }
        }
        if (state.worker.state() == QProcess::NotRunning) return fail(CaptureFailure::WorkerFailed, "Preview worker exited unexpectedly");
        state.worker.waitForFinished(10);
    }
    return fail(CaptureFailure::Timeout, "Preview frame deadline exceeded");
}

namespace {
QByteArray connectionKey(const QJsonObject& device) {
    QJsonObject connection;
    for(const auto* key:{"ip_or_sn","port","rtsp_port","rtsp_url","protocol_type","username","password"})
        connection[key]=device[key];
    return QCryptographicHash::hash(QJsonDocument(connection).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256);
}
struct CaptureSlot {
    QThread thread;
    QObject context;
    QProcess* process{nullptr};
    std::unique_ptr<QTemporaryDir> directory;
    int sequence{0};
    bool busy{false};
    QByteArray key;
#ifdef _WIN32
    HANDLE job{nullptr};
    bool containProcess() {
        job=CreateJobObjectW(nullptr,nullptr);
        if(!job)return false;
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if(!SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limits,sizeof(limits)))return false;
        HANDLE child=OpenProcess(PROCESS_SET_QUOTA|PROCESS_TERMINATE,FALSE,DWORD(process->processId()));
        if(!child)return false;
        const bool assigned=AssignProcessToJobObject(job,child)!=0;CloseHandle(child);return assigned;
    }
#else
    bool containProcess(){return true;}
#endif
    CaptureSlot(){context.moveToThread(&thread);thread.start();}
    void reset(){
        if(process){process->kill();process->waitForFinished(1000);delete process;process=nullptr;}
#ifdef _WIN32
        if(job){CloseHandle(job);job=nullptr;}
#endif
        directory.reset();sequence=0;
    }
    ~CaptureSlot(){auto* owner=QThread::currentThread();QMetaObject::invokeMethod(&context,[&]{reset();context.moveToThread(owner);},Qt::BlockingQueuedConnection);thread.quit();thread.wait();}
};
class CapturePool {
    // Shared by every InspectionService in this application, not one pool per camera.
    std::array<CaptureSlot,4> workers;
    std::mutex mutex;
    std::condition_variable available;
    int waiting{0};
public:
    void shutdown() {
        for(auto& worker:workers)
            QMetaObject::invokeMethod(&worker.context,[&worker]{worker.reset();},Qt::BlockingQueuedConnection);
    }
    FrameCaptureResult run(const InspectionTask& task,const std::shared_ptr<std::atomic<bool>>& cancel,bool prepare) {
        FrameCaptureResult result;
        QElapsedTimer timer;timer.start();
        const int deadline=qBound(100,task.captureTimeoutMs,120000);
        auto request=deviceToJson(task.device,true);
        const auto key=connectionKey(request);
        CaptureSlot* selected=nullptr;
        {
            std::unique_lock<std::mutex> lock(mutex);
            if(!prepare)++waiting;
            while(!selected && !cancel->load() && timer.elapsed()<deadline) {
                int active=0;bool same=false;
                for(const auto& slot:workers){active+=slot.busy;same|=slot.busy && slot.key==key;}
                // Speculative work never queues ahead of a real capture; reserve one slot.
                if(prepare && (waiting || active>=3 || same))break;
                if(!same) {
                    const unsigned preferred=qHash(key)%workers.size();
                    for(unsigned n=0;n<workers.size();++n) {
                        auto& slot=workers[(preferred+n)%workers.size()];
                        if(!slot.busy){selected=&slot;slot.busy=true;slot.key=key;break;}
                    }
                }
                if(prepare || selected)break;
                available.wait_for(lock,std::chrono::milliseconds(20));
            }
            if(!prepare)--waiting;
        }
        if(!selected){result.failure=cancel->load()?CaptureFailure::Cancelled:CaptureFailure::Timeout;result.message="Capture pool wait cancelled or expired";return result;}
        const double queueMs=timer.elapsed();
        auto release=[&]{std::lock_guard<std::mutex> lock(mutex);selected->busy=false;available.notify_all();};
        request["_prefer_onvif_snapshot"]=task.preferOnvifSnapshot;
        request["_require_onvif_snapshot"]=task.requireOnvifSnapshot;
        request["_allow_lower_resolution_http"]=task.allowLowerResolutionHttpSnapshot;
        request["_prepare_only"]=prepare;
        QMetaObject::invokeMethod(&selected->context,[&]{
            auto& slot=*selected;
            auto fail=[&](CaptureFailure code,const char* message){result.failure=code;result.message=message;slot.reset();};
            try {
                const auto startup=timer.elapsed();
                if(slot.process && slot.process->state()==QProcess::NotRunning)slot.reset();
                if(!slot.process) {
                    slot.directory=std::make_unique<QTemporaryDir>();
                    if(!slot.directory->isValid()){fail(CaptureFailure::WorkerFailed,"Capture workspace unavailable");return;}
                    slot.process=new QProcess;
                    slot.process->setStandardOutputFile(QProcess::nullDevice());slot.process->setStandardErrorFile(QProcess::nullDevice());
                    slot.process->start(QCoreApplication::applicationFilePath(),{"--capture-pool-worker",slot.directory->path()});
                    while(slot.process->state()==QProcess::Starting && !cancel->load() && timer.elapsed()<deadline)slot.process->waitForStarted(20);
                    if(slot.process->state()!=QProcess::Running){fail(cancel->load()?CaptureFailure::Cancelled:timer.elapsed()>=deadline?CaptureFailure::Timeout:CaptureFailure::WorkerFailed,"Capture pool worker startup failed");return;}
                    if(!slot.containProcess()){fail(CaptureFailure::WorkerFailed,"Unable to contain capture worker lifetime");return;}
                }
                const double startupMs=timer.elapsed()-startup;
                const int sequence=++slot.sequence;request["_sequence"]=sequence;
                const auto bytes=QJsonDocument(request).toJson(QJsonDocument::Compact)+'\n';
                if(slot.process->write(bytes)!=bytes.size()){fail(CaptureFailure::WorkerFailed,"Capture request write failed");return;}
                const auto metadataPath=slot.directory->filePath(QString("result_%1.json").arg(sequence));
                while(timer.elapsed()<deadline && !cancel->load()) {
                    if(prepare) {
                        bool demanded;{std::lock_guard<std::mutex> lock(mutex);demanded=waiting>0;}
                        if(demanded){fail(CaptureFailure::Cancelled,"Prewarm yielded to capture demand");return;}
                    }
                    slot.process->waitForFinished(10);
                    QFile metadata(metadataPath);
                    if(metadata.open(QIODevice::ReadOnly)) {
                        const auto json=QJsonDocument::fromJson(metadata.readAll()).object();metadata.close();
                        if(json["sequence"].toInt()!=sequence){fail(CaptureFailure::WorkerFailed,"Invalid capture response sequence");return;}
                        const auto decodeStarted=timer.elapsed();
                        if(prepare){result.failure=static_cast<CaptureFailure>(json["failure"].toInt());result.timings=json["timings"].toObject();}
                        else result=readWorkerResult(slot.directory->path(),json);
                        result.timings["transfer_decode_ms"]=double(timer.elapsed()-decodeStarted);
                        result.timings["pool_queue_ms"]=queueMs;result.timings["worker_start_ms"]=startupMs;
                        result.timings["worker_pid"]=double(slot.process->processId());
                        QFile::remove(metadataPath);
                        const auto suffix=json["original_suffix"].toString();
                        if(suffix==".jpg"||suffix==".png")QFile::remove(slot.directory->filePath(QString("frame_%1").arg(sequence)+suffix));
                        if(cancel->load() || timer.elapsed()>=deadline){result.frame.release();result.encodedOriginal.clear();fail(cancel->load()?CaptureFailure::Cancelled:CaptureFailure::Timeout,"Capture deadline exceeded or cancelled");}
                        return;
                    }
                    if(slot.process->state()==QProcess::NotRunning){fail(CaptureFailure::WorkerFailed,"Capture pool worker exited");return;}
                }
                fail(cancel->load()?CaptureFailure::Cancelled:CaptureFailure::Timeout,"Capture deadline exceeded or cancelled");
            }catch(...){fail(CaptureFailure::WorkerFailed,"Capture pool failure");}
        },Qt::BlockingQueuedConnection);
        release();
        return result;
    }
};
std::atomic<CapturePool*> initializedPool{nullptr};
CapturePool& capturePool(){static CapturePool pool;initializedPool.store(&pool);return pool;}
}
void shutdownIsolatedCapturePool(){if(auto* pool=initializedPool.load())pool->shutdown();}
FrameCaptureResult isolatedCapture(const InspectionTask& task,const std::shared_ptr<std::atomic<bool>>& cancel) {
    return capturePool().run(task,cancel,false);
}
void prewarmIsolatedCapture(const InspectionTask& task,const std::shared_ptr<std::atomic<bool>>& cancel) {
    if(task.inputKind==InspectionInputKind::CameraSnapshot && task.device.protocolType==CameraProtocolType::ONVIF_RTSP_STREAM)
        capturePool().run(task,cancel,true);
}

int runCaptureWorker(int argc, char** argv) {
    const bool pooled = argc == 3 && QString::fromLocal8Bit(argv[1]) == "--capture-pool-worker";
    const bool preview = argc == 3 && QString::fromLocal8Bit(argv[1]) == "--capture-preview-worker";
    if (!pooled && !preview && (argc != 3 || QString::fromLocal8Bit(argv[1]) != "--capture-worker")) return -1;
    // Worker launch precedes the GUI setup. Apply transport settings here too.
    qputenv("OPENCV_FFMPEG_CAPTURE_OPTIONS", "rtsp_transport;tcp");
    QCoreApplication app(argc, argv);
    QFile input;
    if (!input.open(stdin, QIODevice::ReadOnly)) return 2;
    if(pooled) {
        struct Entry {QByteArray key;CameraSourcePtr source;std::chrono::steady_clock::time_point created;};
        std::list<Entry> cache; // Metadata-only sources: never open background RTSP streams.
        while(true) {
            const auto line=input.readLine();
            if(line.isEmpty()){QThread::msleep(10);continue;}
            const auto request=QJsonDocument::fromJson(line).object();
            const int sequence=request["_sequence"].toInt();if(sequence<=0)return 2;
            InspectionTask task;task.device=deviceFromJson(request);
            task.preferOnvifSnapshot=request["_prefer_onvif_snapshot"].toBool();
            task.requireOnvifSnapshot=request["_require_onvif_snapshot"].toBool();
            task.allowLowerResolutionHttpSnapshot=request["_allow_lower_resolution_http"].toBool();
            const bool prepare=request["_prepare_only"].toBool();
            auto key=connectionKey(request);key+=task.preferOnvifSnapshot?'1':'0';key+=task.requireOnvifSnapshot?'1':'0';key+=task.allowLowerResolutionHttpSnapshot?'1':'0';
            const auto now=std::chrono::steady_clock::now();
            cache.remove_if([&](const Entry& e){return now-e.created>std::chrono::minutes(10);});
            auto found=std::find_if(cache.begin(),cache.end(),[&](const Entry& e){return e.key==key;});
            const bool hit=found!=cache.end();
            if(hit)cache.splice(cache.begin(),cache,found);
            else {cache.push_front({key,InspectionService::createCameraSource(task),now});if(cache.size()>128)cache.pop_back();}
            auto source=cache.front().source;
            FrameCaptureResult result;QElapsedTimer timer;timer.start();
            try {
                if(!source)result.failure=CaptureFailure::UnsupportedProtocol;
                else if(prepare) {
                    auto* onvif=dynamic_cast<ONVIFDeviceManager*>(source.get());
                    result.failure=onvif && onvif->prepareSnapshot()?CaptureFailure::None:CaptureFailure::OpenFailed;
                } else result=source->captureSnapshot();
                if(source)source->close();
            }catch(...){result.failure=CaptureFailure::ReadFailed;result.message="Camera acquisition failed";}
            result.timings["device_acquisition_ms"]=double(timer.elapsed());
            result.timings["metadata_cache_hit"]=hit;
            result.timings["metadata_cache_entries"]=int(cache.size());
            if(result.failure!=CaptureFailure::None)cache.pop_front();
            // Metadata-only preparation deliberately returns no image.
            if(!prepare && result.failure==CaptureFailure::None && result.frame.empty())result.failure=CaptureFailure::NoFrame;
            if(!saveWorkerResult(QString::fromLocal8Bit(argv[2]),result,sequence))return 3;
        }
    }
    const auto document = QJsonDocument::fromJson(preview ? input.readLine() : input.readAll());
    if (!document.isObject()) return 2;
    InspectionTask task;
    task.device = deviceFromJson(document.object());
    task.preferOnvifSnapshot = !preview && document.object()["_prefer_onvif_snapshot"].toBool();
    task.requireOnvifSnapshot = !preview && document.object()["_require_onvif_snapshot"].toBool();
    task.allowLowerResolutionHttpSnapshot = !preview && document.object()["_allow_lower_resolution_http"].toBool();
    FrameCaptureResult result;
    try {
        auto source = InspectionService::createCameraSource(task);
        if (preview) {
            const bool opened = source && source->open();
            while (true) {
                bool validSequence = false;
                const auto command = input.readLine();
                // Windows QFile pipes may report no available bytes between
                // requests. The parent owns shutdown and kills this process.
                if (command.isEmpty()) { QThread::msleep(10); continue; }
                const int sequence = command.trimmed().toInt(&validSequence);
                if (!validSequence) return 2;
                FrameCaptureResult acquired;
                if (!source) acquired.failure = CaptureFailure::UnsupportedProtocol;
                else if (!opened) {
                    acquired.failure = CaptureFailure::OpenFailed;
                    if (auto* onvif = dynamic_cast<ONVIFDeviceManager*>(source.get())) acquired.message = onvif->lastError();
                }
                else {
                    // The stream opens asynchronously; let the parent's deadline
                    // govern first-frame wait and disconnected-stream recovery.
                    do {
                        acquired = source->readFrame();
                        if (acquired.ok()) break;
                        if (acquired.failure != CaptureFailure::NoFrame && acquired.failure != CaptureFailure::StaleFrame) break;
                        QThread::msleep(10);
                    } while (true);
                }
                if (!saveWorkerResult(QString::fromLocal8Bit(argv[2]), acquired, sequence)) return 3;
            }
            if (source) source->close();
            return 0;
        }
        if (!source) result.failure = CaptureFailure::UnsupportedProtocol;
        else { result = source->captureSnapshot(); source->close(); }
        if (!result.ok()) result.frame.release();
        if (result.failure == CaptureFailure::None && result.frame.empty()) result.failure = CaptureFailure::NoFrame;
        const QString folder = QString::fromLocal8Bit(argv[2]);
        if (!saveWorkerResult(folder, result, 0)) return 3;
        return 0;
    } catch (...) { return 3; }
}
