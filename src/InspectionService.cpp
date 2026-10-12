#include "InspectionService.hpp"
#include "FileCameraSource.hpp"
#include "MySqlRepository.hpp"
#include "DeviceCodec.hpp"
#include <QTemporaryFile>
#include <QFutureWatcher>
#include <QtConcurrent/QtConcurrent>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QSaveFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QUuid>
#include <QUrl>
#include <algorithm>
#include <stdexcept>

namespace {
CameraSourcePtr createSource(const InspectionTask& task) {
    const auto& dev = task.device;
    if (dev.protocolType == CameraProtocolType::LOCAL_FILE)
        return std::make_shared<FileCameraSource>(task.localImagePath.toStdString());
    if (dev.protocolType == CameraProtocolType::ONVIF_RTSP_STREAM) {
        ONVIFDeviceInfo info;
        info.ipAddress = dev.ipOrSn.toStdString();
        info.port = dev.port;
        info.username = dev.username.toStdString();
        info.password = dev.password.toStdString();
        QUrl uri(dev.rtspUrl);
        if (dev.rtspUrl.isEmpty()) {
            uri.setScheme("rtsp");
            uri.setHost(dev.ipOrSn);
            uri.setPort(dev.rtspPort > 0 ? dev.rtspPort : 554);
            uri.setPath("/h264/ch1/main/av_stream");
        }
        if (uri.userName().isEmpty() && !dev.username.isEmpty()) {
            uri.setUserName(dev.username);
            uri.setPassword(dev.password);
        }
        info.rtspUri = uri.toEncoded().toStdString();
        return std::make_shared<ONVIFDeviceManager>(info);
    }
    // Demo GB28181 / SDK sources must not supply production readings.
    return nullptr;
}

struct CapturedTask {
    InspectionTask task;
    InspectionOutcome outcome;
    cv::Mat frame;
};

CapturedTask capture(const InspectionTask& task, const InspectionService::SourceFactory& factory,
                     const std::shared_ptr<std::atomic<bool>>& cancel) {
    CapturedTask data;
    data.task = task;
    data.outcome.taskId = task.taskId;
    data.outcome.cameraId = task.device.cameraId;
    data.outcome.inputKind = task.inputKind;
    data.outcome.parentTaskId = task.parentTaskId;
    data.outcome.deviceSnapshot = deviceToJson(task.device);
    data.outcome.result.runMode = task.runMode;
    if (cancel->load()) return data;
    try {
        if (!task.databaseOriginalTaskId.isEmpty()) {
            if (task.inputKind != InspectionInputKind::HistoricalImage || task.parentTaskId != task.databaseOriginalTaskId)
                throw std::runtime_error("HISTORICAL_IDENTITY_MISMATCH");
            const auto stored = MySqlRepository().original(task.databaseOriginalTaskId);
            if (stored.inspection.cameraId != task.device.cameraId || stored.inspection.capturedAt != task.originalCapturedAt)
                throw std::runtime_error("HISTORICAL_IDENTITY_MISMATCH");
            std::vector<uchar> bytes(stored.bytes.begin(), stored.bytes.end());
            data.frame = cv::imdecode(bytes, cv::IMREAD_COLOR);
            if (data.frame.empty() || data.frame.cols != stored.width || data.frame.rows != stored.height)
                throw std::runtime_error("ORIGINAL_DECODE_FAILED");
            if (task.useHistoricalConfiguration) {
                data.task.device = stored.inspection.device;
                data.task.historicalProfile = stored.inspection.profile;
            }
            data.outcome.deviceSnapshot = deviceToJson(data.task.device);
            data.outcome.capturedAt = stored.inspection.capturedAt;
            data.outcome.capturedFrame = data.frame;
            return data;
        }
        if (task.inputKind != InspectionInputKind::CameraSnapshot) {
            if (task.localImagePath.isEmpty() ||
                (task.inputKind == InspectionInputKind::HistoricalImage &&
                 (task.parentTaskId.isEmpty() || !task.originalCapturedAt.isValid()))) {
                data.outcome.errorCode = "INVALID_INPUT";
                data.outcome.errorMessage = QStringLiteral("未指定有效图片或历史任务身份");
                return data;
            }
            data.frame = cv::imread(task.localImagePath.toStdString());
            if (data.frame.empty()) {
                data.outcome.errorCode = "IMAGE_READ_FAILED";
                data.outcome.errorMessage = QStringLiteral("指定图片无法读取，未使用其他图像替代");
            } else {
                if (task.inputKind == InspectionInputKind::HistoricalImage) {
                    QFile metadata(QFileInfo(task.localImagePath).dir().filePath("result.json"));
                    const bool opened = metadata.open(QIODevice::ReadOnly);
                    const auto record = opened ? QJsonDocument::fromJson(metadata.readAll()).object() : QJsonObject();
                    const auto storedTime = QDateTime::fromString(record["captured_at"].toString(), Qt::ISODateWithMs);
                    const auto storedPath = QFileInfo(record["original_path"].toString()).canonicalFilePath();
                    if (record["task_id"].toString() != task.parentTaskId ||
                        record["camera_id"].toString() != task.device.cameraId ||
                        storedTime != task.originalCapturedAt || storedPath.isEmpty() ||
                        storedPath != QFileInfo(task.localImagePath).canonicalFilePath()) {
                        data.frame.release();
                        data.outcome.errorCode = "HISTORICAL_IDENTITY_MISMATCH";
                        data.outcome.errorMessage = QStringLiteral("历史原图与任务、设备或采集时间不匹配");
                        return data;
                    }
                }
                data.outcome.capturedAt = task.inputKind == InspectionInputKind::HistoricalImage
                    ? task.originalCapturedAt.toUTC() : QDateTime::currentDateTimeUtc();
                data.outcome.capturedFrame = data.frame;
            }
            return data;
        }
        if (task.device.protocolType == CameraProtocolType::LOCAL_FILE) {
            data.outcome.errorCode = "INVALID_INPUT";
            data.outcome.errorMessage = QStringLiteral("实时抓拍不能使用本地图像设备");
            return data;
        }
        auto source = factory(task);
        if (!source) {
            data.outcome.errorCode = "UNSUPPORTED_PROTOCOL";
            data.outcome.errorMessage = QStringLiteral("协议尚未接入生产采集服务");
        } else {
            const auto acquired = source->captureSnapshot();
            data.frame = acquired.frame;
            source->close();
            if (!acquired.ok()) {
                data.frame.release();
                data.outcome.errorCode = captureFailureName(acquired.failure == CaptureFailure::None
                    ? CaptureFailure::NoFrame : acquired.failure);
                data.outcome.errorMessage = QString::fromStdString(acquired.message);
            } else {
                data.outcome.capturedAt = QDateTime::fromMSecsSinceEpoch(
                    std::chrono::duration_cast<std::chrono::milliseconds>(acquired.capturedAt.time_since_epoch()).count(), Qt::UTC);
                data.outcome.capturedFrame = data.frame;
            }
        }
    } catch (const std::exception& e) {
        data.frame.release();
        data.outcome.errorCode = "CAPTURE_EXCEPTION";
        data.outcome.errorMessage = QString::fromUtf8(e.what());
    }
    return data;
}

InspectionOutcome recognize(CapturedTask data, const std::shared_ptr<std::atomic<bool>>& cancel) {
    auto& out = data.outcome;
    if (cancel->load() || !out.errorCode.isEmpty()) return out;
    try {
        QJsonObject frozen = data.task.historicalProfile;
        if (frozen.isEmpty()) {
            QFile file(data.task.profilePath);
            if (file.open(QIODevice::ReadOnly)) frozen = QJsonDocument::fromJson(file.readAll()).object();
        }
        out.profileSnapshot = frozen;
        if (data.task.captureOnly) return out;
        GaugeProfile profile;
        const auto& dev = data.task.device;
        QTemporaryFile profileFile;
        const auto profileBytes = QJsonDocument(frozen).toJson();
        if (frozen.isEmpty() || !profileFile.open() || profileFile.write(profileBytes) != profileBytes.size()) {
            out.errorCode = "PROFILE_INVALID";
            out.errorMessage = QStringLiteral("无法冻结仪表配置");
            return out;
        }
        profileFile.flush();
        if (!profile.loadFromFile(profileFile.fileName().toStdString(), dev.defaultProfileId.toStdString())) {
            out.errorCode = "PROFILE_INVALID";
            out.errorMessage = QStringLiteral("无法加载仪表配置");
            return out;
        }
        profile.gauge_id = dev.cameraId.toStdString();
        profile.perspective_correction_enabled = dev.perspectiveEnabled;
        profile.perspective_pts = {
            cv::Point2f(dev.pt0_x, dev.pt0_y), cv::Point2f(dev.pt1_x, dev.pt1_y),
            cv::Point2f(dev.pt2_x, dev.pt2_y), cv::Point2f(dev.pt3_x, dev.pt3_y)};
        profile.center = cv::Point2f(dev.centerX, dev.centerY);
        profile.pointerBaseRadius = dev.pointerBaseRadius;
        profile.calibrated_width = dev.calibratedWidth;
        profile.calibrated_height = dev.calibratedHeight;
        profile.auto_detect_roi = false;
        profile.save_debug_images = false; // Shared debug filenames are unsafe under concurrency.
        if (frozen.contains("_runtime_enable_preprocessing"))
            profile.enable_steps_4_to_9 = frozen["_runtime_enable_preprocessing"].toBool();
        if (data.task.overridePreprocessing) profile.enable_steps_4_to_9 = data.task.enablePreprocessing;
        out.profileSnapshot["_runtime_enable_preprocessing"] = profile.enable_steps_4_to_9;
        GaugeReaderEngine engine;
        out.recognitionPerformed = true;
        out.result = engine.process(data.frame, profile, data.task.runMode);
        if (!out.result.success) {
            out.errorCode = "RECOGNITION_FAILED";
            out.errorMessage = QString::fromStdString(out.result.errorMessage);
        }
    } catch (const std::exception& e) {
        out.errorCode = "RECOGNITION_EXCEPTION";
        out.errorMessage = QString::fromUtf8(e.what());
    }
    return out;
}

InspectionOutcome persist(InspectionOutcome out, const InspectionTask& task,
                          const std::shared_ptr<std::atomic<bool>>& cancel) {
    if (cancel->load()) return out;
    try {
        // UUID folder prevents overwrites and path traversal through device IDs.
        const QString folder = QDir(task.outputDirectory).filePath(out.taskId);
        if (!QDir().mkpath(folder)) throw std::runtime_error("Cannot create evidence directory");
        auto saveImage = [&](const cv::Mat& mat, const QString& name, QString& path) {
            if (mat.empty()) return;
            std::vector<uchar> bytes;
            if (!cv::imencode(".png", mat, bytes)) throw std::runtime_error("Image encoding failed");
            const QString target = QDir(folder).filePath(name);
            QSaveFile file(target);
            if (!file.open(QIODevice::WriteOnly) ||
                file.write(reinterpret_cast<const char*>(bytes.data()), qint64(bytes.size())) != qint64(bytes.size()) ||
                !file.commit()) throw std::runtime_error("Evidence write failed");
            path = target;
        };
        // Acquisition evidence is independent of diagnostic rendering. Keep it even
        // when recognition/configuration fails, and never replace it with another frame.
        saveImage(out.capturedFrame, "original.png", out.originalPath);
        saveImage(out.result.step2_rectified, "rectified.png", out.rectifiedPath);
        saveImage(out.result.step4_recognition, "recognition.png", out.recognitionPath);
        QJsonArray debugSteps;
        if (task.runMode == GaugeRunMode::Debug) {
            for (const auto& step : out.result.intermediateSteps) {
                QString path;
                saveImage(step.image, QString("step_%1.png").arg(step.stepNumber, 2, 10, QChar('0')), path);
                QJsonObject entry;
                entry["number"] = step.stepNumber;
                entry["title"] = QString::fromStdString(step.title);
                entry["path"] = path;
                debugSteps.append(entry);
            }
            QString hudPath;
            saveImage(out.result.diagnosticHudImage, "diagnostic_hud.png", hudPath);
        }
        QJsonObject record;
        record["mode"] = gaugeRunModeName(task.runMode);
        record["input_kind"] = inspectionInputName(task.inputKind);
        record["parent_task_id"] = task.parentTaskId;
        record["processed_at"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        record["recognition_performed"] = out.recognitionPerformed;
        record["device"] = out.deviceSnapshot;
        record["profile_snapshot"] = out.profileSnapshot;
        record["original_path"] = out.originalPath;
        record["review_image_available"] = !out.originalPath.isEmpty();
        record["recognition_path"] = out.recognitionPath;
        record["debug_steps"] = debugSteps;
        record["center_inconsistent"] = out.result.centerInconsistent;
        record["center_offset_px"] = out.result.centerOffsetDist;
        record["warning"] = QString::fromStdString(out.result.warningMessage);
        if (task.runMode == GaugeRunMode::Debug)
            record["diagnostics"] = QString::fromStdString(out.result.detailedDebugInfo);
        record["task_id"] = out.taskId;
        record["camera_id"] = out.cameraId;
        record["captured_at"] = out.capturedAt.toString(Qt::ISODateWithMs);
        record["success"] = out.errorCode.isEmpty() && out.result.success;
        record["value"] = out.result.success ? QJsonValue(out.result.value) : QJsonValue(QJsonValue::Null);
        record["unit"] = QString::fromStdString(out.result.unit);
        record["confidence"] = out.result.confidence;
        record["error_code"] = out.errorCode;
        record["error_message"] = out.errorMessage.isEmpty() ? QString::fromStdString(out.result.errorMessage) : out.errorMessage;
        const QByteArray json = QJsonDocument(record).toJson();
        QSaveFile file(QDir(folder).filePath("result.json"));
        if (!file.open(QIODevice::WriteOnly) || file.write(json) != json.size() || !file.commit())
            throw std::runtime_error("Result write failed");
    } catch (const std::exception& e) {
        out.errorCode = "STORAGE_FAILED";
        out.errorMessage = QString::fromUtf8(e.what());
    }
    if (DatabaseSettings::fromEnvironment().enabled && out.errorCode != "STORAGE_FAILED") {
        QString pendingPath;
        try {
            const auto envelope = MySqlRepository::inspectionEnvelope(task, out);
            pendingPath = MySqlRepository::queueInspection(envelope, task.outputDirectory);
            out.databasePending = true;
            MySqlRepository().saveInspection(envelope);
            out.databaseSaved = true;
            out.databasePending = false;
            if (!QFile::remove(pendingPath)) out.databaseError = QStringLiteral("入库成功，待同步文件清理失败；重试不会重复插入");
        } catch (const std::exception& e) {
            out.databaseError = QString::fromUtf8(e.what());
        }
    }
    // Evidence is on disk; retain scalar results rather than full per-device pipelines.
    if (task.returnImages) return out; // A single GUI task may display its outputs; patrol remains compact.
    GaugeResult compact;
    compact.runMode = task.runMode;
    compact.success = out.result.success;
    compact.value = out.result.value;
    compact.unit = out.result.unit;
    compact.pointer_angle_deg = out.result.pointer_angle_deg;
    compact.confidence = out.result.confidence;
    compact.centerInconsistent = out.result.centerInconsistent;
    compact.centerOffsetDist = out.result.centerOffsetDist;
    compact.warningMessage = out.result.warningMessage;
    compact.errorMessage = out.result.errorMessage;
    compact.detailedDebugInfo = out.result.detailedDebugInfo;
    out.result = std::move(compact);
    out.capturedFrame.release();
    return out;
}
}

CameraSourcePtr InspectionService::createCameraSource(const InspectionTask& task) {
    return createSource(task);
}

InspectionService::InspectionService(QObject* parent, int maxConcurrent, SourceFactory factory)
    : QObject(parent), m_factory(factory ? std::move(factory) : SourceFactory(createSource)),
      m_limit(std::max(1, maxConcurrent)) {
    qRegisterMetaType<InspectionOutcome>();
    m_capturePool.setMaxThreadCount(m_limit);
    m_recognitionPool.setMaxThreadCount(m_limit);
    m_storagePool.setMaxThreadCount(1);
}

InspectionService::~InspectionService() {
    stop();
    m_capturePool.waitForDone();
    m_recognitionPool.waitForDone();
    m_storagePool.waitForDone();
}

bool InspectionService::start(const QList<InspectionTask>& tasks) {
    if (m_running || tasks.isEmpty()) return false;
    m_tasks = tasks;
    for (auto& task : m_tasks) task.taskId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_next = m_active = m_completed = 0;
    m_cancel = std::make_shared<std::atomic<bool>>(false);
    m_running = true;
    dispatch();
    return true;
}

void InspectionService::stop() {
    if (m_cancel) m_cancel->store(true);
}

void InspectionService::retryDatabaseWrites(const QString& directory) {
    if (m_databaseRetryRunning || !DatabaseSettings::fromEnvironment().enabled) return;
    m_databaseRetryRunning = true;
    auto* watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher]() {
        m_databaseRetryRunning = false;
        emit databaseSyncFinished(watcher->result());
        watcher->deleteLater();
    });
    watcher->setFuture(QtConcurrent::run(&m_storagePool, [directory]() { return MySqlRepository::retryPending(directory); }));
}

