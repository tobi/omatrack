// Focused regression coverage for the pure comparison-alignment helper used by
// TelemetryStore's primary/reference lap alignment (computeComparisonAlignment
// and comparisonAlignmentConfidenceLabel).
//
// Synthetic omatrack::UnifiedLap inputs exercise every alignment path:
//   - speed-landmark DTW alignment
//   - distance and index progress fallbacks (absent / misaligned / too-short
//     speed data)
//   - GPS anchor refinement (valid, distributed fixes)
//   - insufficient / invalid GPS fallback to the underlying basis
//   - output size, range, and monotonicity invariants
//   - basis strings, GPS anchor counts, and confidence labels
//
// TelemetryStore remains the QML-facing cache owner; these tests drive the
// calculation directly so no QML/GUI or session parsing is required.

#include "app/ComparisonAlignment.h"
#include "core/TelemetryEngine.h"

#include <QtTest>

#include <cmath>
#include <limits>

namespace {
constexpr double kPi = 3.14159265358979323846;

// Deterministic synthetic lap builder. Only the fields the alignment helper
// reads are populated (time, speed, distance, distanceSource, sampleRate,
// gpsLat/gpsLon/gpsPositionAccuracy); everything else stays default-empty.
omatrack::UnifiedLap makeLap(int samples, int sampleRate, bool withSpeed,
                             bool withDistance, bool nativeDistance,
                             bool withGps) {
    omatrack::UnifiedLap lap;
    lap.sampleRate = sampleRate;
    lap.distanceSource = nativeDistance ? omatrack::DistanceSource::Native
                                         : omatrack::DistanceSource::SpeedFused;
    const double dt = 1.0 / double(sampleRate);
    lap.time.reserve(samples);
    if (withSpeed) lap.speed.reserve(samples);
    if (withDistance) lap.distance.reserve(samples);
    if (withGps) {
        lap.gpsLat.reserve(samples);
        lap.gpsLon.reserve(samples);
        lap.gpsPositionAccuracy.reserve(samples);
    }
    for (int i = 0; i < samples; ++i) {
        lap.time.push_back(i * dt);
        if (withSpeed)
            lap.speed.push_back(120.0 + 80.0 * std::sin(2.0 * kPi * i / samples));
        if (withDistance)
            lap.distance.push_back(samples > 1
                                       ? double(i) * 1000.0 / double(samples - 1)
                                       : 0.0);
        if (withGps) {
            // ~11 m per sample north/east: distinct enough that the nearest
            // compare fix to primary fix i is compare fix i (distance 0), yet
            // spread across the lap so anchors distribute over all 8 bins.
            lap.gpsLat.push_back(40.0 + 0.0001 * i);
            lap.gpsLon.push_back(-80.0 + 0.0001 * i);
            lap.gpsPositionAccuracy.push_back(1.0);
        }
    }
    return lap;
}

bool approx(double a, double b, double eps = 1e-6) {
    return std::abs(a - b) <= eps;
}

bool monotonicNonDecreasing(const QVector<double>& v) {
    for (int i = 1; i < v.size(); ++i)
        if (v[i] < v[i - 1] - 1e-9) return false;
    return true;
}

bool withinRange(const QVector<double>& v, double lo, double hi) {
    for (double x : v)
        if (x < lo - 1e-9 || x > hi + 1e-9) return false;
    return true;
}
}  // namespace

// ────────────────────────────────────────────────────────────────────
// Speed-landmark alignment
// ────────────────────────────────────────────────────────────────────

