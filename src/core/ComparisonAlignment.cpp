#include "ComparisonAlignment.h"

#include "MonotonicSeries.h"
#include "TelemetryEngine.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
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

std::vector<double> lapPercentage(const omatrack::UnifiedLap& primary,
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
    return count >= 8 && first < last &&
           double(last - first) / double(lap.time.size() - 1) >= 0.5;
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

std::vector<Anchor> continuousGpsAnchors(
    const omatrack::UnifiedLap& primary, const omatrack::UnifiedLap& compare,
    const std::vector<double>& baseTimes) {
    std::vector<Anchor> anchors;
    if (!gpsAvailable(primary, compare)) return anchors;
    const size_t step =
        size_t(std::max(1.0, primary.sampleRate / kGpsAnchorRate));
    for (size_t i = 0; i < primary.time.size(); i += step) {
        const auto match =
            nearestGpsIndex(primary, i, compare, baseTimes[i]);
        if (!match) continue;
        const double compareTime = compare.time[*match];
        if (!anchors.empty() && compareTime <= anchors.back().compareTime)
            continue;
        anchors.push_back({i, compareTime});
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
    const double coverage = anchors.size() >= 2
                                ? double(anchors.back().primaryIndex -
                                         anchors.front().primaryIndex) /
                                      double(primary.time.size() - 1)
                                : 0.0;
    if (anchors.size() < 8 || occupiedBinCount < 4 || coverage < 0.5)
        anchors.clear();
    return anchors;
}

std::vector<Anchor> preCornerGpsAnchors(
    const omatrack::UnifiedLap& primary, const omatrack::UnifiedLap& compare,
    const std::vector<double>& baseTimes,
    const std::vector<double>& cornerStarts) {
    std::vector<Anchor> anchors;
    if (!gpsAvailable(primary, compare)) return anchors;
    for (double fraction : cornerStarts) {
        const size_t index = size_t(std::llround(
            std::clamp(fraction, 0.0, 1.0) * double(primary.time.size() - 1)));
        const auto match = nearestGpsIndex(primary, index, compare,
                                           baseTimes[index]);
        if (!match) continue;
        const double compareTime = compare.time[*match];
        if (!anchors.empty() && (index <= anchors.back().primaryIndex ||
                                 compareTime <= anchors.back().compareTime))
            continue;
        anchors.push_back({index, compareTime});
    }
    return anchors;
}

bool frontDamperAvailable(const omatrack::UnifiedLap& lap) {
    return lap.damperFL.size() == lap.time.size() ||
           lap.damperFR.size() == lap.time.size();
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
    const std::vector<double> primaryDamper = frontDamperSeries(primary);
    const std::vector<double> compareDamper = frontDamperSeries(compare);
    if (primaryDamper.empty() || compareDamper.empty()) return anchors;
    for (double fraction : cornerStarts) {
        const size_t index = size_t(std::llround(
            std::clamp(fraction, 0.0, 1.0) * double(primary.time.size() - 1)));
        const auto compareTime =
            damperTimeAtCorner(primary, primaryDamper, index, compare,
                               compareDamper, baseTimes[index]);
        if (!compareTime) continue;
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
    result.time = lapPercentage(primary, compare);

    switch (options.strategy) {
        case ComparisonAlignmentStrategy::GpsContinuous: {
            const auto anchors =
                continuousGpsAnchors(primary, compare, result.time);
            if (!anchors.empty()) {
                applyAnchors(result.time, anchors, compare, true);
                result.gpsAnchors = int(anchors.size());
                result.basis = "GPS \xc2\xb7 variable speed";
                break;
            }
            result.basis = "Lap percentage";
            break;
        }
        case ComparisonAlignmentStrategy::PreCornerGps: {
            const auto anchors = preCornerGpsAnchors(
                primary, compare, result.time, options.cornerStarts);
            if (!anchors.empty()) {
                applyAnchors(result.time, anchors, compare, false);
                result.gpsAnchors = int(anchors.size());
                result.basis = "GPS \xc2\xb7 pre-corner";
                break;
            }
            result.basis = "Lap percentage";
            break;
        }
        case ComparisonAlignmentStrategy::PreCornerDampers: {
            const auto anchors = preCornerDamperAnchors(
                primary, compare, result.time, options.cornerStarts);
            if (!anchors.empty()) {
                applyAnchors(result.time, anchors, compare, false);
                result.basis = "Dampers \xc2\xb7 pre-corner";
                break;
            }
            result.basis = "Lap percentage";
            break;
        }
        case ComparisonAlignmentStrategy::ManualDampers:
            result.basis = "Dampers \xc2\xb7 manual";
            break;
        case ComparisonAlignmentStrategy::LapPercentage:
            result.basis = "Lap percentage";
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

std::string confidenceLabel(const std::string& basis, int gpsAnchors) {
    if (basis.empty()) return "NONE";
    if (basis.rfind("GPS", 0) == 0)
        return gpsAnchors >= 2 ? "HIGH" : "LOW";
    if (basis == "Dampers \xc2\xb7 pre-corner")
        return "MED";
    return "LOW";
}

}  // namespace omatrack::alignment
