#include "TrackAtlasSpatial.h"

#include "TelemetryEngine.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace omatrack::trackatlas {
namespace {

bool validGpsCoordinate(double latitude, double longitude) {
    return std::isfinite(latitude) && std::isfinite(longitude) &&
           std::fabs(latitude) <= 90.0 && std::fabs(longitude) <= 180.0 &&
           (std::fabs(latitude) >= 0.001 || std::fabs(longitude) >= 0.001);
}

struct AtlasProjection {
    double station = 0.0;
    double errorMeters = std::numeric_limits<double>::max();
};

}  // namespace

bool hasPositionalGps(const UnifiedLap& lap) {
    if (lap.gpsLat.size() < 10 || lap.gpsLat.size() != lap.gpsLon.size())
        return false;
    double minimumLatitude = std::numeric_limits<double>::max();
    double maximumLatitude = std::numeric_limits<double>::lowest();
    double minimumLongitude = std::numeric_limits<double>::max();
    double maximumLongitude = std::numeric_limits<double>::lowest();
    int validFixes = 0;
    for (size_t i = 0; i < lap.gpsLat.size(); ++i) {
        if (!validGpsCoordinate(lap.gpsLat[i], lap.gpsLon[i])) continue;
        ++validFixes;
        minimumLatitude = std::min(minimumLatitude, lap.gpsLat[i]);
        maximumLatitude = std::max(maximumLatitude, lap.gpsLat[i]);
        minimumLongitude = std::min(minimumLongitude, lap.gpsLon[i]);
        maximumLongitude = std::max(maximumLongitude, lap.gpsLon[i]);
    }
    return validFixes >= 10 && ((maximumLatitude - minimumLatitude) > 1e-5 ||
                                (maximumLongitude - minimumLongitude) > 1e-5);
}

