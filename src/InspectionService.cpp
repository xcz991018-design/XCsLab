#include "DeviceConnection.hpp"
#include "InspectionService.hpp"
#include "IsolatedCapture.hpp"
#include "FileCameraSource.hpp"
#include "MySqlRepository.hpp"
#include "DeviceCodec.hpp"
#include "InspectionCalibration.hpp"
#include "EvidenceArchive.hpp"
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
using TimingClock = std::chrono::steady_clock;
double elapsedMs(TimingClock::time_point start) {
    return std::chrono::duration<double, std::milli>(TimingClock::now() - start).count();
}
CameraSourcePtr createSource(const InspectionTask& task) {
    const auto& dev = task.device;
    if (dev.protocolType == CameraProtocolType::LOCAL_FILE)
        return std::make_shared<FileCameraSource>(task.localImagePath.toStdString());
    if (dev.protocolType == CameraProtocolType::ONVIF_RTSP_STREAM) {
        ONVIFDeviceInfo info;
        info.preferHttpSnapshot = task.preferOnvifSnapshot || task.requireOnvifSnapshot;
        info.requireHttpSnapshot = task.requireOnvifSnapshot;
        info.ipAddress = dev.ipOrSn.toStdString();
        info.port = dev.port;
        info.username = dev.username.toStdString();
        info.password = dev.password.toStdString();
        info.xaddr.clear();
        QUrl uri(dev.rtspUrl);
        if (!dev.rtspUrl.isEmpty() && uri.userName().isEmpty() && !dev.username.isEmpty()) {
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
                     const std::shared_ptr<std::atomic<bool>>& cancel, bool isolate) {
    CapturedTask data;
    data.task = task;
    data.outcome.taskId = task.taskId;
    data.outcome.cameraId = task.device.cameraId;
    data.outcome.inputKind = task.inputKind;
    data.outcome.parentTaskId = task.parentTaskId;
    data.outcome.deviceSnapshot = deviceToJson(task.device);
    data.outcome.result.runMode = task.runMode;
    if (cancel->load()) return data;
    if (task.inputKind == InspectionInputKind::HistoricalImage && !task.captureOnly &&
        !usesHistoricalDatabaseConfiguration(task) && !hasSavedDeviceCalibration(task.historicalCalibration)) {
        data.outcome.errorCode = "HISTORICAL_CALIBRATION_REQUIRED";
        data.outcome.errorMessage = QStringLiteral(u"请先根据本次历史原图完成手动标定");
        return data;
    }
    auto applyHistoricalCalibration = [&]() {
        if (task.inputKind != InspectionInputKind::HistoricalImage || task.captureOnly ||
            usesHistoricalDatabaseConfiguration(task)) return;
        if (task.historicalCalibration["calibrated_width"].toInt() != data.frame.cols ||
            task.historicalCalibration["calibrated_height"].toInt() != data.frame.rows)
            throw std::runtime_error("HISTORICAL_CALIBRATION_DIMENSIONS_MISMATCH");
        auto device = deviceToJson(data.task.device, true);
        device["calibration"] = task.historicalCalibration;
        data.task.device = deviceFromJson(device);
        data.outcome.deviceSnapshot = deviceToJson(data.task.device);
    };
    // Historical database tasks validate their frozen device after it is loaded.
    if (task.inputKind != InspectionInputKind::HistoricalImage &&
        (task.databaseOriginalTaskId.isEmpty() || !task.useHistoricalConfiguration) &&
        !task.captureOnly && !recognitionMissingRequirements(task.device).isEmpty()) {
        data.outcome.errorCode = "DEVICE_CONFIGURATION_REQUIRED";
        data.outcome.errorMessage = recognitionMissingRequirements(task.device).join(QStringLiteral(u"；"));
        return data;
    }
    try {
        if (!task.databaseOriginalTaskId.isEmpty()) {
            if (task.inputKind != InspectionInputKind::HistoricalImage || task.parentTaskId != task.databaseOriginalTaskId)
                throw std::runtime_error("HISTORICAL_IDENTITY_MISMATCH");
            const auto stored = MySqlRepository().original(task.databaseOriginalTaskId);
            if (stored.inspection.cameraId != task.device.cameraId || stored.inspection.capturedAt != task.originalCapturedAt)
                throw std::runtime_error("HISTORICAL_IDENTITY_MISMATCH");
            std::vector<uchar> bytes(stored.bytes.begin(), stored.bytes.end());
            data.frame = cv::imdecode(bytes, cv::IMREAD_COLOR);
            data.outcome.encodedOriginal = bytes;
            if (data.frame.empty() || data.frame.cols != stored.width || data.frame.rows != stored.height)
                throw std::runtime_error("ORIGINAL_DECODE_FAILED");
            if (task.useHistoricalConfiguration) {
                data.task.device = stored.inspection.device;
                data.task.historicalProfile = stored.inspection.profile;
            }
            applyHistoricalCalibration();
            data.outcome.deviceSnapshot = deviceToJson(data.task.device);
            data.outcome.capturedAt = stored.inspection.capturedAt;
            data.outcome.capturedFrame = data.frame;
            if (usesHistoricalDatabaseConfiguration(task) && !task.captureOnly &&
                (!canRecognizeDevice(data.task.device) || data.task.historicalProfile.isEmpty())) {
                data.outcome.errorCode = "HISTORICAL_CONFIGURATION_MISSING";
                data.outcome.errorMessage = QStringLiteral(u"该历史任务未保存完整有效的标定或算法配置，无法使用历史配置重分析");
            }
            return data;
        }
        if (task.inputKind != InspectionInputKind::CameraSnapshot) {
            if (task.localImagePath.isEmpty() ||
                (task.inputKind == InspectionInputKind::HistoricalImage &&
                 (task.parentTaskId.isEmpty() || !task.originalCapturedAt.isValid()))) {
                data.outcome.errorCode = "INVALID_INPUT";
                data.outcome.errorMessage = QStringLiteral(u"未指定有效图片或历史任务身份");
                return data;
            }
            QFile originalFile(task.localImagePath);
            if (originalFile.open(QIODevice::ReadOnly) && originalFile.size() <= 32*1024*1024) {
                const auto bytes=originalFile.readAll();
                data.outcome.encodedOriginal.assign(bytes.begin(),bytes.end());
                data.frame=cv::imdecode(data.outcome.encodedOriginal,cv::IMREAD_COLOR);
            }
            if (data.frame.empty()) {
                data.outcome.errorCode = "IMAGE_READ_FAILED";
                data.outcome.errorMessage = QStringLiteral(u"指定图片无法读取，未使用其他图像替代");
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
                        data.outcome.errorMessage = QStringLiteral(u"历史原图与任务、设备或采集时间不匹配");
                        return data;
                    }
                }
                applyHistoricalCalibration();
                data.outcome.capturedAt = task.inputKind == InspectionInputKind::HistoricalImage
                    ? task.originalCapturedAt.toUTC() : QDateTime::currentDateTimeUtc();
                data.outcome.capturedFrame = data.frame;
            }
            return data;
        }
        if (task.device.protocolType == CameraProtocolType::LOCAL_FILE) {
            data.outcome.errorCode = "INVALID_INPUT";
            data.outcome.errorMessage = QStringLiteral(u"实时抓拍不能使用本地图像设备");
            return data;
        }
        if (task.requireOnvifSnapshot && task.device.protocolType!=CameraProtocolType::ONVIF_RTSP_STREAM) {
            data.outcome.errorCode="UNSUPPORTED_PROTOCOL";
            data.outcome.errorMessage=QStringLiteral(u"此操作要求设备使用 ONVIF 快照协议，请检查设备配置");
            return data;
        }
        if (!task.requireOnvifSnapshot && !task.previewSnapshot.empty()) {
            const auto age = task.previewCapturedAt.msecsTo(QDateTime::currentDateTimeUtc());
            if (!task.previewCapturedAt.isValid() || age < 0 || age >= 2000) {
                data.outcome.errorCode = "STALE_FRAME";
                data.outcome.errorMessage = QStringLiteral(u"预览帧已过期，请等待实时画面更新后重新抓拍");
                return data;
            }
            data.frame = task.previewSnapshot.clone();
            data.task.previewSnapshot.release();
            data.outcome.capturedAt = task.previewCapturedAt;
            data.outcome.capturedFrame = data.frame;
            return data;
        }
        auto source = isolate ? CameraSourcePtr() : factory(task);
        if (!source && !isolate) {
            data.outcome.errorCode = "UNSUPPORTED_PROTOCOL";
            data.outcome.errorMessage = QStringLiteral(u"协议尚未接入生产采集服务");
        } else {
            const auto acquired = isolate ? isolatedCapture(task, cancel) : source->captureSnapshot();
            data.frame = acquired.frame;
            data.outcome.encodedOriginal = acquired.encodedOriginal;
            data.outcome.captureMethod = acquired.message == "ONVIF_HTTP" ? "ONVIF_HTTP"
                : task.device.protocolType == CameraProtocolType::ONVIF_RTSP_STREAM ? "RTSP" : "OTHER";
            if (source) source->close();
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
    if (!data.task.captureOnly && !usesHistoricalDatabaseConfiguration(data.task) &&
        !recognitionMissingRequirements(data.task.device).isEmpty()) {
        out.errorCode = "DEVICE_CONFIGURATION_REQUIRED";
        out.errorMessage = recognitionMissingRequirements(data.task.device).join(QStringLiteral(u"；"));
        return out;
    }
    if (requiresInspectionCalibration(data.task)) {
        out.errorCode = "CALIBRATION_REQUIRED";
        out.errorMessage = QStringLiteral(u"请先完成设备 %1 的表计标定").arg(data.task.device.cameraId);
        return out;
    }
    try {
        const auto templateStarted = TimingClock::now();
        QJsonObject frozen = data.task.historicalProfile;
        if (frozen.isEmpty()) {
            // New devices use built-in algorithm parameters and their saved OCR
            // range. Legacy devices retain their explicit database template.
            if (data.task.captureOnly && !DatabaseSettings::fromEnvironment().enabled) return out;
            try {
                const auto id = data.task.device.defaultProfileId;
                if(id.isEmpty()) {
                    frozen=GaugeProfile::createDefault().toJson();
                    frozen["_configuration_source"]="device_ocr";
                } else {
                    const auto record = MySqlRepository().loadTemplate(id);
                    frozen = QJsonObject{{"profiles", QJsonArray{record.definition}},
                                         {"_database_template_id", record.id},
                                         {"_database_template_revision", static_cast<double>(record.revision)}};
                }
            } catch (const std::exception& e) {
                out.errorCode = "TEMPLATE_DATABASE_FAILED";
                out.errorMessage = QString::fromUtf8(e.what());
                return out;
            }
        }
        out.timings["template_load_ms"] = elapsedMs(templateStarted);
        const auto profileStarted = TimingClock::now();
        if(!usesHistoricalDatabaseConfiguration(data.task))frozen=OcrDialRange::withRange(frozen,data.task.device.meterRange);
        out.profileSnapshot = frozen;
        if (data.task.captureOnly) return out;
        GaugeProfile profile;
        const auto& dev = data.task.device;
        if (frozen.isEmpty()) {
            out.errorCode = "PROFILE_INVALID";
            out.errorMessage = QStringLiteral(u"无法冻结仪表配置");
            return out;
        }
        if (!profile.loadFromJson(frozen, dev.defaultProfileId.toStdString())) {
            out.errorCode = "PROFILE_INVALID";
            out.errorMessage = QStringLiteral(u"无法加载仪表配置");
            return out;
        }
        profile.gauge_id = dev.cameraId.toStdString();
        profile.perspective_correction_enabled = dev.perspectiveEnabled;
        profile.perspective_pts = {
            cv::Point2f(dev.pt0_x, dev.pt0_y), cv::Point2f(dev.pt1_x, dev.pt1_y),
            cv::Point2f(dev.pt2_x, dev.pt2_y), cv::Point2f(dev.pt3_x, dev.pt3_y)};
        profile.center = cv::Point2f(dev.centerX, dev.centerY);
        profile.scalePoints = dev.scalePoints;
        profile.pointerBaseRadius = dev.pointerBaseRadius;
        profile.calibrated_width = dev.calibratedWidth;
        profile.calibrated_height = dev.calibratedHeight;
        profile.auto_detect_roi = false;
        profile.save_debug_images = false; // Shared debug filenames are unsafe under concurrency.
        out.timings["profile_setup_ms"] = elapsedMs(profileStarted);
        GaugeReaderEngine engine;
        out.recognitionPerformed = true;
        out.result = engine.process(data.frame, profile, data.task.runMode);
        out.timings["engine_steps"] = out.result.stageTimings;
        out.profileSnapshot["_runtime_quality_config"] = out.result.recognitionQuality["config"];
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
    const auto snapshotDevice = out.deviceSnapshot.isEmpty() ? task.device : deviceFromJson(out.deviceSnapshot);
    out.safetyAssessment = snapshotDevice.safeWorkingRange.assess(out.result.value,
        out.recognitionPerformed && out.result.success && out.errorCode.isEmpty());
    const auto imageStarted = TimingClock::now();
    const auto processedAt = QDateTime::currentDateTimeUtc();
    const auto policy = EvidenceArchive::Policy::fromEnvironment();
    const bool anomaly = EvidenceArchive::abnormal(out);
    out.archiveMetadata = {{"schema_version",2}, {"anomaly",anomaly}, {"local_root",QFileInfo(task.outputDirectory).absoluteFilePath()},
        {"retention_days",anomaly ? policy.abnormalDays : policy.normalDays},
        {"images_expires_at",processedAt.addDays(anomaly ? policy.abnormalDays : policy.normalDays).toString(Qt::ISODateWithMs)},
        {"software",EvidenceArchive::software()}};
    QJsonArray imageManifest;
    try {
        // UUID folder prevents overwrites and path traversal through device IDs.
        const QString folder = QDir(task.outputDirectory).filePath(out.taskId);
        if (!QDir().mkpath(folder)) throw std::runtime_error("Cannot create evidence directory");
        auto saveImage = [&](const cv::Mat& mat, const QString& name, QString& path) {
            if (mat.empty()) return;
            std::vector<uchar> bytes;
            const auto extension = QFileInfo(name).suffix();
            const std::vector<int> options = extension=="jpg" ? std::vector<int>{cv::IMWRITE_JPEG_QUALITY,90} : std::vector<int>{cv::IMWRITE_PNG_COMPRESSION,3};
            if (!cv::imencode(("."+extension).toStdString(), mat, bytes, options)) throw std::runtime_error("Image encoding failed");
            const QString target = QDir(folder).filePath(name);
            QSaveFile file(target);
            if (!file.open(QIODevice::WriteOnly) ||
                file.write(reinterpret_cast<const char*>(bytes.data()), qint64(bytes.size())) != qint64(bytes.size()) ||
                !file.commit()) throw std::runtime_error("Evidence write failed");
            path = target;
            imageManifest.append(QJsonObject{{"file",name}, {"bytes",static_cast<double>(bytes.size())},
                {"sha256",QString::fromLatin1(QCryptographicHash::hash(QByteArray(reinterpret_cast<const char*>(bytes.data()), int(bytes.size())),QCryptographicHash::Sha256).toHex())}});
        };
        // Acquisition evidence is independent of diagnostic rendering. Keep it even
        // when recognition/configuration fails, and never replace it with another frame.
        const auto suffix=EvidenceArchive::imageSuffix(out.encodedOriginal);
        if (!out.capturedFrame.empty() && !suffix.isEmpty()) {
            const auto bytes=QByteArray(reinterpret_cast<const char*>(out.encodedOriginal.data()),int(out.encodedOriginal.size()));
            out.originalPath=QDir(folder).filePath("original"+suffix);
            QSaveFile file(out.originalPath);
            if (!file.open(QIODevice::WriteOnly) || file.write(bytes)!=bytes.size() || !file.commit()) throw std::runtime_error("Original evidence write failed");
            imageManifest.append(QJsonObject{{"file","original"+suffix},{"bytes",bytes.size()},
                {"sha256",QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex())}});
            out.archiveMetadata["original_encoding"]="source_bytes";
        } else {
            saveImage(out.capturedFrame, "original.png", out.originalPath);
            out.archiveMetadata["original_encoding"]="lossless_png";
        }
        out.archiveMetadata["original_width"]=out.capturedFrame.cols;
        out.archiveMetadata["original_height"]=out.capturedFrame.rows;
        saveImage(out.result.step2_rectified, "rectified.jpg", out.rectifiedPath);
        saveImage(out.result.step4_recognition, "recognition.jpg", out.recognitionPath);
        if (task.runMode == GaugeRunMode::Debug) {
            QString grayPath;
            saveImage(out.result.step3_gray, "gray.png", grayPath);
            QString hudPath;
            saveImage(out.result.diagnosticHudImage, "diagnostic_hud.png", hudPath);
        }
        out.archiveMetadata["images"]=imageManifest;
        out.timings["image_save_ms"] = elapsedMs(imageStarted);
        QJsonObject record;
        record["archive"] = out.archiveMetadata;
        record["safety_assessment"] = out.safetyAssessment;
        record["pointer_angle_deg"] = out.result.pointer_angle_deg;
        record["rectified_path"] = out.rectifiedPath;
        record["capture_method"] = out.captureMethod;
        record["timings_path"] = QDir(folder).filePath("timings.json");
        record["mode"] = gaugeRunModeName(task.runMode);
        record["input_kind"] = inspectionInputName(task.inputKind);
        record["parent_task_id"] = task.parentTaskId;
        record["processed_at"] = processedAt.toString(Qt::ISODateWithMs);
        record["recognition_performed"] = out.recognitionPerformed;
        record["device"] = out.deviceSnapshot;
        record["profile_snapshot"] = out.profileSnapshot;
        record["original_path"] = out.originalPath;
        record["review_image_available"] = !out.originalPath.isEmpty();
        record["recognition_path"] = out.recognitionPath;
        record["center_inconsistent"] = out.result.centerInconsistent;
        record["center_offset_px"] = out.result.centerOffsetDist;
        record["warning"] = QString::fromStdString(out.result.warningMessage);
        record["diagnostics"] = QString::fromStdString(out.result.detailedDebugInfo);
        record["task_id"] = out.taskId;
        record["camera_id"] = out.cameraId;
        record["captured_at"] = out.capturedAt.toString(Qt::ISODateWithMs);
        record["success"] = out.errorCode.isEmpty() && out.result.success;
        record["value"] = out.result.success ? QJsonValue(out.result.value) : QJsonValue(QJsonValue::Null);
        record["unit"] = QString::fromStdString(out.result.unit);
        record["confidence"] = out.result.confidence;
        record["recognition_timings_ms"] = out.result.stageTimings;
        record["recognition_quality"] = out.result.recognitionQuality;
        record["error_code"] = out.errorCode;
        record["error_message"] = out.errorMessage.isEmpty() ? QString::fromStdString(out.result.errorMessage) : out.errorMessage;
        const auto resultSaveStarted = TimingClock::now();
        const QByteArray json = QJsonDocument(record).toJson();
        QSaveFile file(QDir(folder).filePath("result.json"));
        if (!file.open(QIODevice::WriteOnly) || file.write(json) != json.size() || !file.commit())
            throw std::runtime_error("Result write failed");
        out.timings["result_save_ms"] = elapsedMs(resultSaveStarted);
    } catch (const std::exception& e) {
        out.errorCode = "STORAGE_FAILED";
        out.errorMessage = QString::fromUtf8(e.what());
    }
    if (!out.timings.contains("image_save_ms")) out.timings["image_save_ms"] = elapsedMs(imageStarted);
    const auto databaseStarted = TimingClock::now();
    const bool databaseAttempted = DatabaseSettings::fromEnvironment().enabled && out.errorCode != "STORAGE_FAILED";
    if (databaseAttempted) {
        QString pendingPath;
        try {
            const auto envelope = MySqlRepository::inspectionEnvelope(task, out);
            pendingPath = MySqlRepository::queueInspection(envelope, task.outputDirectory);
            out.databasePending = true;
            MySqlRepository().saveInspection(envelope);
            out.databaseSaved = true;
            out.databasePending = false;
            if (!QFile::remove(pendingPath)) out.databaseError = QStringLiteral(u"入库成功，待同步文件清理失败；重试不会重复插入");
        } catch (const std::exception& e) {
            out.databaseError = QString::fromUtf8(e.what());
        }
    }
    out.timings["database_write_ms"] = databaseAttempted ? QJsonValue(elapsedMs(databaseStarted)) : QJsonValue(QJsonValue::Null);
    out.timings["total_ms"] = elapsedMs(task.queuedAt);
    // Separate final metrics are written after the durable result and database attempt.
    // This keeps failed database writes measurable without rewriting evidence.
    if (out.errorCode != "STORAGE_FAILED") {
        QSaveFile timingFile(QDir(task.outputDirectory).filePath(out.taskId + "/timings.json"));
        const auto bytes = QJsonDocument(QJsonObject{{"task_id", out.taskId}, {"camera_id", out.cameraId},
                                                   {"timings_ms", out.timings}}).toJson();
        if (!timingFile.open(QIODevice::WriteOnly) || timingFile.write(bytes) != bytes.size() || !timingFile.commit()) {
            out.timings["metrics_save_failed"] = true;
        }
    }
    out.encodedOriginal.clear();
    out.encodedOriginal.shrink_to_fit();
    // Evidence is on disk; retain scalar results rather than full per-device pipelines.
    if (task.returnImages) return out; // A single GUI task may display its outputs; patrol remains compact.
    GaugeResult compact;
    compact.runMode = task.runMode;
    compact.success = out.result.success;
    compact.value = out.result.value;
    compact.unit = out.result.unit;
    compact.pointer_angle_deg = out.result.pointer_angle_deg;
    compact.confidence = out.result.confidence;
    compact.stageTimings = out.result.stageTimings;
    compact.recognitionQuality = out.result.recognitionQuality;
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
    : QObject(parent), m_factory(factory ? factory : SourceFactory(createSource)), m_isolateCapture(!factory),
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
    m_retryIndices.clear();
    m_restartIndices.clear();
    m_taskCancels.clear();
    const auto queuedAt = TimingClock::now();
    for (auto& task : m_tasks) {
        task.taskId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        task.queuedAt = queuedAt;
    }
    m_next = m_active = m_completed = 0;
    m_cancel = std::make_shared<std::atomic<bool>>(false);
    m_running = true;
    dispatch();
    return true;
}

void InspectionService::stop() {
    if (m_cancel) m_cancel->store(true);
    for (const auto& cancel : m_taskCancels) cancel->store(true);
}

void InspectionService::updateDeviceConnection(const DiscoveredCameraItem& device) {
    // Called only on the scheduler/GUI thread. Workers keep immutable task copies.
    if (!m_running || m_cancel->load()) return;
    for (int index=0;index<m_tasks.size();++index) {
        auto& task=m_tasks[index];
        if (task.device.cameraId!=device.cameraId || task.inputKind!=InspectionInputKind::CameraSnapshot ||
            sameDeviceConnection(task.device,device)) continue;
        if (index<m_next && !m_taskCancels.contains(index) && !m_retryIndices.contains(index)) continue;
        ::updateDeviceConnection(task.device,device);
        task.previewSnapshot.release(); task.previewCapturedAt=QDateTime();
        if (m_taskCancels.contains(index)) {
            m_restartIndices.insert(index);
            m_taskCancels[index]->store(true);
        }
    }
}

void InspectionService::retryDatabaseWrites(const QString& directory) {
    if (m_databaseRetryRunning || !DatabaseSettings::fromEnvironment().enabled) return;
    m_databaseRetryRunning = true;
    const auto now=QDateTime::currentDateTimeUtc();
    const bool maintain=!m_lastEvidenceMaintenance.isValid() || m_lastEvidenceMaintenance.secsTo(now)>=300;
    using SyncResult=QPair<QString,QString>;
    auto* watcher = new QFutureWatcher<SyncResult>(this);
    connect(watcher, &QFutureWatcher<SyncResult>::finished, this, [this, watcher, maintain, now]() {
        m_databaseRetryRunning = false;
        const auto result=watcher->result();
        emit databaseSyncFinished(result.first);
        const auto maintenance = QJsonDocument::fromJson(result.second.toUtf8()).object();
        const bool cleaned = maintenance["expired_database_tasks"].toInt() > 0 ||
            maintenance["expired_local_tasks"].toInt() > 0 ||
            maintenance["removed_unreferenced_blobs"].toInt() > 0 ||
            maintenance["local_removed_images"].toDouble() > 0;
        if (cleaned) {
            const auto localCount = qint64(maintenance["local_removed_images"].toDouble());
            const auto databaseCount = maintenance["removed_unreferenced_blobs"].toInt();
            const auto formatBytes = [](double bytes) {
                if (bytes >= 1024.0*1024*1024) return QString::number(bytes/(1024.0*1024*1024), 'f', 2) + " GB";
                if (bytes >= 1024.0*1024) return QString::number(bytes/(1024.0*1024), 'f', 2) + " MB";
                if (bytes >= 1024.0) return QString::number(bytes/1024.0, 'f', 2) + " KB";
                return QString::number(bytes, 'f', 0) + " B";
            };
            const auto localBytes = maintenance["local_reclaimed_bytes"].toDouble();
            const auto databaseBytes = maintenance["database_reclaimed_bytes"].toDouble();
            auto summary = tr("图片清理完成：清理图片共 %1 份（本地 %2 张，数据库 %3 份），回收存储空间 %4（本地 %5，数据库 %6）。过期记录：数据库 %7 个任务，本地 %8 个任务。")
                .arg(localCount + databaseCount).arg(localCount).arg(databaseCount)
                .arg(formatBytes(localBytes + databaseBytes), formatBytes(localBytes), formatBytes(databaseBytes))
                .arg(maintenance["expired_database_tasks"].toInt()).arg(maintenance["expired_local_tasks"].toInt());
            if (maintenance["local_cleanup_failures"].toInt() > 0)
                summary += tr("本地清理失败 %1 个任务，将后续重试。").arg(maintenance["local_cleanup_failures"].toInt());
            emit evidenceMaintenanceFinished(summary);
        } else if (result.second.startsWith("RETENTION_FAILED")) {
            emit evidenceMaintenanceFinished(tr("图片清理失败：%1").arg(result.second.mid(QStringLiteral("RETENTION_FAILED: ").size())));
        }
        if(maintain && !result.second.isEmpty() && !result.second.startsWith("RETENTION_FAILED")) m_lastEvidenceMaintenance=now;
        watcher->deleteLater();
    });
    watcher->setFuture(QtConcurrent::run(&m_storagePool, [directory,maintain]() {
        SyncResult result;
        result.first=MySqlRepository::retryPending(directory);
        if(maintain && result.first.isEmpty()) {
            try { result.second=MySqlRepository().maintainEvidence(directory); }
            catch(const std::exception& e) { result.second="RETENTION_FAILED: "+QString::fromUtf8(e.what()); }
        }
        return result;
    }));
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
                return QStringLiteral(u"图像编码失败");
            QSaveFile file(path);
            if (!file.open(QIODevice::WriteOnly) ||
                file.write(reinterpret_cast<const char*>(bytes.data()), qint64(bytes.size())) != qint64(bytes.size()) || !file.commit())
                return QStringLiteral(u"图片写入失败");
            return QString();
        } catch (const std::exception& e) { return QString::fromUtf8(e.what()); }
    }));
}

