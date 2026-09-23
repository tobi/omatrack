#include "ComparisonAlignment.h"

#include "MonotonicSeries.h"
#include "TelemetryEngine.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace omatrack::alignment {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kMetersPerDegree = 111320.0;
constexpr double kGpsAnchorRate = 5.0;
// Travel-direction gate for GPS anchors: heading is measured over ±0.3 s and
// the candidate must point within 60° of the primary (cos 60° = 0.5). A
// hairpin's two legs differ by ~180°, a chicane's by 90° or more.
constexpr double kHeadingHalfWindowSeconds = 0.3;
constexpr double kHeadingMinimumTravelMeters = 3.0;
constexpr double kHeadingMinimumAgreement = 0.5;

struct Anchor {
    size_t primaryIndex = 0;
    double compareTime = 0.0;
};

// Lap distance is only a better base than lap time when it is the logger's
// own and both laps agree on the total: speed-fused distance drifts by
// several percent per lap (wheelspin, lock-ups, tyre growth), which is more
// than the pace difference between two drivers.
constexpr double kDistanceBaseTotalTolerance = 0.02;

bool distanceBaseUsable(const omatrack::UnifiedLap& primary,
                        const omatrack::UnifiedLap& compare) {
    if (primary.distanceSource != omatrack::DistanceSource::Native ||
        compare.distanceSource != omatrack::DistanceSource::Native ||
        primary.distance.size() != primary.time.size() ||
        compare.distance.size() != compare.time.size())
        return false;
    const double p = primary.distance.back() - primary.distance.front();
    const double c = compare.distance.back() - compare.distance.front();
    if (!(p > 0.0) || !(c > 0.0) || !std::isfinite(p + c)) return false;
    return std::abs(p - c) / std::max(p, c) <= kDistanceBaseTotalTolerance;
}

std::vector<double> lapTimePercentage(const omatrack::UnifiedLap& primary,
                                      const omatrack::UnifiedLap& compare) {
    std::vector<double> times(primary.time.size());
    const double primaryStart = primary.time.front();
    const double primarySpan = primary.time.back() - primaryStart;
    const double compareStart = compare.time.front();
    const double compareSpan = compare.time.back() - compareStart;
    for (size_t i = 0; i < primary.time.size(); ++i) {
        const double pct =
            primarySpan > 0.0 ? (primary.time[i] - primaryStart) / primarySpan
                              : double(i) / double(primary.time.size() - 1);
        times[i] = compareStart + std::clamp(pct, 0.0, 1.0) * compareSpan;
    }
    return times;
}

// Compare time at the same share of lap distance, for every primary sample.
std::vector<double> lapDistancePercentage(const omatrack::UnifiedLap& primary,
                                          const omatrack::UnifiedLap& compare) {
    std::vector<double> times(primary.time.size());
    const double p0 = primary.distance.front();
    const double pSpan = primary.distance.back() - p0;
    const double c0 = compare.distance.front();
    const double cSpan = compare.distance.back() - c0;
    size_t high = 1;
    for (size_t i = 0; i < primary.time.size(); ++i) {
        const double share = std::clamp((primary.distance[i] - p0) / pSpan, 0.0,
                                        1.0);
        const double target = c0 + share * cSpan;
        while (high + 1 < compare.distance.size() &&
               compare.distance[high] < target)
            ++high;
        const size_t low = high - 1;
        const double span = compare.distance[high] - compare.distance[low];
        const double local =
            span > 0.0
                ? std::clamp((target - compare.distance[low]) / span, 0.0, 1.0)
                : 0.0;
        times[i] = compare.time[low] +
                   local * (compare.time[high] - compare.time[low]);
        if (i > 0) times[i] = std::max(times[i], times[i - 1]);
    }
    return times;
}

std::vector<double> lapPercentage(const omatrack::UnifiedLap& primary,
                                  const omatrack::UnifiedLap& compare,
                                  bool* distanceBase) {
    const bool distance = distanceBaseUsable(primary, compare);
    if (distanceBase) *distanceBase = distance;
    return distance ? lapDistancePercentage(primary, compare)
                    : lapTimePercentage(primary, compare);
}

