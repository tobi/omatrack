#include "TrackAtlasSpatial.h"

#include "core/TrackAtlasSpatial.h"
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

std::vector<Point> toCorePoints(const QVector<QPointF>& points) {
    std::vector<Point> result;
    result.reserve(size_t(points.size()));
    for (const QPointF& point : points)
        result.push_back({point.x(), point.y()});
    return result;
}

QVector<QPointF> fromCorePoints(const std::vector<Point>& points) {
    QVector<QPointF> result;
    result.reserve(int(points.size()));
    for (const Point& point : points) result.append(QPointF(point.x, point.y));
    return result;
}

}  // namespace

QVector<QPointF> parseCenterline(const QByteArray& payload) {
    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || document.isNull())
        return {};

    // A bare LineString geometry (not wrapped in a FeatureCollection).
    if (document.isObject()) {
        const QJsonObject object = document.object();
        const QString type = object.value(QStringLiteral("type")).toString();
        if (type == QStringLiteral("LineString")) {
            const auto coordinates =
                object.value(QStringLiteral("coordinates")).toArray();
            const QVector<QPointF> points = parseLineString(coordinates);
            return points.size() >= 4 ? points : QVector<QPointF>();
        }
        const auto features =
            object.value(QStringLiteral("features")).toArray();
        if (features.isEmpty()) return {};
        int longest = 0;
        QVector<QPointF> best;
        for (const QJsonValue& featureValue : features) {
            const QJsonObject feature = featureValue.toObject();
            const QJsonObject geometry =
                feature.value(QStringLiteral("geometry")).toObject();
            if (geometry.value(QStringLiteral("type")).toString() !=
                QStringLiteral("LineString"))
                continue;
            const QVector<QPointF> points = parseLineString(
                geometry.value(QStringLiteral("coordinates")).toArray());
            if (points.size() > longest) {
                longest = points.size();
                best = points;
            }
        }
        return longest >= 4 ? best : QVector<QPointF>();
    }
    return {};
}

QVector<QPointF> spatialStationMap(const UnifiedLap& lap,
                                   const QVector<QPointF>& centerline) {
    return fromCorePoints(::omatrack::trackatlas::spatialStationMap(
        lap, toCorePoints(centerline)));
}

double lapFractionAtStation(const QVector<QPointF>& mapping, double station) {
    return ::omatrack::trackatlas::lapFractionAtStation(toCorePoints(mapping),
                                                        station);
}

}  // namespace omatrack::trackatlas