std::vector<Point> spatialStationMap(const UnifiedLap& lap,
                                     const std::vector<Point>& centerline) {
    if (!hasPositionalGps(lap) || centerline.size() < 3) return {};

    constexpr double kMetersPerDegree = 111319.49079327357;
    constexpr double kRadiansPerDegree = 3.14159265358979323846 / 180.0;
    double referenceLatitude = 0.0;
    for (const Point& point : centerline) referenceLatitude += point.y;
    referenceLatitude /= centerline.size();
    const double longitudeScale =
        kMetersPerDegree * std::cos(referenceLatitude * kRadiansPerDegree);

    std::vector<Point> localLine;
    localLine.reserve(centerline.size());
    for (const Point& point : centerline)
        localLine.push_back(
            {point.x * longitudeScale, point.y * kMetersPerDegree});

    std::vector<double> cumulative(centerline.size(), 0.0);
    for (size_t i = 1; i < localLine.size(); ++i) {
        const double dx = localLine[i].x - localLine[i - 1].x;
        const double dy = localLine[i].y - localLine[i - 1].y;
        cumulative[i] = cumulative[i - 1] + std::hypot(dx, dy);
    }
    const double centerlineLength = cumulative.back();
    if (centerlineLength < 100.0) return {};

    auto project = [&](double latitude, double longitude) {
        const Point fix{longitude * longitudeScale,
                        latitude * kMetersPerDegree};
        AtlasProjection best;
        for (size_t i = 1; i < localLine.size(); ++i) {
            const Point start = localLine[i - 1];
            const Point delta{localLine[i].x - start.x,
                              localLine[i].y - start.y};
            const double lengthSquared =
                delta.x * delta.x + delta.y * delta.y;
            if (lengthSquared <= 0.0) continue;
            const Point relative{fix.x - start.x, fix.y - start.y};
            const double t = std::clamp(
                (relative.x * delta.x + relative.y * delta.y) /
                    lengthSquared,
                0.0, 1.0);
            const Point nearest{start.x + delta.x * t,
                                start.y + delta.y * t};
            const double errorMeters =
                std::hypot(fix.x - nearest.x, fix.y - nearest.y);
            if (errorMeters >= best.errorMeters) continue;
            best.errorMeters = errorMeters;
            best.station = (cumulative[i - 1] + std::sqrt(lengthSquared) * t) /
                           centerlineLength;
        }
        return best;
    };

    struct Anchor {
        double station;
        double fraction;
    };
    std::vector<Anchor> anchors;
    const size_t stride = std::max<size_t>(1, lap.gpsLat.size() / 2500);
    double minimumStation = 1.0;
    double maximumStation = 0.0;
    for (size_t i = 0; i < lap.gpsLat.size(); i += stride) {
        const double latitude = lap.gpsLat[i];
        const double longitude = lap.gpsLon[i];
        if (!validGpsCoordinate(latitude, longitude)) continue;

        double accuracy = 0.0;
        if (lap.gpsPositionAccuracy.size() == lap.gpsLat.size())
            accuracy = lap.gpsPositionAccuracy[i];
        if (std::isfinite(accuracy) && accuracy > 50.0) continue;
        const double maximumError = std::isfinite(accuracy) && accuracy > 0.0
                                        ? std::clamp(accuracy * 3.0, 15.0, 75.0)
                                        : 50.0;
        const AtlasProjection projection = project(latitude, longitude);
        if (projection.errorMeters > maximumError) continue;

        const double fraction =
            double(i) / double(std::max<size_t>(1, lap.gpsLat.size() - 1));
        // The centerline is a closed loop. Choose the copy nearest this lap's
        // time progress solely to unwrap start/finish; station itself still
        // comes from the coordinate projection.
        const double unwrapped =
            projection.station + std::round(fraction - projection.station);
        if (unwrapped < -0.03 || unwrapped > 1.03 ||
            std::fabs(unwrapped - fraction) > 0.35)
            continue;
        const double station = std::clamp(unwrapped, 0.0, 1.0);
        anchors.push_back({station, fraction});
        minimumStation = std::min(minimumStation, station);
        maximumStation = std::max(maximumStation, station);
    }
    if (anchors.size() < 30 || minimumStation > 0.12 || maximumStation < 0.88)
        return {};

    double meanStation = 0.0;
    double meanFraction = 0.0;
    for (const Anchor& anchor : anchors) {
        meanStation += anchor.station;
        meanFraction += anchor.fraction;
    }
    meanStation /= anchors.size();
    meanFraction /= anchors.size();
    double covariance = 0.0;
    double stationVariance = 0.0;
    double fractionVariance = 0.0;
    for (const Anchor& anchor : anchors) {
        const double stationDelta = anchor.station - meanStation;
        const double fractionDelta = anchor.fraction - meanFraction;
        covariance += stationDelta * fractionDelta;
        stationVariance += stationDelta * stationDelta;
        fractionVariance += fractionDelta * fractionDelta;
    }
    const double correlation =
        covariance / std::sqrt(stationVariance * fractionVariance);
    if (!std::isfinite(correlation) || correlation < 0.75) return {};

    constexpr int kStationBins = 500;
    std::vector<std::vector<double>> fractionsByStation(kStationBins + 1);
    for (const Anchor& anchor : anchors) {
        const int bin = std::clamp(
            int(std::lround(anchor.station * kStationBins)), 0, kStationBins);
        fractionsByStation[size_t(bin)].push_back(anchor.fraction);
    }

    struct StationRow {
        double station;
        double fraction;
        double weight;
    };
    std::vector<StationRow> rows;
    rows.push_back({0.0, 0.0, 1.0});
    for (int bin = 1; bin < kStationBins; ++bin) {
        std::vector<double>& fractions = fractionsByStation[size_t(bin)];
        if (fractions.empty()) continue;
        std::sort(fractions.begin(), fractions.end());
        rows.push_back({double(bin) / kStationBins,
                        fractions[fractions.size() / 2],
                        double(fractions.size())});
    }
    rows.push_back({1.0, 1.0, 1.0});
    if (rows.size() < 22) return {};

    // Pool-adjacent-violators makes the station-to-time map monotonic without
    // letting one noisy fix on a nearby parallel section reverse a corner.
    struct IsotonicBlock {
        int first;
        int last;
        double weightedFraction;
        double weight;
    };
    std::vector<IsotonicBlock> blocks;
    for (int row = 0; row < int(rows.size()); ++row) {
        blocks.push_back({row, row, rows[size_t(row)].fraction * rows[size_t(row)].weight,
                          rows[size_t(row)].weight});
        while (blocks.size() >= 2) {
            const IsotonicBlock& previous = blocks[blocks.size() - 2];
            const IsotonicBlock& current = blocks.back();
            if (previous.weightedFraction / previous.weight <=
                current.weightedFraction / current.weight)
                break;
            IsotonicBlock merged{
                previous.first,
                current.last,
                previous.weightedFraction + current.weightedFraction,
                previous.weight + current.weight,
            };
            blocks.pop_back();
            blocks.pop_back();
            blocks.push_back(merged);
        }
    }

    for (const IsotonicBlock& block : blocks) {
        const double fitted =
            std::clamp(block.weightedFraction / block.weight, 0.0, 1.0);
        for (int row = block.first; row <= block.last; ++row)
            rows[size_t(row)].fraction = fitted;
    }
    rows.front().fraction = 0.0;
    rows.back().fraction = 1.0;

    std::vector<Point> mapping;
    mapping.reserve(rows.size());
    for (const StationRow& row : rows)
        mapping.push_back({row.station, row.fraction});
    return mapping;
}

double lapFractionAtStation(const std::vector<Point>& mapping, double station) {
    if (mapping.size() < 2) return -1.0;
    station = std::clamp(station, 0.0, 1.0);
    // Binary search by station (x) to find the bracketing pair.
    auto upper = std::lower_bound(
        mapping.begin(), mapping.end(), station,
        [](const Point& point, double value) { return point.x < value; });
    if (upper == mapping.begin()) return upper->y;
    if (upper == mapping.end()) return mapping.back().y;
    const Point& high = *upper;
    const Point& low = *(upper - 1);
    const double span = high.x - low.x;
    const double local = span > 0.0 ? (station - low.x) / span : 0.0;
    return std::clamp(low.y + (high.y - low.y) * local, 0.0, 1.0);
}

}  // namespace omatrack::trackatlas