bool gpsArraysAvailable(const omatrack::UnifiedLap& lap) {
    return lap.gpsLat.size() == lap.time.size() &&
           lap.gpsLon.size() == lap.time.size() &&
           lap.gpsPositionAccuracy.size() == lap.time.size();
}

bool gpsFixUsable(double latitude, double longitude, double accuracy) {
    return std::isfinite(latitude) && std::isfinite(longitude) &&
           std::isfinite(accuracy) && std::abs(latitude) <= 90.0 &&
           std::abs(longitude) <= 180.0 &&
           (std::abs(latitude) > 1e-8 || std::abs(longitude) > 1e-8) &&
           accuracy > 0.0 && accuracy <= 25.0;
}

bool gpsCoverageAvailable(const omatrack::UnifiedLap& lap) {
    if (!gpsArraysAvailable(lap) || lap.time.size() < 8) return false;
    size_t first = lap.time.size();
    size_t last = 0;
    size_t count = 0;
    for (size_t i = 0; i < lap.time.size(); ++i) {
        if (!gpsFixUsable(lap.gpsLat[i], lap.gpsLon[i],
                          lap.gpsPositionAccuracy[i]))
            continue;
        first = std::min(first, i);
        last = i;
        ++count;
    }
    // Enough to offer GPS at all; patchy coverage is handled as re-syncs,
    // and the strategy falls back to the base when nothing verifies.
    return count >= 8 && first < last &&
           double(last - first) / double(lap.time.size() - 1) >= 0.2;
}

/// Local travel direction at `index` as a unit (north, east) vector from the
/// GPS fixes `kHeadingHalfWindowSeconds` either side. Empty when either end
/// lacks a usable fix or the car barely moved (a heading from two fixes a
/// metre apart is GPS noise, not a direction).
struct Heading {
    double north = 0.0;
    double east = 0.0;
};

std::optional<Heading> travelHeading(const omatrack::UnifiedLap& lap,
                                     size_t index) {
    const size_t half = size_t(
        std::max(1.0, std::round(lap.sampleRate * kHeadingHalfWindowSeconds)));
    const size_t before = index > half ? index - half : 0;
    const size_t after = std::min(index + half, lap.time.size() - 1);
    if (after <= before) return std::nullopt;
    if (!gpsFixUsable(lap.gpsLat[before], lap.gpsLon[before],
                      lap.gpsPositionAccuracy[before]) ||
        !gpsFixUsable(lap.gpsLat[after], lap.gpsLon[after],
                      lap.gpsPositionAccuracy[after]))
        return std::nullopt;
    const double meanLatitude =
        0.5 * (lap.gpsLat[before] + lap.gpsLat[after]) * kPi / 180.0;
    const double north =
        (lap.gpsLat[after] - lap.gpsLat[before]) * kMetersPerDegree;
    const double east = (lap.gpsLon[after] - lap.gpsLon[before]) *
                        kMetersPerDegree * std::cos(meanLatitude);
    const double length = std::hypot(north, east);
    if (length < kHeadingMinimumTravelMeters) return std::nullopt;
    return Heading{north / length, east / length};
}