class SpeedLandmarkAlignmentTest : public QObject {
    Q_OBJECT
private slots:
    void identicalProfilesAlignBySpeed() {
        const int N = 100;
        auto primary = makeLap(N, 50, true, false, false, false);
        auto compare = primary;  // identical speed → DTW follows the diagonal
        const auto result = computeComparisonAlignment(primary, compare);

        QCOMPARE(result.basis, QStringLiteral("speed landmarks"));
        QCOMPARE(result.gpsAnchors, 0);
        QVERIFY(result.time.size() == qsizetype(N));
        QVERIFY(result.fraction.size() == qsizetype(N));
        // Identity alignment maps each primary sample to the same compare time.
        QVERIFY(approx(result.time.first(), compare.time.front()));
        QVERIFY(approx(result.time[50], compare.time[50]));
        QVERIFY(approx(result.time.last(), compare.time.back()));
        QVERIFY(monotonicNonDecreasing(result.time));
        QVERIFY(withinRange(result.time, compare.time.front(), compare.time.back()));
        QVERIFY(withinRange(result.fraction, 0.0, 1.0));
        QVERIFY(monotonicNonDecreasing(result.fraction));
        QCOMPARE(comparisonAlignmentConfidenceLabel(result.basis, result.gpsAnchors),
                 QStringLiteral("MED"));
    }

    void warpedProfileProducesContinuousMillisecondDelta() {
        constexpr int N = 500;
        auto primary = makeLap(N, 50, true, false, false, false);
        auto compare = primary;
        auto profile = [](double progress) {
            return 145.0 + 48.0 * std::sin(6.0 * kPi * progress) +
                   17.0 * std::sin(14.0 * kPi * progress + 0.4);
        };
        for (int i = 0; i < N; ++i) {
            const double progress = double(i) / double(N - 1);
            primary.speed[i] = profile(progress);
            compare.speed[i] =
                profile(std::clamp(progress +
                                       0.025 * std::sin(kPi * progress),
                                   0.0, 1.0));
        }

        const auto result = computeComparisonAlignment(primary, compare);
        QCOMPARE(result.basis, QStringLiteral("speed landmarks"));
        QCOMPARE(result.time.size(), qsizetype(N));

        // The bounded DTW grid must not appear in the user-facing delta as
        // 0.1 s steps. At 50 Hz, a continuous warp changes by milliseconds
        // between samples rather than catching up by a whole sample at once.
        double largestDeltaStep = 0.0;
        for (int i = 1; i < N; ++i) {
            const double previousDelta =
                primary.time[size_t(i - 1)] - result.time[i - 1];
            const double delta = primary.time[size_t(i)] - result.time[i];
            largestDeltaStep =
                std::max(largestDeltaStep, std::abs(delta - previousDelta));
        }
        QVERIFY2(largestDeltaStep < 0.01,
                 qPrintable(QStringLiteral("largest delta step was %1 s")
                                .arg(largestDeltaStep, 0, 'f', 6)));
    }
};

// ────────────────────────────────────────────────────────────────────
// Progress fallback (absent / misaligned / too-short speed)
// ────────────────────────────────────────────────────────────────────

class ProgressFallbackTest : public QObject {
    Q_OBJECT
private slots:
    void absentSpeedWithNativeDistanceUsesValidatedDistance() {
        const int N = 100;
        auto primary = makeLap(N, 50, false, true, true, false);
        auto compare = primary;
        const auto result = computeComparisonAlignment(primary, compare);

        QCOMPARE(result.basis, QStringLiteral("validated lap distance"));
        QCOMPARE(result.gpsAnchors, 0);
        QVERIFY(result.time.size() == qsizetype(N));
        QVERIFY(approx(result.time.first(), compare.time.front()));
        QVERIFY(approx(result.time.last(), compare.time.back()));
        QVERIFY(monotonicNonDecreasing(result.time));
        QCOMPARE(comparisonAlignmentConfidenceLabel(result.basis, result.gpsAnchors),
                 QStringLiteral("MED"));
    }

    void absentSpeedWithoutDistanceUsesWheelGps() {
        const int N = 100;
        auto primary = makeLap(N, 50, false, false, false, false);
        auto compare = primary;
        const auto result = computeComparisonAlignment(primary, compare);

        QCOMPARE(result.basis, QStringLiteral("wheel/GPS speed"));
        QCOMPARE(result.gpsAnchors, 0);
        QVERIFY(result.time.size() == qsizetype(N));
        QVERIFY(monotonicNonDecreasing(result.time));
        QCOMPARE(comparisonAlignmentConfidenceLabel(result.basis, result.gpsAnchors),
                 QStringLiteral("LOW"));
    }

