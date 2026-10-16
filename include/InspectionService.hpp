#pragma once

#include "AutoCameraDiscoveryService.hpp"
#include "GaugeReaderEngine.hpp"
#include <QObject>
#include <QThreadPool>
#include <QDateTime>
#include <atomic>
#include <functional>
#include <QJsonObject>
#include <QSet>
#include <chrono>

enum class InspectionInputKind { CameraSnapshot, LocalImage, HistoricalImage };
inline const char* inspectionInputName(InspectionInputKind kind) {
    switch (kind) {
    case InspectionInputKind::CameraSnapshot: return "live_snapshot";
    case InspectionInputKind::LocalImage: return "local_image";
    case InspectionInputKind::HistoricalImage: return "historical_image";
    }
    return "invalid";
}
struct InspectionTask {
    QString taskId;
    std::chrono::steady_clock::time_point queuedAt;
    DiscoveredCameraItem device;
    QString localImagePath;
    QString outputDirectory;
    GaugeRunMode runMode{GaugeRunMode::Production};
    InspectionInputKind inputKind{InspectionInputKind::CameraSnapshot};
    QString parentTaskId;
    QDateTime originalCapturedAt;
    bool captureOnly{false};
    bool requireOnvifSnapshot{false}; // HTTP snapshot only; never reuse preview or fall back to RTSP.
    bool preferOnvifSnapshot{false}; // Batch inspection prefers HTTP; preview stays RTSP.
    // Owned copy of the live preview frame selected by the snapshot button.
    cv::Mat previewSnapshot;
    QDateTime previewCapturedAt;
    int captureTimeoutMs{8000}; // Whole network acquisition, including process startup/close.
    bool returnImages{false};
    QString databaseOriginalTaskId;
    QJsonObject historicalProfile;
    QJsonObject historicalCalibration; // Explicit per-analysis geometry; never saved to camera archives.
    bool useHistoricalConfiguration{true};
};

struct InspectionOutcome {
    std::vector<uchar> encodedOriginal;
    QJsonObject archiveMetadata;
    QJsonObject safetyAssessment; // Frozen task thresholds and unrounded reading evaluation.
    QString captureMethod;
    QJsonObject timings; // Milliseconds, measured using a monotonic clock.
    QString taskId;
    QString cameraId;
    QDateTime capturedAt;
    GaugeResult result;
    QString errorCode;
    QString errorMessage;
    QString originalPath;
    QString rectifiedPath;
    QString recognitionPath;
    // Transient, read-only acquisition buffer. Released before taskFinished is emitted.
    cv::Mat capturedFrame;
    InspectionInputKind inputKind{InspectionInputKind::CameraSnapshot};
    QString parentTaskId;
    bool recognitionPerformed{false};
    QJsonObject profileSnapshot;
    QJsonObject deviceSnapshot;
    bool databaseSaved{false};
    bool databasePending{false};
    QString databaseError;
};
Q_DECLARE_METATYPE(InspectionOutcome)

// Owner-thread scheduler; workers never access GUI objects or mutable device state.
// At most maxConcurrent tasks occupy the capture/recognition/storage pipeline.
class InspectionService : public QObject {
    Q_OBJECT
public:
    using SourceFactory = std::function<CameraSourcePtr(const InspectionTask&)>;
    explicit InspectionService(QObject* parent = nullptr, int maxConcurrent = 4,
                               SourceFactory factory = SourceFactory());
    ~InspectionService() override;
    bool start(const QList<InspectionTask>& tasks);
    void stop();
    void exportImage(const cv::Mat& frame, const QString& path);
    bool isRunning() const { return m_running; }
    static CameraSourcePtr createCameraSource(const InspectionTask& task);
    void retryDatabaseWrites(const QString& directory);
signals:
    void taskStarted(const QString& taskId, const QString& cameraId);
    void taskFinished(const InspectionOutcome& outcome);
    void progressChanged(int completed, int total);
    void finished(bool cancelled);
    void imageExported(const QString& path, const QString& error);
    void databaseSyncFinished(const QString& error);
    void evidenceMaintenanceFinished(const QString& report);
private:
    void dispatch();
    void complete(InspectionOutcome outcome);
    QThreadPool m_capturePool;
    QThreadPool m_recognitionPool;
    QThreadPool m_storagePool;
    SourceFactory m_factory;
    bool m_isolateCapture{false};
    QList<InspectionTask> m_tasks;
    std::shared_ptr<std::atomic<bool>> m_cancel;
    int m_limit;
    int m_next{0};
    int m_active{0};
    int m_completed{0};
    bool m_running{false};
    bool m_databaseRetryRunning{false};
    QDateTime m_lastEvidenceMaintenance;
};
