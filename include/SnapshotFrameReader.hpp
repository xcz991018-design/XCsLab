#pragma once
#include "ICameraSource.hpp"
#include <algorithm>
#include <functional>
#include <thread>

struct SnapshotDecodePolicy {
    std::chrono::milliseconds timeout{4000};
    std::chrono::milliseconds warmup{300};
    int minimumValidFrames{3};
};

// A decoder may report a successful read while its reference frames are still
// missing. Neutral, almost uniform images contain no usable dial information.
inline bool isUniformSnapshotFrame(const cv::Mat& frame) {
    if (frame.empty()) return true;
    if (frame.depth() != CV_8U || (frame.channels() != 1 && frame.channels() != 3 && frame.channels() != 4)) return false;
    cv::Scalar mean, deviation;
    cv::meanStdDev(frame, mean, deviation);
    const int channels = frame.channels() == 4 ? 3 : frame.channels();
    double minimumMean = mean[0], maximumMean = mean[0];
    for (int i = 0; i < channels; ++i) {
        if (deviation[i] > 2.0) return false;
        minimumMean = std::min(minimumMean, mean[i]);
        maximumMean = std::max(maximumMean, mean[i]);
    }
    return maximumMean - minimumMean <= 3.0;
}

// The owning capture worker supplies the overall deadline and cancellation.
// This loop additionally bounds decoder warm-up without returning a partial
// startup frame or retaining a frame from a previous failed read.
inline FrameCaptureResult readDecodedSnapshot(const std::function<bool(cv::Mat&)>& read,
                                             const SnapshotDecodePolicy& policy = SnapshotDecodePolicy(),
                                             const std::function<int()>& frameType = {}) {
    FrameCaptureResult result;
    const auto started = std::chrono::steady_clock::now();
    int validFrames = 0;
    bool sawUniformFrame = false;
    bool keyFrameReady = !frameType;
    while (std::chrono::steady_clock::now() - started < policy.timeout) {
        cv::Mat frame;
        if (!read(frame) || frame.empty()) {
            validFrames = 0;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        if (isUniformSnapshotFrame(frame)) {
            sawUniformFrame = true;
            validFrames = 0;
        } else {
            if (!keyFrameReady) {
                const int type = frameType();
                // FFmpeg reports ASCII I/P/B. Unknown/unsupported metadata falls
                // back to the bounded warm-up and image-content checks.
                keyFrameReady = type == 'I' || type == '?' || type <= 0;
                if (!keyFrameReady) {
                    validFrames = 0;
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
            }
            ++validFrames;
            const auto elapsed = std::chrono::steady_clock::now() - started;
            if (elapsed < policy.timeout && elapsed >= policy.warmup &&
                validFrames >= policy.minimumValidFrames) {
                result.frame = frame.clone();
                result.capturedAt = std::chrono::system_clock::now();
                return result;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    result.failure = CaptureFailure::NoFrame;
    result.message = sawUniformFrame
        ? "Camera stream returned uniform gray/blank frames; no usable image after decoder warm-up"
        : "Camera stream did not provide a usable image before decoder warm-up deadline";
    return result;
}