std::optional<size_t> nearestGpsIndex(const omatrack::UnifiedLap& primary,
                                      size_t primaryIndex,
                                      const omatrack::UnifiedLap& compare,
                                      double baseCompareTime) {
    if (!gpsArraysAvailable(primary) || !gpsArraysAvailable(compare) ||
        primaryIndex >= primary.time.size())
        return std::nullopt;
    const double latitude = primary.gpsLat[primaryIndex];
    const double longitude = primary.gpsLon[primaryIndex];
    const double primaryAccuracy = primary.gpsPositionAccuracy[primaryIndex];
    if (!gpsFixUsable(latitude, longitude, primaryAccuracy))
        return std::nullopt;
    const auto primaryHeading = travelHeading(primary, primaryIndex);

    const auto centerIt = std::lower_bound(compare.time.begin(),
                                           compare.time.end(), baseCompareTime);
    const size_t center = std::min(size_t(centerIt - compare.time.begin()),
                                   compare.time.size() - 1);
    const size_t searchRadius = size_t(std::max(1, compare.sampleRate * 8));
    const size_t begin = center > searchRadius ? center - searchRadius : 0;
    const size_t end = std::min(center + searchRadius, compare.time.size() - 1);
    size_t best = compare.time.size();
    double bestDistance = std::numeric_limits<double>::infinity();
    for (size_t j = begin; j <= end; ++j) {
        const double compareAccuracy = compare.gpsPositionAccuracy[j];
        if (!gpsFixUsable(compare.gpsLat[j], compare.gpsLon[j],
                          compareAccuracy))
            continue;
        const double meanLatitude =
            0.5 * (latitude + compare.gpsLat[j]) * kPi / 180.0;
        const double north = (compare.gpsLat[j] - latitude) * kMetersPerDegree;
        const double east = (compare.gpsLon[j] - longitude) * kMetersPerDegree *
                            std::cos(meanLatitude);
        const double distance = std::hypot(north, east);
        if (distance >= bestDistance) continue;
        // The other leg of a hairpin, or the straight behind a chicane, can
        // be the nearest fix inside the search window while being a point
        // the car passes in the opposite direction. Reject candidates whose
        // travel direction disagrees with the primary's; a pit-lane fix or a
        // stationary car has no heading and is judged on distance alone.
        if (primaryHeading) {
            const auto compareHeading = travelHeading(compare, j);
            if (compareHeading) {
                const double agreement =
                    primaryHeading->north * compareHeading->north +
                    primaryHeading->east * compareHeading->east;
                if (agreement < kHeadingMinimumAgreement) continue;
            }
        }
        bestDistance = distance;
        best = j;
    }
    if (best == compare.time.size()) return std::nullopt;
    const double acceptance = std::min(
        35.0, std::max(18.0, primaryAccuracy +
                                 compare.gpsPositionAccuracy[best] + 8.0));
    if (bestDistance > acceptance) return std::nullopt;
    return best;
}

// A fix is trusted only if it agrees with the car: the speed implied by the
// positions ±0.5 s either side must match the vehicle speed. A receiver's own
// accuracy estimate is not enough — a SmartyCam that lost satellites reports
// "4 m" while its positions wander by hundreds of metres.
constexpr double kGpsConsistencyHalfWindowSeconds = 0.5;
constexpr double kGpsConsistencyMinimumSpeedKmh = 30.0;
constexpr double kGpsConsistencyRelativeTolerance = 0.08;
constexpr double kGpsConsistencyAbsoluteToleranceKmh = 6.0;

bool gpsSelfConsistent(const omatrack::UnifiedLap& lap, size_t index) {
    if (lap.speed.size() != lap.time.size() || index >= lap.time.size())
        return false;
    const double speed = lap.speed[index];
    if (!std::isfinite(speed) || speed < kGpsConsistencyMinimumSpeedKmh)
        return false;
    const size_t half = size_t(std::max(
        1.0, std::round(lap.sampleRate * kGpsConsistencyHalfWindowSeconds)));
    if (index < half || index + half >= lap.time.size()) return false;
    const size_t before = index - half;
    const size_t after = index + half;
    if (!gpsFixUsable(lap.gpsLat[before], lap.gpsLon[before],
                      lap.gpsPositionAccuracy[before]) ||
        !gpsFixUsable(lap.gpsLat[after], lap.gpsLon[after],
                      lap.gpsPositionAccuracy[after]))
        return false;
    const double dt = lap.time[after] - lap.time[before];
    if (!(dt > 0.0)) return false;
    const double meanLatitude =
        0.5 * (lap.gpsLat[before] + lap.gpsLat[after]) * kPi / 180.0;
    const double north =
        (lap.gpsLat[after] - lap.gpsLat[before]) * kMetersPerDegree;
    const double east = (lap.gpsLon[after] - lap.gpsLon[before]) *
                        kMetersPerDegree * std::cos(meanLatitude);
    const double derived = std::hypot(north, east) / dt * 3.6;
    return std::abs(derived - speed) <=
           std::max(kGpsConsistencyAbsoluteToleranceKmh,
                    kGpsConsistencyRelativeTolerance * speed);
}

// At the same place on track two cars rarely differ by more than this:
// a GPS match that pairs a braking car with an accelerating one is a biased
// fix, not a slower driver.
constexpr double kMatchedSpeedRelativeTolerance = 0.20;
constexpr double kMatchedSpeedAbsoluteToleranceKmh = 15.0;