    void misalignedSpeedFallsBackToProgress() {
        const int N = 100;
        auto primary = makeLap(N, 50, true, false, false, false);
        auto compare = makeLap(N, 50, true, false, false, false);
        compare.speed.resize(50);  // 50 != 100 time samples → speed mismatch
        const auto result = computeComparisonAlignment(primary, compare);

        QCOMPARE(result.basis, QStringLiteral("wheel/GPS speed"));
        QCOMPARE(result.gpsAnchors, 0);
        QVERIFY(result.time.size() == qsizetype(N));
        QVERIFY(monotonicNonDecreasing(result.time));
        QCOMPARE(comparisonAlignmentConfidenceLabel(result.basis, result.gpsAnchors),
                 QStringLiteral("LOW"));
    }

    void tooShortSpeedFallsBackToProgress() {
        const int N = 100;
        auto primary = makeLap(N, 50, true, false, false, false);
        primary.speed.resize(2);  // < 3 → not enough landmarks to warp
        auto compare = makeLap(N, 50, true, false, false, false);
        const auto result = computeComparisonAlignment(primary, compare);

        QCOMPARE(result.basis, QStringLiteral("wheel/GPS speed"));
        QCOMPARE(result.gpsAnchors, 0);
        QVERIFY(result.time.size() == qsizetype(N));
        QVERIFY(monotonicNonDecreasing(result.time));
        QCOMPARE(comparisonAlignmentConfidenceLabel(result.basis, result.gpsAnchors),
                 QStringLiteral("LOW"));
    }
};

// ────────────────────────────────────────────────────────────────────
// GPS affine alignment
// ────────────────────────────────────────────────────────────────────

class GpsAffineAlignmentTest : public QObject {
    Q_OBJECT
private slots:
    void distributedGpsRefinesSpeedLandmarks() {
        const int N = 300;
        constexpr int kGpsShiftSamples = 10;
        auto primary = makeLap(N, 50, true, false, false, true);
        auto compare = primary;
        for (int i = 0; i < N; ++i) {
            // The same position occurs later on the compare lap. GPS anchors
            // must therefore apply a measurable non-zero time correction
            // rather than merely relabeling the speed-landmark result.
            const double shiftedIndex = double(i - kGpsShiftSamples);
            compare.gpsLat[i] = 40.0 + 0.0001 * shiftedIndex;
            compare.gpsLon[i] = -80.0 + 0.0001 * shiftedIndex;
        }
        const auto result = computeComparisonAlignment(primary, compare);

        QCOMPARE(result.basis, QStringLiteral("GPS anchored · speed landmarks"));
        QVERIFY(result.gpsAnchors >= 8);
        QVERIFY(result.time.size() == qsizetype(N));
        QVERIFY(result.time[N / 2] > compare.time[N / 2] + 0.1);
        QVERIFY(monotonicNonDecreasing(result.time));
        QVERIFY(withinRange(result.time, compare.time.front(), compare.time.back()));
        QVERIFY(withinRange(result.fraction, 0.0, 1.0));
        QVERIFY(monotonicNonDecreasing(result.fraction));
        QCOMPARE(comparisonAlignmentConfidenceLabel(result.basis, result.gpsAnchors),
                 QStringLiteral("HIGH"));
    }

    void distributedGpsRefinesFallbackPath() {
        const int N = 300;
        auto primary = makeLap(N, 50, false, false, false, true);  // no speed
        auto compare = primary;
        const auto result = computeComparisonAlignment(primary, compare);

        QCOMPARE(result.basis, QStringLiteral("GPS anchored · wheel/GPS speed"));
        QVERIFY(result.gpsAnchors >= 8);
        QVERIFY(result.time.size() == qsizetype(N));
        QVERIFY(monotonicNonDecreasing(result.time));
        QCOMPARE(comparisonAlignmentConfidenceLabel(result.basis, result.gpsAnchors),
                 QStringLiteral("HIGH"));
    }
};

