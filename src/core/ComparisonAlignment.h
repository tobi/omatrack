// Pure primary/reference lap alignment for traces, delta, and synchronized
// video. Qt-free: uses std::vector and std::string so the CLI and unit tests
// can run it without a QGuiApplication. The app layer wraps this in a thin
// QVector/QString adapter (src/app/ComparisonAlignment.h).

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace omatrack {
struct UnifiedLap;
}

enum class ComparisonAlignmentStrategy {
    GpsContinuous,
    PreCornerGps,
    PreCornerDampers,
    ManualDampers,
    LapPercentage,
};

namespace omatrack::alignment {

struct Options {
    ComparisonAlignmentStrategy strategy =
        ComparisonAlignmentStrategy::GpsContinuous;
    // Primary-lap sample fractions at the starts of configured corners.
    std::vector<double> cornerStarts;
};

// One pass of comparison alignment for a primary and a reference (compare) lap.
struct Result {
    // Compare-lap time (seconds) for every primary sample, in primary order.
    std::vector<double> time;
    // Compare-lap fraction (0-1) for every primary sample, in primary order.
    std::vector<double> fraction;
    // Stable user-facing alignment basis (empty when alignment is impossible).
    std::string basis;
    // Number of accepted GPS anchors (zero for non-GPS strategies).
    int gpsAnchors = 0;
    // Non-empty when monotonicity validation rejected the time axis.
    std::string rejectionReason;
};

// Pure: no globals, no I/O, no Qt event loop. Returns an empty result when
// either lap is too short. An unavailable requested strategy degrades to lap
// percentage rather than manufacturing an alignment.
Result compute(const omatrack::UnifiedLap& primary,
               const omatrack::UnifiedLap& compare,
               const Options& options = {});

// Cheap capability checks for presenting only strategies that both selected
// laps can actually support.
bool gpsAvailable(const omatrack::UnifiedLap& primary,
                  const omatrack::UnifiedLap& compare);
bool damperAvailable(const omatrack::UnifiedLap& primary,
                     const omatrack::UnifiedLap& compare);

// Signed along-track distance in metres from the primary car to the
// reference car: positive when the reference is ahead in the primary's
// direction of travel. Both positions are GPS, interpolated at the given
// lap fractions, and each fix must report a position accuracy strictly
// below `maxAccuracyMeters` — with worse GPS there is no answer rather
// than a noisy one. Also empty when the primary has no travel heading
// (stationary) or the cars are on different parts of the track.
std::optional<double> relativeAlongTrackMeters(
    const omatrack::UnifiedLap& primary, double primaryFraction,
    const omatrack::UnifiedLap& compare, double compareFraction,
    double maxAccuracyMeters = 1.0);

std::string confidenceLabel(const std::string& basis, int gpsAnchors);

// Look up the compare-lap fraction for a primary-lap fraction. An empty or
// degenerate map is identity: the caller must not treat "not yet aligned"
// as "the reference is at the start of the lap".
double interpolateFraction(const double* map, size_t count,
                           double primaryFraction);
double interpolateFraction(const std::vector<double>& map,
                           double primaryFraction);

// Inverse of interpolateFraction. Same identity fallback.
double invertFraction(const std::vector<double>& map, double compareFraction);

}  // namespace omatrack::alignment
