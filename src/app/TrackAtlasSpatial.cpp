#include "TrackAtlasSpatial.h"

#include "core/TelemetryEngine.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

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

QVector<QPointF> parseLineString(const QJsonArray& coordinates) {
    QVector<QPointF> points;
    points.reserve(coordinates.size());
    for (const QJsonValue& coordinateValue : coordinates) {
        const QJsonArray coordinate = coordinateValue.toArray();
        if (coordinate.size() < 2) continue;
        const double longitude =
            coordinate.at(0).toDouble(std::numeric_limits<double>::quiet_NaN());
        const double latitude =
            coordinate.at(1).toDouble(std::numeric_limits<double>::quiet_NaN());
        if (!validGpsCoordinate(latitude, longitude)) continue;
        points.append(QPointF(longitude, latitude));
    }
    return points;
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
        const double latitude = lap.gpsLat[i];
        const double longitude = lap.gpsLon[i];
        if (!validGpsCoordinate(latitude, longitude)) continue;
        minimumLatitude = std::min(minimumLatitude, latitude);
        maximumLatitude = std::max(maximumLatitude, latitude);
        minimumLongitude = std::min(minimumLongitude, longitude);
        maximumLongitude = std::max(maximumLongitude, longitude);
        ++validFixes;
    }
    return validFixes >= 10 && ((maximumLatitude - minimumLatitude) > 1e-5 ||
                                (maximumLongitude - minimumLongitude) > 1e-5);
}

