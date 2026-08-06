#include "ComparisonAlignment.h"

#include "core/TelemetryEngine.h"

#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace {
constexpr double kPi = 3.14159265358979323846;

// Speed-landmark dynamic-time-warping alignment. Returns a compare-lap time per
// primary sample (size primary.time.size()) when the warp succeeds, or an
// empty vector when speed is absent, mismatched, or too short to warp.
QVector<double> speedLandmarkAlignment(const omatrack::UnifiedLap& primary,
                                       const omatrack::UnifiedLap& compare) {
    if (primary.speed.size() != primary.time.size() ||
        compare.speed.size() != compare.time.size() ||
        primary.speed.size() < 3 || compare.speed.size() < 3)
        return {};

    constexpr int kLandmarkRate = 5;
    constexpr size_t kMaximumLandmarks = 1500;
    const size_t primaryStep = std::max(
        size_t(std::max(1, primary.sampleRate / kLandmarkRate)),
        (primary.time.size() + kMaximumLandmarks - 1) / kMaximumLandmarks);
    const size_t compareStep = std::max(
        size_t(std::max(1, compare.sampleRate / kLandmarkRate)),
        (compare.time.size() + kMaximumLandmarks - 1) / kMaximumLandmarks);
    auto sampleIndices = [](size_t count, size_t step) {
        std::vector<size_t> result;
        result.reserve(count / step + 2);
        for (size_t i = 0; i < count; i += step) result.push_back(i);
        if (result.back() != count - 1) result.push_back(count - 1);
        return result;
    };
    const std::vector<size_t> primaryIndices =
        sampleIndices(primary.time.size(), primaryStep);
    const std::vector<size_t> compareIndices =
        sampleIndices(compare.time.size(), compareStep);
    const size_t rows = primaryIndices.size();
    const size_t columns = compareIndices.size();
    if (rows < 3 || columns < 3) return {};

    constexpr float kInfinity = std::numeric_limits<float>::infinity();
    constexpr float kWarpPenalty = 0.035F;
    std::vector<float> costs(rows * columns, kInfinity);
    std::vector<uint8_t> parents(rows * columns, 0);
    const size_t band =
        std::max<size_t>(50, size_t(std::ceil(double(columns) * 0.18)));
    for (size_t row = 0; row < rows; ++row) {
        const size_t center = size_t(
            std::llround(double(row) * double(columns - 1) / double(rows - 1)));
        const size_t begin = center > band ? center - band : 0;
        const size_t end = std::min(center + band, columns - 1);
        for (size_t column = begin; column <= end; ++column) {
            const double primarySpeed = primary.speed[primaryIndices[row]];
            const double compareSpeed = compare.speed[compareIndices[column]];
            const double primarySlope =
                row > 0 ? primarySpeed - primary.speed[primaryIndices[row - 1]]
                        : 0.0;
            const double compareSlope =
                column > 0
                    ? compareSpeed - compare.speed[compareIndices[column - 1]]
                    : 0.0;
            float local =
                float(std::abs(primarySpeed - compareSpeed) / 100.0 +
                      0.25 * std::abs(primarySlope - compareSlope) / 30.0 +
                      0.08 * std::abs(double(row) / double(rows - 1) -
                                      double(column) / double(columns - 1)));
            const size_t index = row * columns + column;
            if (row == 0 && column == 0) {
                costs[index] = local;
                continue;
            }
            float best = kInfinity;
            uint8_t parent = 0;
            if (row > 0 && column > 0 &&
                costs[(row - 1) * columns + column - 1] < best) {
                best = costs[(row - 1) * columns + column - 1];
                parent = 0;
            }
            if (row > 0 &&
                costs[(row - 1) * columns + column] + kWarpPenalty < best) {
                best = costs[(row - 1) * columns + column] + kWarpPenalty;
                parent = 1;
            }
            if (column > 0 &&
                costs[row * columns + column - 1] + kWarpPenalty < best) {
                best = costs[row * columns + column - 1] + kWarpPenalty;
                parent = 2;
            }
            costs[index] = local + best;
            parents[index] = parent;
        }
    }
    if (!std::isfinite(costs.back())) return {};

    std::vector<size_t> firstMatch(rows, columns);
    std::vector<size_t> lastMatch(rows, 0);
    size_t row = rows - 1;
    size_t column = columns - 1;
    while (true) {
        firstMatch[row] = std::min(firstMatch[row], column);
        lastMatch[row] = std::max(lastMatch[row], column);
        if (row == 0 && column == 0) break;
        switch (parents[row * columns + column]) {
            case 0:
                --row;
                --column;
                break;
            case 1: --row; break;
            default: --column; break;
        }
    }

    std::vector<double> landmarkTimes(rows);
    for (size_t i = 0; i < rows; ++i) {
        if (firstMatch[i] == columns) return {};
        const size_t matched = (firstMatch[i] + lastMatch[i]) / 2;
        landmarkTimes[i] = compare.time[compareIndices[matched]];
    }

    QVector<double> result(qsizetype(primary.time.size()));
    size_t high = 1;
    for (size_t i = 0; i < primary.time.size(); ++i) {
        while (high + 1 < primaryIndices.size() && primaryIndices[high] < i)
            ++high;
        const size_t low = high - 1;
        const double span = double(primaryIndices[high] - primaryIndices[low]);
        const double local =
            span > 0.0 ? double(i - primaryIndices[low]) / span : 0.0;
        result[qsizetype(i)] =
            landmarkTimes[low] +
            local * (landmarkTimes[high] - landmarkTimes[low]);
    }
    return result;
}
}  // namespace

