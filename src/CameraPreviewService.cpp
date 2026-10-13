#include "CameraPreviewService.hpp"
#include "IsolatedCapture.hpp"
#include "SnapshotFrameReader.hpp"
#include <QtConcurrent/QtConcurrent>

CameraPreviewService::CameraPreviewService(QObject* parent, InspectionService::SourceFactory factory)
    : QObject(parent), m_factory(factory ? factory : InspectionService::SourceFactory(InspectionService::createCameraSource)), m_isolateCapture(!factory) {
    m_pool.setMaxThreadCount(1);
    connect(&m_watcher, &QFutureWatcher<FrameCaptureResult>::finished, this, [this]() {
        m_busy = false;
        if (!m_stopped && m_pendingGeneration == m_generation) {
            m_latestPreview = m_watcher.result();
            m_latestPreview.frame = m_latestPreview.frame.clone();
            emit frameReady(m_pendingCamera, m_latestPreview);
        }
        else if (!m_stopped) requestFrame();
    });
}

CameraPreviewService::~CameraPreviewService() {
    stop();
    m_pool.waitForDone();
}

void CameraPreviewService::select(const InspectionTask& task) {
    m_latestPreview = FrameCaptureResult();
    m_captureCancel->store(true);
    m_captureCancel = std::make_shared<std::atomic<bool>>(false);
    m_requested = task;
    ++m_generation;
    if (task.device.cameraId.isEmpty()) {
        QtConcurrent::run(&m_pool, [this]() { if (m_source) m_source->close(); m_source.reset(); m_isolatedPreview.reset(); });
        return;
    }
    requestFrame();
}

FrameCaptureResult CameraPreviewService::snapshot(const QString& cameraId) const {
    FrameCaptureResult result;
    if (m_stopped || cameraId.isEmpty() || cameraId != m_requested.device.cameraId || !m_latestPreview.ok() ||
        isUniformSnapshotFrame(m_latestPreview.frame)) {
        result.failure = CaptureFailure::NoFrame;
        return result;
    }
    const auto age = std::chrono::system_clock::now() - m_latestPreview.capturedAt;
    if (age < std::chrono::system_clock::duration::zero() || age >= std::chrono::seconds(2)) {
        result.failure = CaptureFailure::StaleFrame;
        return result;
    }
    result = m_latestPreview;
    result.frame = result.frame.clone();
    return result;
}

void CameraPreviewService::requestFrame() {
    if (m_stopped || m_busy || m_requested.device.cameraId.isEmpty()) return;
    m_busy = true;
    const auto task = m_requested;
    const auto generation = m_generation;
    const auto cancel = m_captureCancel;
    m_pendingCamera = task.device.cameraId;
    m_pendingGeneration = generation;
    m_watcher.setFuture(QtConcurrent::run(&m_pool, [this, task, generation, cancel]() {
        FrameCaptureResult result;
        try {
            if (m_isolateCapture && task.device.protocolType != CameraProtocolType::LOCAL_FILE) {
                if (m_workerGeneration != generation || !m_isolatedPreview) {
                    m_isolatedPreview = std::make_shared<IsolatedPreview>();
                    m_workerGeneration = generation;
                }
                auto acquired = m_isolatedPreview->read(task, cancel);
                if (!acquired.ok()) m_isolatedPreview.reset();
                return acquired;
            }
            if (m_workerGeneration != generation) {
                m_isolatedPreview.reset();
                if (m_source) m_source->close();
                m_source = m_factory(task);
                m_workerGeneration = generation;
                if (!m_source) { result.failure = CaptureFailure::UnsupportedProtocol; return result; }
                if (!m_source->open()) {
                    result.failure = CaptureFailure::OpenFailed;
                    if (auto* onvif = dynamic_cast<ONVIFDeviceManager*>(m_source.get())) result.message = onvif->lastError();
                    return result;
                }
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
    m_captureCancel->store(true);
    ++m_generation;
    // Runs after the pending read, so source shutdown never races acquisition.
    QtConcurrent::run(&m_pool, [this]() { if (m_source) m_source->close(); m_source.reset(); m_isolatedPreview.reset(); });
}