QVector<QPointF> parseCenterline(const QByteArray& payload) {
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(payload, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return {};

    QVector<QJsonObject> geometries;
    const QJsonObject root = document.object();
    const QString rootType = root.value(QStringLiteral("type")).toString();
    if (rootType == QStringLiteral("FeatureCollection")) {
        for (const QJsonValue& featureValue :
             root.value(QStringLiteral("features")).toArray()) {
            const QJsonObject feature = featureValue.toObject();
            const QString role = feature.value(QStringLiteral("properties"))
                                     .toObject()
                                     .value(QStringLiteral("role"))
                                     .toString();
            if (!role.isEmpty() && role != QStringLiteral("outline") &&
                role != QStringLiteral("centerline"))
                continue;
            geometries.append(
                feature.value(QStringLiteral("geometry")).toObject());
        }
    } else if (rootType == QStringLiteral("Feature")) {
        geometries.append(root.value(QStringLiteral("geometry")).toObject());
    } else {
        geometries.append(root);
    }

    QVector<QPointF> longest;
    for (const QJsonObject& geometry : geometries) {
        if (geometry.value(QStringLiteral("type")).toString() !=
            QStringLiteral("LineString"))
            continue;
        QVector<QPointF> candidate = parseLineString(
            geometry.value(QStringLiteral("coordinates")).toArray());
        if (candidate.size() > longest.size()) longest = std::move(candidate);
    }
    return longest.size() >= 3 ? longest : QVector<QPointF>{};
}

QVector<QPointF> spatialStationMap(const UnifiedLap& lap,
                                   const QVector<QPointF>& centerline) {
    if (!hasPositionalGps(lap) || centerline.size() < 3) return {};

    constexpr double kMetersPerDegree = 111319.49079327357;
    constexpr double kRadiansPerDegree = 3.14159265358979323846 / 180.0;
    double referenceLatitude = 0.0;
    for (const QPointF& point : centerline) referenceLatitude += point.y();
    referenceLatitude /= centerline.size();
    const double longitudeScale =
        kMetersPerDegree * std::cos(referenceLatitude * kRadiansPerDegree);

    QVector<QPointF> localLine;
    localLine.reserve(centerline.size());
    for (const QPointF& point : centerline)
        localLine.append(
            QPointF(point.x() * longitudeScale, point.y() * kMetersPerDegree));

    QVector<double> cumulative(centerline.size(), 0.0);
    for (int i = 1; i < localLine.size(); ++i) {
        const double dx = localLine[i].x() - localLine[i - 1].x();
        const double dy = localLine[i].y() - localLine[i - 1].y();
        cumulative[i] = cumulative[i - 1] + std::hypot(dx, dy);
    }
    const double centerlineLength = cumulative.back();
    if (centerlineLength < 100.0) return {};

    auto project = [&](double latitude, double longitude) {
        const QPointF fix(longitude * longitudeScale,
                          latitude * kMetersPerDegree);
        AtlasProjection best;
        for (int i = 1; i < localLine.size(); ++i) {
            const QPointF start = localLine[i - 1];
            const QPointF delta = localLine[i] - start;
            const double lengthSquared =
                delta.x() * delta.x() + delta.y() * delta.y();
            if (lengthSquared <= 0.0) continue;
            const QPointF relative = fix - start;
            const double t = std::clamp(
                (relative.x() * delta.x() + relative.y() * delta.y()) /
                    lengthSquared,
                0.0, 1.0);
            const QPointF nearest = start + delta * t;
            const double errorMeters =
                std::hypot(fix.x() - nearest.x(), fix.y() - nearest.y());
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
    QVector<Anchor> anchors;
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
        anchors.append({station, fraction});
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
    QVector<QVector<double>> fractionsByStation(kStationBins + 1);
    for (const Anchor& anchor : anchors) {
        const int bin = std::clamp(
            int(std::lround(anchor.station * kStationBins)), 0, kStationBins);
        fractionsByStation[bin].append(anchor.fraction);
    }

    struct StationRow {
        double station;
        double fraction;
        double weight;
    };
    QVector<StationRow> rows;
    rows.append({0.0, 0.0, 1.0});
    for (int bin = 1; bin < kStationBins; ++bin) {
        QVector<double>& fractions = fractionsByStation[bin];
        if (fractions.isEmpty()) continue;
        std::sort(fractions.begin(), fractions.end());
        rows.append({double(bin) / kStationBins,
                     fractions[fractions.size() / 2],
                     double(fractions.size())});
    }
    rows.append({1.0, 1.0, 1.0});
    if (rows.size() < 22) return {};

    // Pool-adjacent-violators makes the station-to-time map monotonic without
    // letting one noisy fix on a nearby parallel section reverse a corner.
    struct IsotonicBlock {
        int first;
        int last;
        double weightedFraction;
        double weight;
    };
    QVector<IsotonicBlock> blocks;
    for (int row = 0; row < rows.size(); ++row) {
        blocks.append({row, row, rows[row].fraction * rows[row].weight,
                       rows[row].weight});
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
            blocks.removeLast();
            blocks.removeLast();
            blocks.append(merged);
        }
    }

    for (const IsotonicBlock& block : blocks) {
        const double fitted =
            std::clamp(block.weightedFraction / block.weight, 0.0, 1.0);
        for (int row = block.first; row <= block.last; ++row)
            rows[row].fraction = fitted;
    }
    rows.front().fraction = 0.0;
    rows.back().fraction = 1.0;

    QVector<QPointF> mapping;
    mapping.reserve(rows.size());
    for (const StationRow& row : rows)
        mapping.append(QPointF(row.station, row.fraction));
    return mapping;
}

double lapFractionAtStation(const QVector<QPointF>& mapping, double station) {
    if (mapping.size() < 2) return -1.0;
    station = std::clamp(station, 0.0, 1.0);
    const auto upper = std::lower_bound(
        mapping.cbegin(), mapping.cend(), station,
        [](const QPointF& point, double value) { return point.x() < value; });
    if (upper == mapping.cbegin()) return upper->y();
    if (upper == mapping.cend()) return mapping.back().y();
    const QPointF& high = *upper;
    const QPointF& low = *(upper - 1);
    const double span = high.x() - low.x();
    const double local = span > 0.0 ? (station - low.x()) / span : 0.0;
    return std::clamp(low.y() + (high.y() - low.y()) * local, 0.0, 1.0);
}

}  // namespace omatrack::trackatlas
