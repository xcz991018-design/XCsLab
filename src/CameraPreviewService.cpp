#include "CameraPreviewService.hpp"
#include <QtConcurrent/QtConcurrent>

CameraPreviewService::CameraPreviewService(QObject* parent, InspectionService::SourceFactory factory)
    : QObject(parent), m_factory(factory ? std::move(factory) : InspectionService::SourceFactory(InspectionService::createCameraSource)) {
    m_pool.setMaxThreadCount(1);
    connect(&m_watcher, &QFutureWatcher<FrameCaptureResult>::finished, this, [this]() {
        m_busy = false;
        if (!m_stopped && m_pendingGeneration == m_generation)
            emit frameReady(m_pendingCamera, m_watcher.result());
        else if (!m_stopped) requestFrame();
    });
}

CameraPreviewService::~CameraPreviewService() {
    stop();
    m_pool.waitForDone();
}

void CameraPreviewService::select(const InspectionTask& task) {
    m_requested = task;
    ++m_generation;
    if (task.device.cameraId.isEmpty()) {
        QtConcurrent::run(&m_pool, [this]() { if (m_source) m_source->close(); m_source.reset(); });
        return;
    }
    requestFrame();
}

void CameraPreviewService::requestFrame() {
    if (m_stopped || m_busy || m_requested.device.cameraId.isEmpty()) return;
    m_busy = true;
    const auto task = m_requested;
    const auto generation = m_generation;
    m_pendingCamera = task.device.cameraId;
    m_pendingGeneration = generation;
    m_watcher.setFuture(QtConcurrent::run(&m_pool, [this, task, generation]() {
        FrameCaptureResult result;
        try {
            if (m_workerGeneration != generation) {
                if (m_source) m_source->close();
                m_source = m_factory(task);
                m_workerGeneration = generation;
                if (!m_source) { result.failure = CaptureFailure::UnsupportedProtocol; return result; }
                if (!m_source->open()) { result.failure = CaptureFailure::OpenFailed; return result; }
            }
            if (!m_source) { result.failure = CaptureFailure::UnsupportedProtocol; return result; }
            auto acquired = m_source->readFrame();
            if (!acquired.ok()) {
                acquired.frame.release();
                if (acquired.failure == CaptureFailure::None) acquired.failure = CaptureFailure::NoFrame;
            }
            return acquired;
        } catch (const std::exception& e) {
            result.failure = CaptureFailure::ReadFailed;
            result.message = e.what();
            return result;
        }
    }));
}

void CameraPreviewService::stop() {
    if (m_stopped) return;
    m_stopped = true;
    ++m_generation;
    // Runs after the pending read, so source shutdown never races acquisition.
    QtConcurrent::run(&m_pool, [this]() { if (m_source) m_source->close(); m_source.reset(); });
}