void InspectionService::exportImage(const cv::Mat& frame, const QString& path) {
    auto* watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher, path]() {
        const auto error = watcher->result();
        watcher->deleteLater();
        emit imageExported(path, error);
    });
    watcher->setFuture(QtConcurrent::run(&m_storagePool, [frame, path]() -> QString {
        try {
            const QString suffix = QFileInfo(path).suffix();
            std::vector<uchar> bytes;
            if (frame.empty() || !cv::imencode(("." + suffix).toStdString(), frame, bytes))
                return QStringLiteral("图像编码失败");
            QSaveFile file(path);
            if (!file.open(QIODevice::WriteOnly) ||
                file.write(reinterpret_cast<const char*>(bytes.data()), qint64(bytes.size())) != qint64(bytes.size()) || !file.commit())
                return QStringLiteral("图片写入失败");
            return QString();
        } catch (const std::exception& e) { return QString::fromUtf8(e.what()); }
    }));
}

void InspectionService::dispatch() {
    while (!m_cancel->load() && m_active < m_limit && m_next < m_tasks.size()) {
        const auto task = m_tasks[m_next++];
        const auto cancel = m_cancel;
        ++m_active;
        emit taskStarted(task.taskId, task.device.cameraId);
        auto* captureWatcher = new QFutureWatcher<CapturedTask>(this);
        connect(captureWatcher, &QFutureWatcher<CapturedTask>::finished, this, [this, captureWatcher, task, cancel]() {
            auto captured = captureWatcher->result();
            captureWatcher->deleteLater();
            if (cancel->load()) { complete(captured.outcome); return; }
            auto* recognitionWatcher = new QFutureWatcher<InspectionOutcome>(this);
            connect(recognitionWatcher, &QFutureWatcher<InspectionOutcome>::finished, this, [this, recognitionWatcher, task, cancel]() {
                auto outcome = recognitionWatcher->result();
                recognitionWatcher->deleteLater();
                if (cancel->load()) { complete(outcome); return; }
                auto* storageWatcher = new QFutureWatcher<InspectionOutcome>(this);
                connect(storageWatcher, &QFutureWatcher<InspectionOutcome>::finished, this, [this, storageWatcher]() {
                    auto outcome = storageWatcher->result();
                    storageWatcher->deleteLater();
                    complete(std::move(outcome));
                });
                storageWatcher->setFuture(QtConcurrent::run(&m_storagePool, [outcome, task, cancel]() {
                    return persist(outcome, task, cancel);
                }));
            });
            recognitionWatcher->setFuture(QtConcurrent::run(&m_recognitionPool, [captured, cancel]() {
                return recognize(captured, cancel);
            }));
        });
        const auto factory = m_factory;
        captureWatcher->setFuture(QtConcurrent::run(&m_capturePool, [task, factory, cancel]() {
            return capture(task, factory, cancel);
        }));
    }
    if (m_active == 0 && (m_cancel->load() || m_next == m_tasks.size())) {
        const bool cancelled = m_cancel->load();
        m_running = false;
        m_tasks.clear();
        emit finished(cancelled);
    }
}

void InspectionService::complete(InspectionOutcome outcome) {
    --m_active;
    if (!m_cancel->load()) {
        ++m_completed;
        emit taskFinished(outcome);
        emit progressChanged(m_completed, m_tasks.size());
    }
    dispatch();
}