bool speedsAgree(const omatrack::UnifiedLap& primary, size_t i,
                 const omatrack::UnifiedLap& compare, size_t j) {
    const double a = primary.speed[i];
    const double b = compare.speed[j];
    return std::isfinite(a) && std::isfinite(b) &&
           std::abs(a - b) <=
               std::max(kMatchedSpeedAbsoluteToleranceKmh,
                        kMatchedSpeedRelativeTolerance * std::max(a, b));
}

// Mean absolute speed difference between the primary and the compare lap at
// the mapped compare times. The correct station map minimises it; it is the
// GPS-free check that a correction improved on the base.
double mappedSpeedDisagreement(const omatrack::UnifiedLap& primary,
                               const omatrack::UnifiedLap& compare,
                               const std::vector<double>& times) {
    double sum = 0.0;
    size_t count = 0;
    size_t high = 1;
    for (size_t i = 0; i < times.size(); i += 5) {
        while (high + 1 < compare.time.size() && compare.time[high] < times[i])
            ++high;
        const size_t low = high - 1;
        const double span = compare.time[high] - compare.time[low];
        const double local =
            span > 0.0
                ? std::clamp((times[i] - compare.time[low]) / span, 0.0, 1.0)
                : 0.0;
        const double speed =
            compare.speed[low] + local * (compare.speed[high] - compare.speed[low]);
        if (!std::isfinite(speed) || !std::isfinite(primary.speed[i])) continue;
        sum += std::abs(primary.speed[i] - speed);
        ++count;
    }
    return count ? sum / double(count)
                 : std::numeric_limits<double>::infinity();
}

struct GpsAnchors {
    std::vector<Anchor> anchors;
    int rejected = 0;
    bool continuous = false;
};

GpsAnchors validatedGpsAnchors(const omatrack::UnifiedLap& primary,
                               const omatrack::UnifiedLap& compare,
                               const std::vector<double>& baseTimes) {
    GpsAnchors result;
    if (!gpsAvailable(primary, compare)) return result;
    const size_t step =
        size_t(std::max(1.0, primary.sampleRate / kGpsAnchorRate));
    for (size_t i = 0; i < primary.time.size(); i += step) {
        const auto match = nearestGpsIndex(primary, i, compare, baseTimes[i]);
        if (!match) continue;
        if (!gpsSelfConsistent(primary, i) ||
            !gpsSelfConsistent(compare, *match) ||
            !speedsAgree(primary, i, compare, *match)) {
            ++result.rejected;
            continue;
        }
        const double compareTime = compare.time[*match];
        if (!result.anchors.empty() &&
            compareTime <= result.anchors.back().compareTime)
            continue;
        result.anchors.push_back({i, compareTime});
    }
    auto& anchors = result.anchors;
    if (anchors.size() < 2) {
        anchors.clear();
        return result;
    }
    uint8_t occupiedBins = 0;
    for (const Anchor& anchor : anchors) {
        const size_t bin =
            std::min<size_t>(7, anchor.primaryIndex * 8 / primary.time.size());
        occupiedBins |= uint8_t(1U << bin);
    }
    int occupiedBinCount = 0;
    for (int i = 0; i < 8; ++i)
        occupiedBinCount += (occupiedBins & uint8_t(1U << i)) != 0 ? 1 : 0;
    const double coverage = double(anchors.back().primaryIndex -
                                   anchors.front().primaryIndex) /
                            double(primary.time.size() - 1);
    // Continuous: trusted fixes around the whole lap. Otherwise the anchors
    // are re-syncs and the base carries the stretches between them.
    result.continuous =
        anchors.size() >= 8 && occupiedBinCount >= 4 && coverage >= 0.5;
    return result;
}

std::vector<double> frontDamperSeries(const omatrack::UnifiedLap& lap);