ComparisonAlignmentResult computeComparisonAlignment(
    const omatrack::UnifiedLap& primary,
    const omatrack::UnifiedLap& compare) {
    ComparisonAlignmentResult result;
    if (primary.time.size() < 2 || compare.time.size() < 2) return result;

    result.time = speedLandmarkAlignment(primary, compare);
    const bool speedLandmarks =
        result.time.size() == qsizetype(primary.time.size());
    if (!speedLandmarks) {
        auto timeAtProgress = [&compare](double progress) {
            progress = std::clamp(progress, 0.0, 1.0);
            const bool usableDistance =
                compare.distance.size() == compare.time.size() &&
                compare.distance.back() - compare.distance.front() > 100.0;
            if (!usableDistance) {
                const double position =
                    progress * double(compare.time.size() - 1);
                const size_t low = size_t(std::floor(position));
                const size_t high = std::min(low + 1, compare.time.size() - 1);
                return compare.time[low] +
                       (compare.time[high] - compare.time[low]) *
                           (position - double(low));
            }

            const double target = compare.distance.front() +
                                  progress * (compare.distance.back() -
                                              compare.distance.front());
            const auto upper = std::lower_bound(
                compare.distance.begin(), compare.distance.end(), target);
            if (upper == compare.distance.begin())
                return compare.time.front();
            if (upper == compare.distance.end()) return compare.time.back();
            const size_t high = size_t(upper - compare.distance.begin());
            const size_t low = high - 1;
            const double span =
                compare.distance[high] - compare.distance[low];
            const double local =
                span > 0.0 ? (target - compare.distance[low]) / span : 0.0;
            return compare.time[low] +
                   local * (compare.time[high] - compare.time[low]);
        };

        const bool primaryDistanceUsable =
            primary.distance.size() == primary.time.size() &&
            primary.distance.back() - primary.distance.front() > 100.0;
        result.time.resize(qsizetype(primary.time.size()));
        for (size_t i = 0; i < primary.time.size(); ++i) {
            const double progress =
                primaryDistanceUsable
                    ? (primary.distance[i] - primary.distance.front()) /
                          (primary.distance.back() - primary.distance.front())
                    : double(i) / double(primary.time.size() - 1);
            result.time[qsizetype(i)] = timeAtProgress(progress);
        }
    }
    result.basis =
        speedLandmarks
            ? QStringLiteral("speed landmarks")
            : (primary.distanceSource == omatrack::DistanceSource::Native &&
                       compare.distanceSource ==
                           omatrack::DistanceSource::Native
                   ? QStringLiteral("validated lap distance")
                   : QStringLiteral("wheel/GPS speed"));

    struct GpsAnchor {
        size_t primaryIndex;
        double correction;
    };
    std::vector<GpsAnchor> anchors;
    const bool gpsAvailable =
        primary.gpsLat.size() == primary.time.size() &&
        primary.gpsLon.size() == primary.time.size() &&
        primary.gpsPositionAccuracy.size() == primary.time.size() &&
        compare.gpsLat.size() == compare.time.size() &&
        compare.gpsLon.size() == compare.time.size() &&
        compare.gpsPositionAccuracy.size() == compare.time.size();
    auto gpsUsable = [](double latitude, double longitude, double accuracy) {
        return std::isfinite(latitude) && std::isfinite(longitude) &&
               std::isfinite(accuracy) && std::abs(latitude) <= 90.0 &&
               std::abs(longitude) <= 180.0 &&
               (std::abs(latitude) > 1e-8 || std::abs(longitude) > 1e-8) &&
               accuracy > 0.0 && accuracy <= 6.0;
    };

    if (gpsAvailable) {
        constexpr double kMetersPerDegree = 111320.0;
        const size_t anchorStep = size_t(std::max(1, primary.sampleRate / 2));
        const size_t searchRadius =
            size_t(std::max(1, compare.sampleRate * 8));
        for (size_t i = 0; i < primary.time.size(); i += anchorStep) {
            const double primaryAccuracy = primary.gpsPositionAccuracy[i];
            const double latitude = primary.gpsLat[i];
            const double longitude = primary.gpsLon[i];
            if (!gpsUsable(latitude, longitude, primaryAccuracy)) continue;

            const double baseTime = result.time[qsizetype(i)];
            const auto centerIt = std::lower_bound(
                compare.time.begin(), compare.time.end(), baseTime);
            const size_t center =
                std::min(size_t(centerIt - compare.time.begin()),
                         compare.time.size() - 1);
            const size_t begin =
                center > searchRadius ? center - searchRadius : 0;
            const size_t end =
                std::min(center + searchRadius, compare.time.size() - 1);
            size_t best = compare.time.size();
            double bestDistance = std::numeric_limits<double>::infinity();
            for (size_t j = begin; j <= end; ++j) {
                const double compareAccuracy = compare.gpsPositionAccuracy[j];
                if (!gpsUsable(compare.gpsLat[j], compare.gpsLon[j],
                               compareAccuracy))
                    continue;
                const double meanLatitude =
                    0.5 * (latitude + compare.gpsLat[j]) * kPi / 180.0;
                const double north =
                    (compare.gpsLat[j] - latitude) * kMetersPerDegree;
                const double east = (compare.gpsLon[j] - longitude) *
                                    kMetersPerDegree * std::cos(meanLatitude);
                const double distance = std::hypot(north, east);
                if (distance < bestDistance) {
                    bestDistance = distance;
                    best = j;
                }
            }
            if (best == compare.time.size()) continue;
            const double acceptance = std::min(
                35.0,
                std::max(18.0, primaryAccuracy +
                                   compare.gpsPositionAccuracy[best] + 8.0));
            if (bestDistance > acceptance) continue;
            if (!anchors.empty() &&
                compare.time[best] <= result.time[qsizetype(
                                           anchors.back().primaryIndex)] +
                                           anchors.back().correction)
                continue;
            anchors.push_back({i, compare.time[best] -
                                      result.time[qsizetype(i)]});
        }
    }

    // Accept GPS corrections only when good fixes cover several track
    // sections. A small spatially clustered set is usually multipath or a
    // logger clock mismatch and must not distort the landmark map.
    uint8_t occupiedBins = 0;
    for (const GpsAnchor& anchor : anchors) {
        const size_t bin =
            std::min<size_t>(7, anchor.primaryIndex * 8 / primary.time.size());
        occupiedBins |= uint8_t(1U << bin);
    }
    int occupiedBinCount = 0;
    for (int i = 0; i < 8; ++i)
        occupiedBinCount += (occupiedBins & uint8_t(1U << i)) != 0 ? 1 : 0;
    const double anchorCoverage = anchors.size() >= 2
                                      ? double(anchors.back().primaryIndex -
                                               anchors.front().primaryIndex) /
                                            double(primary.time.size() - 1)
                                      : 0.0;
    const bool gpsDistributed =
        anchors.size() >= 8 && occupiedBinCount >= 4 && anchorCoverage >= 0.5;
    if (gpsDistributed) {
        std::vector<double> filtered;
        filtered.reserve(anchors.size());
        for (size_t i = 0; i < anchors.size(); ++i) {
            const size_t begin = i > 2 ? i - 2 : 0;
            const size_t end = std::min(i + 3, anchors.size());
            std::vector<double> window;
            window.reserve(end - begin);
            for (size_t j = begin; j < end; ++j)
                window.push_back(anchors[j].correction);
            const auto middle = window.begin() + window.size() / 2;
            std::nth_element(window.begin(), middle, window.end());
            filtered.push_back(*middle);
        }
        for (size_t i = 0; i < anchors.size(); ++i)
            anchors[i].correction = filtered[i];
        const int gpsAnchorCount = int(anchors.size());
        // Lap boundaries are start/finish-line station anchors. Never extend
        // the first/last intermittent GPS correction backward over them.
        anchors.insert(anchors.begin(), {0, 0.0});
        anchors.push_back({primary.time.size() - 1, 0.0});

        size_t anchor = 0;
        for (size_t i = 0; i < primary.time.size(); ++i) {
            while (anchor + 1 < anchors.size() &&
                   anchors[anchor + 1].primaryIndex < i)
                ++anchor;
            double correction = anchors[anchor].correction;
            if (anchor + 1 < anchors.size() &&
                anchors[anchor + 1].primaryIndex >
                    anchors[anchor].primaryIndex) {
                const double fraction =
                    std::clamp(double(i - anchors[anchor].primaryIndex) /
                                   double(anchors[anchor + 1].primaryIndex -
                                          anchors[anchor].primaryIndex),
                               0.0, 1.0);
                correction +=
                    fraction * (anchors[anchor + 1].correction - correction);
            }
            result.time[qsizetype(i)] =
                std::clamp(result.time[qsizetype(i)] + correction,
                           compare.time.front(), compare.time.back());
            if (i > 0)
                result.time[qsizetype(i)] =
                    std::max(result.time[qsizetype(i)],
                             result.time[qsizetype(i - 1)]);
        }
        result.gpsAnchors = gpsAnchorCount;
        result.basis =
            speedLandmarks ? QStringLiteral("GPS anchored · speed landmarks")
                           : QStringLiteral("GPS anchored · wheel/GPS speed");
    }

    result.fraction.resize(result.time.size());
    size_t high = 1;
    for (qsizetype i = 0; i < result.time.size(); ++i) {
        const double time = result.time[i];
        while (high < compare.time.size() && compare.time[high] < time)
            ++high;
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
    return result;
}

QString comparisonAlignmentConfidenceLabel(const QString& basis,
                                            int gpsAnchors) {
    if (basis.isEmpty()) return QStringLiteral("NONE");
    if (gpsAnchors >= 8) return QStringLiteral("HIGH");
    if (basis.contains(QStringLiteral("speed landmarks")) ||
        basis == QStringLiteral("validated lap distance"))
        return QStringLiteral("MED");
    return QStringLiteral("LOW");
}
