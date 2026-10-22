#pragma once
#include <QJsonObject>
#include <QString>
#include <cmath>
#include <limits>

struct SafeWorkingRange {
    bool enabled{true};
    double normalMin{0}, normalMax{0};
    double lower() const { return normalMin; }
    double upper() const { return normalMax; }
    bool valid() const {
        return std::isfinite(normalMin) && std::isfinite(normalMax) &&
            normalMin <= normalMax;
    }
    QJsonObject toJson() const {
        return {{"defaults_version",2},{"enabled",enabled},{"normal_min",normalMin},{"normal_max",normalMax}};
    }
    static SafeWorkingRange fromJson(const QJsonObject& object) {
        SafeWorkingRange r;
        // Upgrade the former opt-in default once; later explicit opt-outs remain respected.
        r.enabled = object["defaults_version"].toInt() < 2 ? true : object["enabled"].toBool(true);
        const double missing = r.enabled ? std::numeric_limits<double>::quiet_NaN() : 0.0;
        r.normalMin = object["normal_min"].toDouble(missing);
        r.normalMax = object["normal_max"].toDouble(missing);
        return r;
    }
    QJsonObject assess(double value, bool recognized) const {
        QString state = "disabled";
        if (enabled) {
            if (!valid()) state = "invalid_configuration";
            else if (!recognized || !std::isfinite(value)) state = "not_evaluated";
            else {
                // Allow only one representable step for arithmetic at inclusive decimal boundaries.
                const double min = std::nextafter(lower(), -std::numeric_limits<double>::infinity());
                const double max = std::nextafter(upper(), std::numeric_limits<double>::infinity());
                state = value < min || value > max ? "out_of_range" : "normal";
            }
        }
        auto result = toJson();
        result["status"] = state;
        if (valid()) { result["safe_min"] = lower(); result["safe_max"] = upper(); }
        return result;
    }
};