// A mapped damper channel is not enough: loggers without the sensors carry a
// constant (often zero) channel. Require most samples finite and real motion.
bool frontDamperAvailable(const omatrack::UnifiedLap& lap) {
    if (lap.damperFL.size() != lap.time.size() &&
        lap.damperFR.size() != lap.time.size())
        return false;
    const std::vector<double> series = frontDamperSeries(lap);
    size_t finite = 0;
    double sum = 0.0;
    double squares = 0.0;
    for (double value : series) {
        if (!std::isfinite(value)) continue;
        ++finite;
        sum += value;
        squares += value * value;
    }
    if (finite < series.size() * 4 / 5 || finite < 2) return false;
    const double mean = sum / double(finite);
    const double variance = squares / double(finite) - mean * mean;
    return variance > 1e-9 * std::max(1.0, mean * mean);
}

std::vector<double> frontDamperSeries(const omatrack::UnifiedLap& lap) {
    std::vector<double> result;
    const bool left = lap.damperFL.size() == lap.time.size();
    const bool right = lap.damperFR.size() == lap.time.size();
    if (!left && !right) return result;
    result.resize(lap.time.size());
    for (size_t i = 0; i < result.size(); ++i) {
        const double a = left ? lap.damperFL[i] : lap.damperFR[i];
        const double b = right ? lap.damperFR[i] : a;
        result[i] = std::isfinite(a) && std::isfinite(b)
                        ? (a + b) * 0.5
                        : std::numeric_limits<double>::quiet_NaN();
    }
    return result;
}

std::optional<double> damperTimeAtCorner(
    const omatrack::UnifiedLap& primary,
    const std::vector<double>& primaryDamper, size_t primaryIndex,
    const omatrack::UnifiedLap& compare,
    const std::vector<double>& compareDamper, double baseCompareTime) {
    constexpr double kWindowSeconds = 2.5;
    constexpr double kSearchSeconds = 2.0;
    const size_t primaryWindow =
        size_t(std::max(1.0, primary.sampleRate * kWindowSeconds));
    if (primaryIndex < primaryWindow) return std::nullopt;
    const auto centerIt = std::lower_bound(compare.time.begin(),
                                           compare.time.end(), baseCompareTime);
    const size_t center = std::min(size_t(centerIt - compare.time.begin()),
                                   compare.time.size() - 1);
    const size_t compareWindow =
        size_t(std::max(1.0, compare.sampleRate * kWindowSeconds));
    const int search = int(std::max(1.0, compare.sampleRate * kSearchSeconds));
    constexpr size_t kCorrelationSamples = 96;
    double bestScore = -1.0;
    size_t bestIndex = compare.time.size();

    for (int shift = -search; shift <= search; ++shift) {
        const std::ptrdiff_t shifted = std::ptrdiff_t(center) + shift;
        if (shifted < std::ptrdiff_t(compareWindow) ||
            shifted >= std::ptrdiff_t(compare.time.size()))
            continue;
        const size_t compareIndex = size_t(shifted);
        double primaryMean = 0.0;
        double compareMean = 0.0;
        size_t valid = 0;
        for (size_t sample = 0; sample < kCorrelationSamples; ++sample) {
            const double local =
                double(sample) / double(kCorrelationSamples - 1);
            const size_t pi =
                primaryIndex - primaryWindow +
                size_t(std::llround(local * double(primaryWindow)));
            const size_t ci =
                compareIndex - compareWindow +
                size_t(std::llround(local * double(compareWindow)));
            if (!std::isfinite(primaryDamper[pi]) ||
                !std::isfinite(compareDamper[ci]))
                continue;
            primaryMean += primaryDamper[pi];
            compareMean += compareDamper[ci];
            ++valid;
        }
        if (valid < kCorrelationSamples * 3 / 4) continue;
        primaryMean /= double(valid);
        compareMean /= double(valid);

        double covariance = 0.0;
        double primaryVariance = 0.0;
        double compareVariance = 0.0;
        for (size_t sample = 0; sample < kCorrelationSamples; ++sample) {
            const double local =
                double(sample) / double(kCorrelationSamples - 1);
            const size_t pi =
                primaryIndex - primaryWindow +
                size_t(std::llround(local * double(primaryWindow)));
            const size_t ci =
                compareIndex - compareWindow +
                size_t(std::llround(local * double(compareWindow)));
            if (!std::isfinite(primaryDamper[pi]) ||
                !std::isfinite(compareDamper[ci]))
                continue;
            const double p = primaryDamper[pi] - primaryMean;
            const double c = compareDamper[ci] - compareMean;
            covariance += p * c;
            primaryVariance += p * p;
            compareVariance += c * c;
        }
        const double denominator = std::sqrt(primaryVariance * compareVariance);
        const double score =
            denominator > 1e-9 ? covariance / denominator : -1.0;
        if (score > bestScore) {
            bestScore = score;
            bestIndex = compareIndex;
        }
    }
    if (bestIndex == compare.time.size() || bestScore < 0.25)
        return std::nullopt;
    return compare.time[bestIndex];
}

