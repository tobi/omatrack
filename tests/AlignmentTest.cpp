// Focused regression coverage for the pure comparison-alignment strategies
// shared by traces, delta, cursor readouts, and synchronized video.

#include "app/ComparisonAlignment.h"
#include "core/TelemetryEngine.h"

#include <QtTest>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
constexpr double kPi = 3.14159265358979323846;

omatrack::UnifiedLap makeLap(int samples, bool gps = false,
                             bool dampers = false) {
    omatrack::UnifiedLap lap;
    lap.sampleRate = 50;
    lap.distanceSource = omatrack::DistanceSource::SpeedFused;
    lap.time.reserve(samples);
    lap.speed.reserve(samples);
    lap.distance.reserve(samples);
    if (gps) {
        lap.gpsLat.reserve(samples);
        lap.gpsLon.reserve(samples);
        lap.gpsPositionAccuracy.reserve(samples);
    }
    if (dampers) {
        lap.damperFL.reserve(samples);
        lap.damperFR.reserve(samples);
    }
    for (int i = 0; i < samples; ++i) {
        const double fraction =
            samples > 1 ? double(i) / double(samples - 1) : 0.0;
        lap.time.push_back(i / 50.0);
        lap.speed.push_back(145.0 + 50.0 * std::sin(6.0 * kPi * fraction));
        lap.distance.push_back(1000.0 * fraction);
        if (gps) {
            lap.gpsLat.push_back(43.0 + 0.0001 * i);
            lap.gpsLon.push_back(-88.0 + 0.00008 * i);
            lap.gpsPositionAccuracy.push_back(1.0);
        }
        if (dampers) {
            const double value = std::sin(0.0017 * i * i) +
                                 0.35 * std::sin(0.19 * i) +
                                 0.12 * std::cos(0.047 * i);
            lap.damperFL.push_back(value);
            lap.damperFR.push_back(value + 0.04 * std::sin(0.31 * i));
        }
    }
    return lap;
}

ComparisonAlignmentOptions options(ComparisonAlignmentStrategy strategy,
                                   std::initializer_list<double> corners = {}) {
    ComparisonAlignmentOptions result;
    result.strategy = strategy;
    for (double corner : corners) result.cornerStarts.append(corner);
    return result;
}

bool approx(double a, double b, double epsilon = 1e-6) {
    return std::abs(a - b) <= epsilon;
}

bool monotonic(const QVector<double>& values) {
    for (qsizetype i = 1; i < values.size(); ++i)
        if (values[i] + 1e-9 < values[i - 1]) return false;
    return true;
}

bool bounded(const QVector<double>& values, double low, double high) {
    for (double value : values)
        if (value < low - 1e-9 || value > high + 1e-9) return false;
    return true;
}
}  // namespace

class StrategyTest : public QObject {
    Q_OBJECT
private slots:
    void lapPercentageIgnoresSpeedFusedDistanceDrift() {
        constexpr int kSamples = 1000;
        auto primary = makeLap(kSamples);
        auto compare = primary;
        for (int i = 0; i < kSamples; ++i) {
            const double p = double(i) / double(kSamples - 1);
            primary.distance[size_t(i)] =
                1000.0 * (p - 0.05 * std::sin(kPi * p));
            compare.distance[size_t(i)] =
                1000.0 * (p + 0.08 * std::sin(kPi * p));
        }

        const auto result = computeComparisonAlignment(
            primary, compare,
            options(ComparisonAlignmentStrategy::LapPercentage));
        for (int index : {100, 500, 780, 900}) {
            const double expected = double(index) / double(kSamples - 1);
            QVERIFY(std::abs(result.fraction[index] - expected) < 1e-6);
        }
    }

    void gpsContinuousCorrectsVariableTrackProgress() {
        constexpr int kSamples = 1000;
        auto primary = makeLap(kSamples, true);
        auto compare = primary;
        for (int i = 0; i < kSamples; ++i) {
            const double q = double(i) / double(kSamples - 1);
            const double station = q + 0.04 * std::sin(kPi * q);
            compare.gpsLat[size_t(i)] =
                43.0 + 0.0001 * station * double(kSamples - 1);
            compare.gpsLon[size_t(i)] =
                -88.0 + 0.00008 * station * double(kSamples - 1);
        }
        const auto percentage = computeComparisonAlignment(
            primary, compare,
            options(ComparisonAlignmentStrategy::LapPercentage));
        const auto gps = computeComparisonAlignment(
            primary, compare,
            options(ComparisonAlignmentStrategy::GpsContinuous));

        const int nearFinish = 900;
        const double primaryStation = double(nearFinish) / double(kSamples - 1);
        auto mappedStation = [&](const ComparisonAlignmentResult& result) {
            const double q = result.fraction[nearFinish];
            return q + 0.04 * std::sin(kPi * q);
        };
        QVERIFY(std::abs(mappedStation(percentage) - primaryStation) > 0.008);
        QVERIFY(std::abs(mappedStation(gps) - primaryStation) < 0.003);
        QCOMPARE(gps.basis, QStringLiteral("GPS · variable speed"));
        QVERIFY(gps.gpsAnchors >= 8);
        QVERIFY(monotonic(gps.fraction));
        QVERIFY(bounded(gps.fraction, 0.0, 1.0));
    }

