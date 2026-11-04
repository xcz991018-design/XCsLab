#pragma once
#include "GaugeProfile.hpp"
#include <algorithm>
#include <cmath>

// Device annotations stay in raw calibration pixels. Detector lengths and
// persisted hub exclusion radii retain their legacy 800-pixel reference units.
namespace RectifiedResolution {
inline double scale(const GaugeProfile& p) { return double(p.rectified_size) / 800.0; }
inline void apply(GaugeProfile& p, cv::Size source) {
    const double marginRatio = double(p.rectified_margin) / std::max(1, p.rectified_size);
    p.rectified_size = std::max({800, source.width, source.height});
    p.rectified_margin = int(std::lround(p.rectified_size * marginRatio));
}
}