std::vector<Anchor> preCornerDamperAnchors(
    const omatrack::UnifiedLap& primary, const omatrack::UnifiedLap& compare,
    const std::vector<double>& baseTimes,
    const std::vector<double>& cornerStarts) {
    std::vector<Anchor> anchors;
    const std::vector<double> primaryDamper =
        frontDamperSeries(primary);
    const std::vector<double> compareDamper =
        frontDamperSeries(compare);
    if (primaryDamper.empty() || compareDamper.empty()) return anchors;
    for (double fraction : cornerStarts) {
        const size_t index = size_t(std::llround(
            std::clamp(fraction, 0.0, 1.0) * double(primary.time.size() - 1)));
        const auto compareTime =
            damperTimeAtCorner(primary, primaryDamper, index, compare,
                               compareDamper, baseTimes[index]);
        if (!compareTime) continue;
        const size_t match = size_t(
            std::lower_bound(compare.time.begin(), compare.time.end(),
                             *compareTime) -
            compare.time.begin());
        if (match >= compare.time.size() ||
            !speedsAgree(primary, index, compare, match))
            continue;
        if (!anchors.empty() && (index <= anchors.back().primaryIndex ||
                                 *compareTime <= anchors.back().compareTime))
            continue;
        anchors.push_back({index, *compareTime});
    }
    return anchors;
}

void applyAnchors(std::vector<double>& times,
                  const std::vector<Anchor>& anchors,
                  const omatrack::UnifiedLap& compare, bool medianFilter) {
    if (anchors.empty()) return;
    std::vector<double> corrections;
    corrections.reserve(anchors.size());
    for (const Anchor& anchor : anchors)
        corrections.push_back(anchor.compareTime -
                              times[anchor.primaryIndex]);
    if (medianFilter && corrections.size() >= 3) {
        std::vector<double> filtered;
        filtered.reserve(corrections.size());
        for (size_t i = 0; i < corrections.size(); ++i) {
            const size_t begin = i > 2 ? i - 2 : 0;
            const size_t end = std::min(i + 3, corrections.size());
            std::vector<double> window(corrections.begin() + begin,
                                       corrections.begin() + end);
            const auto middle = window.begin() + window.size() / 2;
            std::nth_element(window.begin(), middle, window.end());
            filtered.push_back(*middle);
        }
        corrections = std::move(filtered);
    }

    size_t anchor = 0;
    for (size_t i = 0; i < times.size(); ++i) {
        double correction = corrections.front();
        if (i >= anchors.back().primaryIndex) {
            correction = corrections.back();
        } else if (i > anchors.front().primaryIndex) {
            while (anchor + 1 < anchors.size() &&
                   anchors[anchor + 1].primaryIndex < i)
                ++anchor;
            correction = corrections[anchor];
            if (anchor + 1 < anchors.size()) {
                const size_t span = anchors[anchor + 1].primaryIndex -
                                    anchors[anchor].primaryIndex;
                const double local =
                    span > 0 ? double(i - anchors[anchor].primaryIndex) /
                                   double(span)
                             : 0.0;
                correction += local * (corrections[anchor + 1] - correction);
            }
        }
        times[i] =
            std::clamp(times[i] + correction, compare.time.front(),
                       compare.time.back());
        if (i > 0)
            times[i] = std::max(times[i], times[i - 1]);
    }
}

bool isMonotonicNonDecreasing(const std::vector<double>& values) {
    for (size_t i = 1; i < values.size(); ++i)
        if (values[i] < values[i - 1] - 1e-9) return false;
    return true;
}