    void preCornerGpsPinsTurnIns() {
        constexpr int kSamples = 900;
        constexpr int kShift = 14;
        auto primary = makeLap(kSamples, true);
        auto compare = primary;
        for (int i = 0; i < kSamples; ++i) {
            const int source = i - kShift;
            compare.gpsLat[size_t(i)] = 43.0 + 0.0001 * source;
            compare.gpsLon[size_t(i)] = -88.0 + 0.00008 * source;
        }

        const auto result = computeComparisonAlignment(
            primary, compare,
            options(ComparisonAlignmentStrategy::PreCornerGps,
                    {0.30, 0.55, 0.80}));
        QCOMPARE(result.basis, QStringLiteral("GPS · pre-corner"));
        QCOMPARE(result.gpsAnchors, 3);
        for (double corner : {0.30, 0.55, 0.80}) {
            const int index = qRound(corner * (kSamples - 1));
            const double expected =
                double(index + kShift) / double(kSamples - 1);
            QVERIFY(std::abs(result.fraction[index] - expected) < 0.004);
        }
    }

    void gpsAnchorsRejectTheOtherLegOfAHairpin() {
        // Out-leg north for 10 s at 20 m/s, hairpin, return leg 8 m to the
        // east. The reference lags by 0.5 s (10 m). Past t ≈ 6 s the nearest
        // reference fix inside the ±8 s window is on the *return* leg (8 m),
        // not the true match (10 m). Without a travel-direction gate that
        // one wrong anchor is accepted, every true anchor behind it is then
        // rejected as non-monotonic, and the map interpolates across a
        // seven-second hole to the wrong side of the hairpin.
        constexpr int kRate = 50;
        constexpr double kLegSeconds = 10.0;
        constexpr double kLag = 0.5;
        constexpr int kSamples = int((2 * kLegSeconds + kLag) * kRate) + 1;
        constexpr double kSpeed = 20.0;  // m/s
        constexpr double kMetersPerDegree = 111320.0;
        const double lonScale =
            1.0 / (kMetersPerDegree * std::cos(43.0 * kPi / 180.0));
        auto position = [&](double t, double* lat, double* lon) {
            t = std::clamp(t, 0.0, 2 * kLegSeconds);
            const double north = t <= kLegSeconds
                                     ? kSpeed * t
                                     : kSpeed * (2 * kLegSeconds - t);
            const double east = t <= kLegSeconds ? 0.0 : 8.0;
            *lat = 43.0 + north / kMetersPerDegree;
            *lon = -88.0 + east * lonScale;
        };
        auto primary = makeLap(kSamples, true);
        auto compare = primary;
        for (int i = 0; i < kSamples; ++i) {
            const double t = double(i) / kRate;
            position(t, &primary.gpsLat[size_t(i)], &primary.gpsLon[size_t(i)]);
            position(t - kLag, &compare.gpsLat[size_t(i)],
                     &compare.gpsLon[size_t(i)]);
        }

        const auto result = computeComparisonAlignment(
            primary, compare,
            options(ComparisonAlignmentStrategy::GpsContinuous));
        QCOMPARE(result.basis, QStringLiteral("GPS · variable speed"));
        for (double t : {3.0, 7.0, 8.0, 9.0, 12.0, 15.0}) {
            const int index = int(t * kRate);
            const double mapped = result.time[index];
            QVERIFY2(std::abs(mapped - (t + kLag)) < 0.25,
                     qPrintable(QStringLiteral("t=%1 mapped to %2, want %3")
                                    .arg(t)
                                    .arg(mapped)
                                    .arg(t + kLag)));
        }
    }

    void preCornerDampersMatchLocalSignature() {
        constexpr int kSamples = 1200;
        constexpr int kShift = 11;
        auto primary = makeLap(kSamples, false, true);
        auto compare = primary;
        for (int i = 0; i < kSamples; ++i) {
            const int source = std::max(0, i - kShift);
            compare.damperFL[size_t(i)] = primary.damperFL[size_t(source)];
            compare.damperFR[size_t(i)] = primary.damperFR[size_t(source)];
        }

        const auto result = computeComparisonAlignment(
            primary, compare,
            options(ComparisonAlignmentStrategy::PreCornerDampers,
                    {0.30, 0.55, 0.80}));
        QCOMPARE(result.basis, QStringLiteral("Dampers · pre-corner"));
        for (double corner : {0.30, 0.55, 0.80}) {
            const int index = qRound(corner * (kSamples - 1));
            const double expected =
                double(index + kShift) / double(kSamples - 1);
            QVERIFY(std::abs(result.fraction[index] - expected) < 0.006);
        }
    }