// ────────────────────────────────────────────────────────────────────
// Insufficient / invalid GPS fallback
// ────────────────────────────────────────────────────────────────────

class GpsFallbackTest : public QObject {
    Q_OBJECT
private slots:
    void invalidGpsAccuracyFallsBackToSpeedLandmarks() {
        const int N = 300;
        auto primary = makeLap(N, 50, true, false, false, true);
        auto compare = primary;
        // GPS present but every fix fails the accuracy > 0 gate.
        for (double& acc : primary.gpsPositionAccuracy) acc = 0.0;
        for (double& acc : compare.gpsPositionAccuracy) acc = 0.0;
        const auto result = computeComparisonAlignment(primary, compare);

        QCOMPARE(result.basis, QStringLiteral("speed landmarks"));
        QCOMPARE(result.gpsAnchors, 0);
        QVERIFY(result.time.size() == qsizetype(N));
        QCOMPARE(comparisonAlignmentConfidenceLabel(result.basis, result.gpsAnchors),
                 QStringLiteral("MED"));
    }

    void clusteredGpsFallsBackToSpeedLandmarks() {
        const int N = 300;
        auto primary = makeLap(N, 50, true, false, false, true);
        auto compare = primary;
        // Only the first 25 samples carry valid GPS; with anchorStep 25 only a
        // single anchor survives, far below the >= 8 distributed threshold.
        const int clusterEnd = 24;
        const double nan = std::numeric_limits<double>::quiet_NaN();
        for (int i = 0; i < N; ++i) {
            if (i > clusterEnd) {
                primary.gpsLat[i] = nan;
                primary.gpsLon[i] = nan;
                primary.gpsPositionAccuracy[i] = nan;
                compare.gpsLat[i] = nan;
                compare.gpsLon[i] = nan;
                compare.gpsPositionAccuracy[i] = nan;
            }
        }
        const auto result = computeComparisonAlignment(primary, compare);

        QCOMPARE(result.basis, QStringLiteral("speed landmarks"));
        QCOMPARE(result.gpsAnchors, 0);
        QCOMPARE(comparisonAlignmentConfidenceLabel(result.basis, result.gpsAnchors),
                 QStringLiteral("MED"));
    }
};

// ────────────────────────────────────────────────────────────────────
// Output size / range / monotonicity invariants
// ────────────────────────────────────────────────────────────────────

class AlignmentInvariantTest : public QObject {
    Q_OBJECT
private slots:
    void timeAndFractionRespectInvariants() {
        // N=200 lands exactly 8 GPS anchors (anchorStep 25): a boundary case
        // for the gpsDistributed >= 8 threshold.
        const int N = 200;
        auto primary = makeLap(N, 50, true, false, false, true);
        auto compare = primary;
        const auto result = computeComparisonAlignment(primary, compare);

        QVERIFY(result.time.size() == qsizetype(N));
        QVERIFY(result.fraction.size() == result.time.size());
        QVERIFY(result.gpsAnchors >= 8);
        QVERIFY(result.basis.contains(QStringLiteral("GPS anchored")));
        // Time is bounded by the compare lap and non-decreasing.
        QVERIFY(withinRange(result.time, compare.time.front(), compare.time.back()));
        QVERIFY(monotonicNonDecreasing(result.time));
        // Fraction is a 0-1 non-decreasing remap of time.
        QVERIFY(withinRange(result.fraction, 0.0, 1.0));
        QVERIFY(monotonicNonDecreasing(result.fraction));
        QVERIFY(result.fraction.first() >= 0.0);
        QVERIFY(result.fraction.last() <= 1.0);
    }
};

// ────────────────────────────────────────────────────────────────────
// Confidence label classification
// ────────────────────────────────────────────────────────────────────