void buildFractions(Result& result, const omatrack::UnifiedLap& compare) {
    if (!isMonotonicNonDecreasing(compare.time)) {
        result.fraction.clear();
        result.time.clear();
        result.rejectionReason = "compare time is not monotonic";
        return;
    }
    if (!isMonotonicNonDecreasing(result.time)) {
        result.fraction.clear();
        result.time.clear();
        result.rejectionReason = "aligned time is not monotonic";
        return;
    }
    result.fraction.resize(result.time.size());
    size_t high = 1;
    for (size_t i = 0; i < result.time.size(); ++i) {
        const double time = result.time[i];
        while (high < compare.time.size() && compare.time[high] < time) ++high;
        if (high >= compare.time.size()) {
            result.fraction[i] = 1.0;
        } else if (time <= compare.time.front()) {
            result.fraction[i] = 0.0;
        } else {
            const size_t low = high - 1;
            const double span = compare.time[high] - compare.time[low];
            const double local =
                span > 0.0 ? (time - compare.time[low]) / span : 0.0;
            result.fraction[i] =
                (double(low) + local) / double(compare.time.size() - 1);
        }
    }
}
}  // namespace

bool gpsAvailable(const omatrack::UnifiedLap& primary,
                  const omatrack::UnifiedLap& compare) {
    return gpsCoverageAvailable(primary) && gpsCoverageAvailable(compare);
}

bool distanceBaseAvailable(const omatrack::UnifiedLap& primary,
                           const omatrack::UnifiedLap& compare) {
    return distanceBaseUsable(primary, compare);
}

bool damperAvailable(const omatrack::UnifiedLap& primary,
                     const omatrack::UnifiedLap& compare) {
    return frontDamperAvailable(primary) && frontDamperAvailable(compare);
}

Result compute(const omatrack::UnifiedLap& primary,
               const omatrack::UnifiedLap& compare,
               const Options& options) {
    Result result;
    if (primary.time.size() < 2 || compare.time.size() < 2) return result;
    if (!isMonotonicNonDecreasing(primary.time)) {
        result.rejectionReason = "primary time is not monotonic";
        return result;
    }
    if (!isMonotonicNonDecreasing(compare.time)) {
        result.rejectionReason = "compare time is not monotonic";
        return result;
    }
    bool distanceBase = false;
    result.time = lapPercentage(primary, compare, &distanceBase);
    result.distanceBase = distanceBase;
    const std::string base =
        distanceBase ? "Lap distance %" : "Lap time %";

    switch (options.strategy) {
        case ComparisonAlignmentStrategy::Gps: {
            const GpsAnchors gps =
                validatedGpsAnchors(primary, compare, result.time);
            result.gpsRejected = gps.rejected;
            if (!gps.anchors.empty()) {
                std::vector<double> corrected = result.time;
                applyAnchors(corrected, gps.anchors, compare, gps.continuous);
                // GPS has to earn its place: a map that agrees worse with
                // both speed traces than the base is biased GPS, not a
                // better alignment.
                if (mappedSpeedDisagreement(primary, compare, corrected) <=
                    mappedSpeedDisagreement(primary, compare, result.time)) {
                    result.time = std::move(corrected);
                    result.gpsAnchors = int(gps.anchors.size());
                    result.basis = gps.continuous
                                       ? "GPS \xc2\xb7 continuous"
                                       : "GPS \xc2\xb7 re-sync";
                    break;
                }
                result.gpsRejected += int(gps.anchors.size());
            }
            result.basis = base;
            break;
        }
        case ComparisonAlignmentStrategy::PreCornerDampers: {
            const auto anchors = preCornerDamperAnchors(
                primary, compare, result.time, options.cornerStarts);
            // Bumps are fixed to the track, so a well-correlated window is
            // trusted on its own; only anchors pairing clearly different
            // speeds are dropped (per anchor, inside the search).
            if (!anchors.empty()) {
                applyAnchors(result.time, anchors, compare, false);
                result.basis = "Dampers \xc2\xb7 pre-corner";
                break;
            }
            result.basis = base;
            break;
        }
        case ComparisonAlignmentStrategy::ManualDampers:
            result.basis = "Dampers \xc2\xb7 manual";
            break;
        case ComparisonAlignmentStrategy::LapPercentage:
            result.basis = base;
            break;
    }
    buildFractions(result, compare);
    return result;
}

