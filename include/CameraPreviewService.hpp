#pragma once
#include "InspectionService.hpp"
#include <QFutureWatcher>

Q_DECLARE_METATYPE(FrameCaptureResult)

// A serialized backend owns stream lifecycle; UI receives only frames or status.
class CameraPreviewService : public QObject {
    Q_OBJECT
public:
    explicit CameraPreviewService(QObject* parent = nullptr,
                                  InspectionService::SourceFactory factory = InspectionService::SourceFactory());
    ~CameraPreviewService() override;
    void select(const InspectionTask& task);
    void requestFrame();
    void stop();
signals:
    void frameReady(const QString& cameraId, const FrameCaptureResult& result);
private:
    QThreadPool m_pool;
    QFutureWatcher<FrameCaptureResult> m_watcher;
    InspectionTask m_requested;
    CameraSourcePtr m_source; // Accessed only by the serialized worker.
    quint64 m_generation{0};
    quint64 m_workerGeneration{0};
    quint64 m_pendingGeneration{0};
    QString m_pendingCamera;
    bool m_stopped{false};
    bool m_busy{false};
    InspectionService::SourceFactory m_factory;
};