class AlignmentConfidenceLabelTest : public QObject {
    Q_OBJECT
private slots:
    void emptyBasisIsNone() {
        QCOMPARE(comparisonAlignmentConfidenceLabel(QString(), 0),
                 QStringLiteral("NONE"));
    }
    void gpsAnchoredWithEnoughAnchorsIsHigh() {
        QCOMPARE(comparisonAlignmentConfidenceLabel(
                     QStringLiteral("GPS anchored · speed landmarks"), 8),
                 QStringLiteral("HIGH"));
        QCOMPARE(comparisonAlignmentConfidenceLabel(
                     QStringLiteral("GPS anchored · wheel/GPS speed"), 12),
                 QStringLiteral("HIGH"));
    }
    void speedLandmarksIsMedium() {
        QCOMPARE(comparisonAlignmentConfidenceLabel(
                     QStringLiteral("speed landmarks"), 0),
                 QStringLiteral("MED"));
    }
    void validatedLapDistanceIsMedium() {
        QCOMPARE(comparisonAlignmentConfidenceLabel(
                     QStringLiteral("validated lap distance"), 0),
                 QStringLiteral("MED"));
    }
    void wheelGpsSpeedIsLow() {
        QCOMPARE(comparisonAlignmentConfidenceLabel(
                     QStringLiteral("wheel/GPS speed"), 0),
                 QStringLiteral("LOW"));
    }
    void anchorsTakePrecedenceOverBasis() {
        // >= 8 anchors is HIGH even when the basis would otherwise classify MED.
        QCOMPARE(comparisonAlignmentConfidenceLabel(
                     QStringLiteral("speed landmarks"), 8),
                 QStringLiteral("HIGH"));
    }
};

// ────────────────────────────────────────────────────────────────────
// Too-short laps produce empty alignment
// ────────────────────────────────────────────────────────────────────

class TinyLapTest : public QObject {
    Q_OBJECT
private slots:
    void primaryTooShortYieldsEmpty() {
        auto primary = makeLap(1, 50, true, false, false, false);
        auto compare = makeLap(100, 50, true, false, false, false);
        const auto result = computeComparisonAlignment(primary, compare);

        QVERIFY(result.time.isEmpty());
        QVERIFY(result.fraction.isEmpty());
        QVERIFY(result.basis.isEmpty());
        QCOMPARE(result.gpsAnchors, 0);
        QCOMPARE(comparisonAlignmentConfidenceLabel(result.basis, result.gpsAnchors),
                 QStringLiteral("NONE"));
    }

    void compareTooShortYieldsEmpty() {
        auto primary = makeLap(100, 50, true, false, false, false);
        auto compare = makeLap(1, 50, true, false, false, false);
        const auto result = computeComparisonAlignment(primary, compare);

        QVERIFY(result.time.isEmpty());
        QVERIFY(result.fraction.isEmpty());
        QVERIFY(result.basis.isEmpty());
        QCOMPARE(result.gpsAnchors, 0);
    }
};

// Run all test classes in one executable. QTEST_APPLESS_MAIN only runs one
// class; a custom main ensures every Q_OBJECT class is executed.
int main(int argc, char* argv[]) {
    int status = 0;
    { SpeedLandmarkAlignmentTest t; status |= QTest::qExec(&t, argc, argv); }
    { ProgressFallbackTest t; status |= QTest::qExec(&t, argc, argv); }
    { GpsAffineAlignmentTest t; status |= QTest::qExec(&t, argc, argv); }
    { GpsFallbackTest t; status |= QTest::qExec(&t, argc, argv); }
    { AlignmentInvariantTest t; status |= QTest::qExec(&t, argc, argv); }
    { AlignmentConfidenceLabelTest t; status |= QTest::qExec(&t, argc, argv); }
    { TinyLapTest t; status |= QTest::qExec(&t, argc, argv); }
    return status;
}

#include "AlignmentTest.moc"