    void manualDampersUsesPercentageUntilUserOffsetsIt() {
        auto primary = makeLap(300, false, true);
        auto compare = primary;
        const auto result = computeComparisonAlignment(
            primary, compare,
            options(ComparisonAlignmentStrategy::ManualDampers));
        QCOMPARE(result.basis, QStringLiteral("Dampers · manual"));
        QVERIFY(approx(result.fraction[150], 150.0 / 299.0));
    }

    void unavailableStrategiesFallBackHonestly() {
        auto primary = makeLap(300);
        auto compare = primary;
        const auto gps = computeComparisonAlignment(
            primary, compare,
            options(ComparisonAlignmentStrategy::GpsContinuous));
        const auto dampers = computeComparisonAlignment(
            primary, compare,
            options(ComparisonAlignmentStrategy::PreCornerDampers, {0.5}));
        QCOMPARE(gps.basis, QStringLiteral("Lap percentage"));
        QCOMPARE(dampers.basis, QStringLiteral("Lap percentage"));
        QCOMPARE(gps.gpsAnchors, 0);
    }
};

class CapabilityTest : public QObject {
    Q_OBJECT
private slots:
    void reportsOnlyDataBothLapsCarry() {
        auto complete = makeLap(300, true, true);
        auto missing = makeLap(300);
        QVERIFY(comparisonGpsAlignmentAvailable(complete, complete));
        QVERIFY(comparisonDamperAlignmentAvailable(complete, complete));
        QVERIFY(!comparisonGpsAlignmentAvailable(complete, missing));
        QVERIFY(!comparisonDamperAlignmentAvailable(complete, missing));
    }

    void clusteredGpsIsNotSensible() {
        auto primary = makeLap(300, true);
        auto compare = primary;
        const double nan = std::numeric_limits<double>::quiet_NaN();
        for (int i = 40; i < 300; ++i) {
            primary.gpsLat[size_t(i)] = nan;
            primary.gpsLon[size_t(i)] = nan;
            primary.gpsPositionAccuracy[size_t(i)] = nan;
        }
        QVERIFY(!comparisonGpsAlignmentAvailable(primary, compare));
    }
};

class AlignmentUtilityTest : public QObject {
    Q_OBJECT
private slots:
    void confidenceReflectsStrategy() {
        QCOMPARE(comparisonAlignmentConfidenceLabel(QString(), 0),
                 QStringLiteral("NONE"));
        QCOMPARE(comparisonAlignmentConfidenceLabel(
                     QStringLiteral("GPS · variable speed"), 20),
                 QStringLiteral("HIGH"));
        QCOMPARE(comparisonAlignmentConfidenceLabel(
                     QStringLiteral("GPS · pre-corner"), 3),
                 QStringLiteral("HIGH"));
        QCOMPARE(comparisonAlignmentConfidenceLabel(
                     QStringLiteral("Dampers · pre-corner"), 0),
                 QStringLiteral("MED"));
        QCOMPARE(comparisonAlignmentConfidenceLabel(
                     QStringLiteral("Lap percentage"), 0),
                 QStringLiteral("LOW"));
    }

    void fractionLookupRoundTrips() {
        QVector<double> map;
        for (int i = 0; i < 21; ++i) map.append(double(i) / 20.0);
        QVERIFY(approx(interpolateAlignmentFraction(map, 0.35), 0.35));
        QVERIFY(approx(invertAlignmentFraction(map, 0.35), 0.35));
        const QVector<double> collapsed{0.0, 0.0, 0.0};
        QVERIFY(approx(interpolateAlignmentFraction(collapsed, 0.42), 0.42));
        QVERIFY(approx(invertAlignmentFraction(collapsed, 0.42), 0.42));
    }

    void tinyLapProducesNoAlignment() {
        auto primary = makeLap(1, true, true);
        auto compare = makeLap(100, true, true);
        const auto result = computeComparisonAlignment(primary, compare);
        QVERIFY(result.time.isEmpty());
        QVERIFY(result.fraction.isEmpty());
        QVERIFY(result.basis.isEmpty());
    }
};

int main(int argc, char* argv[]) {
    int status = 0;
    {
        StrategyTest test;
        status |= QTest::qExec(&test, argc, argv);
    }
    {
        CapabilityTest test;
        status |= QTest::qExec(&test, argc, argv);
    }
    {
        AlignmentUtilityTest test;
        status |= QTest::qExec(&test, argc, argv);
    }
    return status;
}

#include "AlignmentTest.moc"
