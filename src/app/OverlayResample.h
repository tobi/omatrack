// Resampling of an in-memory explicit-time series (plugin channels) onto a
// lap's 50 Hz grid. Header-only so the unit test and OverlayManager share it.
#pragma once

#include "TelemetryStore.h"  // LapEntry

#include <cmath>
#include <limits>
#include <memory>
#include <vector>

namespace omatrack {

/// Linear interpolation of an explicit-time series onto the lap grid. A grid
/// instant before the first or after the last sample stays NaN; a gap wider
/// than `maxGapNs` between neighbours is not bridged.
inline std::shared_ptr<std::vector<double>> resampleSeriesOntoLap(
    const std::vector<qint64>& times, const std::vector<double>& values,
    const LapEntry& lap, const std::vector<double>& gridTime,
    qint64 clipStartNs, qint64 clipEndNs, qint64 maxGapNs) {
    auto out = std::make_shared<std::vector<double>>(
        gridTime.size(), std::numeric_limits<double>::quiet_NaN());
    if (times.size() < 1 || times.size() != values.size()) return out;
    const qint64 lapStartNs = qint64(std::llround(lap.startTime * 1e9));
    size_t cursor = 0;
    for (size_t index = 0; index < gridTime.size(); ++index) {
        const qint64 hostNs =
            lapStartNs + qint64(std::llround(gridTime[index] * 1e9));
        if (clipEndNs > clipStartNs &&
            (hostNs < clipStartNs || hostNs >= clipEndNs))
            continue;
        while (cursor + 1 < times.size() && times[cursor + 1] <= hostNs)
            ++cursor;
        if (hostNs < times.front() || hostNs > times.back()) continue;
        if (times[cursor] == hostNs || cursor + 1 >= times.size()) {
            (*out)[index] = values[cursor];
            continue;
        }
        const qint64 span = times[cursor + 1] - times[cursor];
        if (span <= 0 || (maxGapNs > 0 && span > maxGapNs)) continue;
        const double a = values[cursor];
        const double b = values[cursor + 1];
        if (!std::isfinite(a) || !std::isfinite(b)) continue;
        const double f = double(hostNs - times[cursor]) / double(span);
        (*out)[index] = a + (b - a) * f;
    }
    return out;
}

}  // namespace omatrack