namespace {
bool alignmentMapUsable(const std::vector<double>& map) {
    return map.size() >= 2 && map.back() - map.front() >= 0.01;
}
}  // namespace

double interpolateFraction(const double* map, size_t count,
                           double primaryFraction) {
    if (!map || count < 2 || !(map[count - 1] - map[0] >= 0.01))
        return std::clamp(primaryFraction, 0.0, 1.0);
    return omatrack::interpolateFraction(map, count, primaryFraction);
}

double interpolateFraction(const std::vector<double>& map,
                           double primaryFraction) {
    return interpolateFraction(map.data(), map.size(), primaryFraction);
}

double invertFraction(const std::vector<double>& map,
                      double compareFraction) {
    if (!alignmentMapUsable(map)) return std::clamp(compareFraction, 0.0, 1.0);
    return omatrack::invertFraction(map, compareFraction);
}

namespace {
struct GpsPoint {
    double latitude = 0.0;
    double longitude = 0.0;
};

// Linear interpolation between the two samples around `fraction`, each of
// which must be a usable fix at better than `maxAccuracy`.
std::optional<GpsPoint> preciseGpsAt(const omatrack::UnifiedLap& lap,
                                     double fraction, double maxAccuracy) {
    if (!gpsArraysAvailable(lap) || lap.time.size() < 2 ||
        !std::isfinite(fraction) || fraction < 0.0 || fraction > 1.0)
        return std::nullopt;
    const double position = fraction * double(lap.time.size() - 1);
    const size_t low = std::min(size_t(position), lap.time.size() - 2);
    const size_t high = low + 1;
    for (size_t i : {low, high}) {
        const double accuracy = lap.gpsPositionAccuracy[i];
        if (!gpsFixUsable(lap.gpsLat[i], lap.gpsLon[i], accuracy) ||
            !(accuracy < maxAccuracy))
            return std::nullopt;
    }
    const double local = std::clamp(position - double(low), 0.0, 1.0);
    return GpsPoint{
        lap.gpsLat[low] + (lap.gpsLat[high] - lap.gpsLat[low]) * local,
        lap.gpsLon[low] + (lap.gpsLon[high] - lap.gpsLon[low]) * local};
}
}  // namespace

std::optional<double> relativeAlongTrackMeters(
    const omatrack::UnifiedLap& primary, double primaryFraction,
    const omatrack::UnifiedLap& compare, double compareFraction,
    double maxAccuracyMeters) {
    // A reference beyond this lateral offset is not beside the primary on
    // the same stretch of track (a parallel straight, the pit lane).
    constexpr double kMaximumLateralMeters = 25.0;
    const auto from = preciseGpsAt(primary, primaryFraction, maxAccuracyMeters);
    const auto to = preciseGpsAt(compare, compareFraction, maxAccuracyMeters);
    if (!from || !to) return std::nullopt;
    const size_t index = size_t(std::llround(
        std::clamp(primaryFraction, 0.0, 1.0) * double(primary.time.size() - 1)));
    const auto heading = travelHeading(primary, index);
    if (!heading) return std::nullopt;
    const double meanLatitude =
        0.5 * (from->latitude + to->latitude) * kPi / 180.0;
    const double north = (to->latitude - from->latitude) * kMetersPerDegree;
    const double east = (to->longitude - from->longitude) * kMetersPerDegree *
                        std::cos(meanLatitude);
    const double along = north * heading->north + east * heading->east;
    const double lateral = east * heading->north - north * heading->east;
    if (std::abs(lateral) > kMaximumLateralMeters) return std::nullopt;
    return along;
}

std::string confidenceLabel(const std::string& basis, int gpsAnchors) {
    if (basis.empty()) return "NONE";
    if (basis == "GPS \xc2\xb7 continuous") return gpsAnchors >= 2 ? "HIGH" : "LOW";
    if (basis == "GPS \xc2\xb7 re-sync" || basis == "Dampers \xc2\xb7 pre-corner" ||
        basis == "Lap distance %")
        return "MED";
    return "LOW";
}

}  // namespace omatrack::alignment