void InspectionService::dispatch() {
    while (!m_cancel->load() && m_active < m_limit && (!m_retryIndices.isEmpty() || m_next < m_tasks.size())) {
        const int index = m_retryIndices.isEmpty() ? m_next++ : m_retryIndices.takeFirst();
        const auto task = m_tasks[index];
        const auto cancel = std::make_shared<std::atomic<bool>>(false);
        m_taskCancels.insert(index,cancel);
        ++m_active;
        emit taskStarted(task.taskId, task.device.cameraId);
        auto* captureWatcher = new QFutureWatcher<CapturedTask>(this);
        connect(captureWatcher, &QFutureWatcher<CapturedTask>::finished, this, [this, captureWatcher, task, cancel, index]() {
            auto captured = captureWatcher->result();
            captureWatcher->deleteLater();
            if (cancel->load()) { complete(captured.outcome, index); return; }
            auto* recognitionWatcher = new QFutureWatcher<InspectionOutcome>(this);
            connect(recognitionWatcher, &QFutureWatcher<InspectionOutcome>::finished, this, [this, recognitionWatcher, task, cancel, index]() {
                auto outcome = recognitionWatcher->result();
                recognitionWatcher->deleteLater();
                if (cancel->load()) { complete(outcome, index); return; }
                auto* storageWatcher = new QFutureWatcher<InspectionOutcome>(this);
                connect(storageWatcher, &QFutureWatcher<InspectionOutcome>::finished, this, [this, storageWatcher, index]() {
                    auto outcome = storageWatcher->result();
                    storageWatcher->deleteLater();
                    complete(std::move(outcome), index);
                });
                const auto storageQueued = TimingClock::now();
                storageWatcher->setFuture(QtConcurrent::run(&m_storagePool, [outcome, task, cancel, storageQueued]() mutable {
                    outcome.timings["storage_queue_ms"] = elapsedMs(storageQueued);
                    return persist(outcome, task, cancel);
                }));
            });
            const auto recognitionQueued = TimingClock::now();
            recognitionWatcher->setFuture(QtConcurrent::run(&m_recognitionPool, [captured, cancel, recognitionQueued]() {
                const auto started = TimingClock::now();
                auto outcome = recognize(captured, cancel);
                outcome.timings["recognition_queue_ms"] = std::chrono::duration<double, std::milli>(started - recognitionQueued).count();
                outcome.timings["recognition_ms"] = elapsedMs(started);
                return outcome;
            }));
        });
        const auto factory = m_factory;
        captureWatcher->setFuture(QtConcurrent::run(&m_capturePool, [task, factory, cancel, isolate = m_isolateCapture]() {
            const auto started = TimingClock::now();
            auto captured = capture(task, factory, cancel, isolate);
            captured.outcome.timings["queue_ms"] = std::chrono::duration<double, std::milli>(started - task.queuedAt).count();
            captured.outcome.timings["capture_ms"] = elapsedMs(started);
            return captured;
        }));
    }
    if (m_active == 0 && (m_cancel->load() || (m_next == m_tasks.size() && m_retryIndices.isEmpty()))) {
        const bool cancelled = m_cancel->load();
        m_running = false;
        m_tasks.clear();
        emit finished(cancelled);
    }
}

void InspectionService::complete(InspectionOutcome outcome, int index) {
    --m_active;
    m_taskCancels.remove(index);
    if (m_restartIndices.remove(index) && !m_cancel->load()) {
        // A new attempt gets a fresh evidence ID; any already committed history remains immutable.
        m_tasks[index].taskId=QUuid::createUuid().toString(QUuid::WithoutBraces);
        m_tasks[index].queuedAt=TimingClock::now();
        m_retryIndices.append(index);
        dispatch();
        return;
    }
    if (!m_cancel->load()) {
        ++m_completed;
        emit taskFinished(outcome);
        emit progressChanged(m_completed, m_tasks.size());
    }
    dispatch();
}
