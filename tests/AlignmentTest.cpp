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

constexpr double kMetersPerDegree = 111320.0;

// Positions follow the speed channel along a straight north-east line, so
// GPS is physically consistent with the car (the verified-GPS strategy
// rejects fixes that are not).
void placeOnLine(double meters, double* lat, double* lon) {
    const double north = meters * 0.8;
    const double east = meters * 0.6;
    *lat = 43.0 + north / kMetersPerDegree;
    *lon = -88.0 + east / (kMetersPerDegree * std::cos(43.0 * kPi / 180.0));
}

omatrack::UnifiedLap makeLap(int samples, bool gps = false,
                             bool dampers = false) {
    omatrack::UnifiedLap lap;
    lap.sampleRate = 50;
    lap.distanceSource = omatrack::DistanceSource::SpeedFused;
    double meters = 0.0;
    for (int i = 0; i < samples; ++i) {
        const double fraction =
            samples > 1 ? double(i) / double(samples - 1) : 0.0;
        const double speed = 145.0 + 50.0 * std::sin(6.0 * kPi * fraction);
        if (i > 0) meters += speed / 3.6 / 50.0;
        lap.time.push_back(i / 50.0);
        lap.speed.push_back(speed);
        lap.distance.push_back(meters);
        if (gps) {
            double lat = 0.0;
            double lon = 0.0;
            placeOnLine(meters, &lat, &lon);
            lap.gpsLat.push_back(lat);
            lap.gpsLon.push_back(lon);
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

// The compare lap drives the primary's line on a warped clock: at its time t
// it is where the primary was at tau(t) = t + a·sin(pi·t/T). Speeds follow
// (scaled by tau'), GPS and distance follow the primary's position. The true
// map sends primary time s to the compare time t with tau(t) = s.
omatrack::UnifiedLap warpedLap(const omatrack::UnifiedLap& primary,
                               double amplitude) {
    omatrack::UnifiedLap lap = primary;
    const double total = primary.time.back();
    const auto sampleAt = [&](const std::vector<double>& values, double t) {
        const double position = std::clamp(t * 50.0, 0.0,
                                           double(values.size() - 1));
        const size_t low = std::min(size_t(position), values.size() - 2);
        return values[low] +
               (values[low + 1] - values[low]) * (position - double(low));
    };
    for (size_t i = 0; i < lap.time.size(); ++i) {
        const double t = lap.time[i];
        const double tau = t + amplitude * std::sin(kPi * t / total);
        const double rate =
            1.0 + amplitude * kPi / total * std::cos(kPi * t / total);
        lap.speed[i] = sampleAt(primary.speed, tau) * rate;
        lap.distance[i] = sampleAt(primary.distance, tau);
        if (!lap.gpsLat.empty()) placeOnLine(lap.distance[i], &lap.gpsLat[i],
                                             &lap.gpsLon[i]);
    }
    return lap;
}

double trueCompareTime(double primaryTime, double total, double amplitude) {
    double t = primaryTime;
    for (int k = 0; k < 40; ++k)
        t -= (t + amplitude * std::sin(kPi * t / total) - primaryTime) /
             (1.0 + amplitude * kPi / total * std::cos(kPi * t / total));
    return t;
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
    void nonOwningLookupMatchesVectorAndFallback() {
        const std::vector<double> map{0.03, 0.2, 0.7, 0.98};
        for (double fraction : {-0.1, 0.0, 0.15, 0.5, 0.95, 1.1})
            QCOMPARE(omatrack::alignment::interpolateFraction(
                         map.data(), map.size(), fraction),
                     omatrack::alignment::interpolateFraction(map, fraction));
        QCOMPARE(omatrack::alignment::interpolateFraction(nullptr, 0, 0.3),
                 0.3);
        const double constant[] = {0.2, 0.2};
        QCOMPARE(omatrack::alignment::interpolateFraction(constant, 2, 0.3),
                 0.3);
        const double invalid[] = {0.2,
                                  std::numeric_limits<double>::quiet_NaN()};
        QCOMPARE(omatrack::alignment::interpolateFraction(invalid, 2, 0.3),
                 0.3);
    }

    void lapPercentageUsesTimeOverSpeedFusedDistance() {
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
        QCOMPARE(result.basis, QStringLiteral("Lap time %"));
        for (int index : {100, 500, 780, 900}) {
            const double expected = double(index) / double(kSamples - 1);
            QVERIFY(std::abs(result.fraction[index] - expected) < 1e-6);
        }
    }

    void lapPercentageUsesNativeDistanceWhenTotalsAgree() {
        // A slower first half: lap time % puts the reference in the wrong
        // place; the logger's own distance does not.
        auto primary = makeLap(1500);
        primary.distanceSource = omatrack::DistanceSource::Native;
        const double amplitude = 1.5;
        auto compare = warpedLap(primary, amplitude);
        compare.distanceSource = omatrack::DistanceSource::Native;
        const auto result = computeComparisonAlignment(
            primary, compare,
            options(ComparisonAlignmentStrategy::LapPercentage));
        QCOMPARE(result.basis, QStringLiteral("Lap distance %"));
        const double total = primary.time.back();
        for (int index : {300, 750, 1200}) {
            const double want =
                trueCompareTime(primary.time[size_t(index)], total, amplitude);
            QVERIFY2(std::abs(result.time[index] - want) < 0.03,
                     qPrintable(QStringLiteral("%1 vs %2")
                                    .arg(result.time[index])
                                    .arg(want)));
        }
        // Totals that disagree by more than 2 % are not the logger's truth.
        compare.distance.back() *= 1.05;
        for (auto& d : compare.distance) d *= 1.05;
        QCOMPARE(computeComparisonAlignment(
                     primary, compare,
                     options(ComparisonAlignmentStrategy::LapPercentage))
                     .basis,
                 QStringLiteral("Lap time %"));
    }

    void verifiedGpsCorrectsVariableTrackProgress() {
        auto primary = makeLap(1500, true);
        const double amplitude = 1.5;
        const auto compare = warpedLap(primary, amplitude);
        const auto percentage = computeComparisonAlignment(
            primary, compare,
            options(ComparisonAlignmentStrategy::LapPercentage));
        const auto gps = computeComparisonAlignment(
            primary, compare, options(ComparisonAlignmentStrategy::Gps));
        QCOMPARE(gps.basis, QStringLiteral("GPS · continuous"));
        QVERIFY(gps.gpsAnchors >= 8);
        QVERIFY(monotonic(gps.fraction));
        QVERIFY(bounded(gps.fraction, 0.0, 1.0));
        const double total = primary.time.back();
        const int middle = 750;
        const double want =
            trueCompareTime(primary.time[middle], total, amplitude);
        QVERIFY(std::abs(percentage.time[middle] - want) > 1.0);
        QVERIFY2(std::abs(gps.time[middle] - want) < 0.1,
                 qPrintable(QStringLiteral("%1 vs %2")
                                .arg(gps.time[middle])
                                .arg(want)));
    }

    void patchyGpsResyncsWhereItIsGood() {
        auto primary = makeLap(1500, true);
        const double amplitude = 1.5;
        auto compare = warpedLap(primary, amplitude);
        const double nan = std::numeric_limits<double>::quiet_NaN();
        // Good GPS only in two stretches of each lap.
        for (auto* lap : {&primary, &compare})
            for (size_t i = 0; i < lap->time.size(); ++i) {
                const double f = double(i) / double(lap->time.size() - 1);
                if ((f > 0.26 && f < 0.37) || (f > 0.63 && f < 0.74)) continue;
                lap->gpsLat[i] = lap->gpsLon[i] = nan;
            }
        const auto gps = computeComparisonAlignment(
            primary, compare, options(ComparisonAlignmentStrategy::Gps));
        QCOMPARE(gps.basis, QStringLiteral("GPS · re-sync"));
        const double total = primary.time.back();
        for (int index : {470, 1030}) {
            const double want =
                trueCompareTime(primary.time[size_t(index)], total, amplitude);
            QVERIFY(std::abs(gps.time[index] - want) < 0.1);
        }
    }

    void gpsThatDisagreesWithTheCarIsIgnored() {
        // Positions three seconds behind the car (a receiver that has lost
        // its fix but still reports a small accuracy figure).
        auto primary = makeLap(1500, true);
        auto compare = primary;
        for (size_t i = 0; i < compare.time.size(); ++i)
            placeOnLine(compare.distance[i > 150 ? i - 150 : 0],
                        &compare.gpsLat[i], &compare.gpsLon[i]);
        const auto gps = computeComparisonAlignment(
            primary, compare, options(ComparisonAlignmentStrategy::Gps));
        QCOMPARE(gps.basis, QStringLiteral("Lap time %"));
        QCOMPARE(gps.gpsAnchors, 0);
        QVERIFY(gps.gpsRejected > 0);
        QVERIFY(approx(gps.fraction[700], 700.0 / 1499.0));
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
        std::fill(primary.speed.begin(), primary.speed.end(), kSpeed * 3.6);
        auto compare = primary;
        for (int i = 0; i < kSamples; ++i) {
            const double t = double(i) / kRate;
            position(t, &primary.gpsLat[size_t(i)], &primary.gpsLon[size_t(i)]);
            position(t - kLag, &compare.gpsLat[size_t(i)],
                     &compare.gpsLon[size_t(i)]);
        }

        const auto result = computeComparisonAlignment(
            primary, compare, options(ComparisonAlignmentStrategy::Gps));
        QCOMPARE(result.basis, QStringLiteral("GPS · continuous"));
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
            primary, compare, options(ComparisonAlignmentStrategy::Gps));
        const auto dampers = computeComparisonAlignment(
            primary, compare,
            options(ComparisonAlignmentStrategy::PreCornerDampers, {0.5}));
        QCOMPARE(gps.basis, QStringLiteral("Lap time %"));
        QCOMPARE(dampers.basis, QStringLiteral("Lap time %"));
        QCOMPARE(gps.gpsAnchors, 0);
    }
    void relativePositionIsSignedAlongTravel() {
        // Due north at 1 m per sample (50 m/s), sub-metre GPS.
        const auto straight = [](double accuracy) {
            omatrack::UnifiedLap lap;
            lap.sampleRate = 50;
            for (int i = 0; i < 500; ++i) {
                lap.time.push_back(i / 50.0);
                lap.gpsLat.push_back(43.0 + i / 111320.0);
                lap.gpsLon.push_back(-88.0);
                lap.gpsPositionAccuracy.push_back(accuracy);
            }
            return lap;
        };
        const auto primary = straight(0.4);
        const auto compare = straight(0.6);
        const double at = 200.0 / 499.0;
        const auto ahead = omatrack::alignment::relativeAlongTrackMeters(
            primary, at, compare, 205.0 / 499.0);
        QVERIFY(ahead && approx(*ahead, 5.0, 0.05));
        const auto behind = omatrack::alignment::relativeAlongTrackMeters(
            primary, at, compare, 197.5 / 499.0);
        QVERIFY(behind && approx(*behind, -2.5, 0.05));
        const auto same = omatrack::alignment::relativeAlongTrackMeters(
            primary, at, compare, at);
        QVERIFY(same && approx(*same, 0.0, 1e-6));
    }
    void relativePositionNeedsSubMetreGps() {
        omatrack::UnifiedLap primary;
        primary.sampleRate = 50;
        for (int i = 0; i < 100; ++i) {
            primary.time.push_back(i / 50.0);
            primary.gpsLat.push_back(43.0 + i / 111320.0);
            primary.gpsLon.push_back(-88.0);
            primary.gpsPositionAccuracy.push_back(0.5);
        }
        auto coarse = primary;
        std::fill(coarse.gpsPositionAccuracy.begin(),
                  coarse.gpsPositionAccuracy.end(), 1.0);
        QVERIFY(!omatrack::alignment::relativeAlongTrackMeters(primary, 0.5,
                                                               coarse, 0.5));
        auto parked = primary;
        std::fill(parked.gpsLat.begin(), parked.gpsLat.end(), 43.0);
        QVERIFY(!omatrack::alignment::relativeAlongTrackMeters(parked, 0.5,
                                                               primary, 0.5));
        QVERIFY(!omatrack::alignment::relativeAlongTrackMeters(primary, 0.5,
                                                               primary, 1.5));
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

    void deadDamperChannelsAreNotDampers() {
        // A logger without the sensors still maps a constant channel.
        auto primary = makeLap(300, false, true);
        auto dead = primary;
        std::fill(dead.damperFL.begin(), dead.damperFL.end(), 0.0);
        std::fill(dead.damperFR.begin(), dead.damperFR.end(), 0.0);
        QVERIFY(comparisonDamperAlignmentAvailable(primary, primary));
        QVERIFY(!comparisonDamperAlignmentAvailable(primary, dead));
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
                     QStringLiteral("GPS · continuous"), 20),
                 QStringLiteral("HIGH"));
        QCOMPARE(comparisonAlignmentConfidenceLabel(
                     QStringLiteral("GPS · re-sync"), 3),
                 QStringLiteral("MED"));
        QCOMPARE(comparisonAlignmentConfidenceLabel(
                     QStringLiteral("Dampers · pre-corner"), 0),
                 QStringLiteral("MED"));
        QCOMPARE(comparisonAlignmentConfidenceLabel(
                     QStringLiteral("Lap distance %"), 0),
                 QStringLiteral("MED"));
        QCOMPARE(comparisonAlignmentConfidenceLabel(
                     QStringLiteral("Lap time %"), 0),
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
