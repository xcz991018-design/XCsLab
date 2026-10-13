#pragma once
#include "InspectionService.hpp"
FrameCaptureResult isolatedCapture(const InspectionTask& task,
    const std::shared_ptr<std::atomic<bool>>& cancel);
// Returns -1 for a normal application launch.
int runCaptureWorker(int argc, char** argv);
class IsolatedPreview {
public:
    IsolatedPreview();
    ~IsolatedPreview();
    FrameCaptureResult read(const InspectionTask& task, const std::shared_ptr<std::atomic<bool>>& cancel);
private:
    struct State;
    std::unique_ptr<State> m_state;
};
