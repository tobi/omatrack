#include "app/TrackAtlasSpatial.h"
#include "core/TelemetryEngine.h"

#include <QtTest>

#include <cmath>

namespace {

QVector<QPointF> circularCenterline() {
    constexpr int kSegments = 500;
    constexpr double kPi = 3.14159265358979323846;
    constexpr double kCenterLatitude = 43.8;
    constexpr double kCenterLongitude = -88.0;
    constexpr double kMetersPerDegree = 111319.49079327357;
    const double longitudeScale =
        kMetersPerDegree * std::cos(kCenterLatitude * kPi / 180.0);
    QVector<QPointF> points;
    points.reserve(kSegments + 1);
    for (int segment = 0; segment <= kSegments; ++segment) {
        const double angle = 2.0 * kPi * double(segment) / double(kSegments);
        points.append(QPointF(
            kCenterLongitude + std::cos(angle) * 700.0 / longitudeScale,
            kCenterLatitude + std::sin(angle) * 700.0 / kMetersPerDegree));
    }
    return points;
}

omatrack::UnifiedLap spatialLap(double accuracyMeters = 2.0) {
    constexpr int kSamples = 1001;
    constexpr double kPi = 3.14159265358979323846;
    constexpr double kCenterLatitude = 43.8;
    constexpr double kCenterLongitude = -88.0;
    constexpr double kMetersPerDegree = 111319.49079327357;
    const double longitudeScale =
        kMetersPerDegree * std::cos(kCenterLatitude * kPi / 180.0);
    omatrack::UnifiedLap lap;
    lap.gpsLat.reserve(kSamples);
    lap.gpsLon.reserve(kSamples);
    lap.gpsPositionAccuracy.reserve(kSamples);
    lap.distance.reserve(kSamples);
    for (int sample = 0; sample < kSamples; ++sample) {
        const double fraction = double(sample) / double(kSamples - 1);
        const double station = fraction * fraction;
        const double angle = 2.0 * kPi * station;
        lap.gpsLon.push_back(kCenterLongitude +
                             std::cos(angle) * 700.0 / longitudeScale);
        lap.gpsLat.push_back(kCenterLatitude +
                             std::sin(angle) * 700.0 / kMetersPerDegree);
        lap.gpsPositionAccuracy.push_back(accuracyMeters);
        // Deliberately unrelated to the Track Atlas station. The spatial map
        // must use the coordinates, not this telemetry distance axis.
        lap.distance.push_back(10000.0 * fraction);
    }
    return lap;
}

}  // namespace

class TrackAtlasSpatialTest : public QObject {
    Q_OBJECT

private slots:
    void parsesGeoJsonCenterline() {
        const QByteArray geoJson = R"json(
            {"type":"FeatureCollection","features":[
              {"type":"Feature","properties":{"role":"pit_lane"},
               "geometry":{"type":"LineString","coordinates":[[-88.0,43.8],[-88.1,43.9]]}},
              {"type":"Feature","properties":{"role":"outline"},
               "geometry":{"type":"LineString","coordinates":[[-88.0,43.8],[-88.01,43.81],[-88.02,43.8],[-88.0,43.8]]}}
            ]}
        )json";
        const QVector<QPointF> centerline =
            omatrack::trackatlas::parseCenterline(geoJson);
        QCOMPARE(centerline.size(), 4);
        QCOMPARE(centerline.front(), QPointF(-88.0, 43.8));
    }

    void rejectsAbsentGps() {
        omatrack::UnifiedLap lap;
        lap.gpsLat.assign(100, 0.0);
        lap.gpsLon.assign(100, 0.0);
        QVERIFY(!omatrack::trackatlas::hasPositionalGps(lap));
    }

    void mapsAtlasStationsFromCoordinates() {
        const omatrack::UnifiedLap lap = spatialLap();
        QVERIFY(omatrack::trackatlas::hasPositionalGps(lap));
        const QVector<QPointF> mapping =
            omatrack::trackatlas::spatialStationMap(lap, circularCenterline());
        QVERIFY(mapping.size() > 100);

        const double quarter =
            omatrack::trackatlas::lapFractionAtStation(mapping, 0.25);
        const double eightyOne =
            omatrack::trackatlas::lapFractionAtStation(mapping, 0.81);
        QVERIFY(std::fabs(quarter - 0.5) < 0.02);
        QVERIFY(std::fabs(eightyOne - 0.9) < 0.02);
        for (int i = 1; i < mapping.size(); ++i) {
            QVERIFY(mapping[i].x() > mapping[i - 1].x());
            QVERIFY(mapping[i].y() >= mapping[i - 1].y());
        }
    }

    void rejectsReportedPoorAccuracy() {
        const omatrack::UnifiedLap lap = spatialLap(250.0);
        QVERIFY(omatrack::trackatlas::hasPositionalGps(lap));
        QVERIFY(
            omatrack::trackatlas::spatialStationMap(lap, circularCenterline())
                .isEmpty());
    }

    void rejectsDistantTrace() {
        omatrack::UnifiedLap lap = spatialLap(0.0);
        for (double& latitude : lap.gpsLat) latitude += 0.02;
        QVERIFY(omatrack::trackatlas::hasPositionalGps(lap));
        QVERIFY(
            omatrack::trackatlas::spatialStationMap(lap, circularCenterline())
                .isEmpty());
    }
};

QTEST_MAIN(TrackAtlasSpatialTest)
#include "TrackAtlasSpatialTest.moc"
