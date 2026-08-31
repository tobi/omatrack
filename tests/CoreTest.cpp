// Unit tests for the omatrack Qt-free analysis core.
//
// Covers the public free functions (normalizeChannelName, formatLapTime,
// sessionMetaFromFilename, resample), the detail-namespace lap-detection and
// channel-matching helpers, and TelemetrySource methods with synthetic data.

#include "core/TelemetryEngine.h"
#include "core/MonotonicSeries.h"
#include "core/TelemetryEngineInternal.h"

#include <QtTest>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <cmath>
#include <limits>
#include <numeric>

using namespace omatrack;
using namespace omatrack::detail;

// ────────────────────────────────────────────────────────────────────
// normalizeChannelName
// ────────────────────────────────────────────────────────────────────

class NormalizeChannelNameTest : public QObject {
    Q_OBJECT
private slots:
    void lowercasesLetters() {
        QCOMPARE(normalizeChannelName("GroundSpeed"), "groundspeed");
    }
    void stripsNonAlphanumeric() {
        QCOMPARE(normalizeChannelName("Brake Pressure F!"), "brakepressuref");
    }
    void handlesSpacesAndUnderscores() {
        QCOMPARE(normalizeChannelName("lap_distance corrected"),
                 "lapdistancecorrected");
    }
    void emptyInput() { QVERIFY(normalizeChannelName("").empty()); }
    void purePunctuation() { QVERIFY(normalizeChannelName("___- -!").empty()); }
    void preservesDigits() {
        QCOMPARE(normalizeChannelName("Gear 2 Pos"), "gear2pos");
    }
    void mixedCaseWithSymbols() {
        QCOMPARE(normalizeChannelName("GPS_Lat-N"), "gpslatn");
    }
};

// ────────────────────────────────────────────────────────────────────
// formatLapTime
// ────────────────────────────────────────────────────────────────────

class FormatLapTimeTest : public QObject {
    Q_OBJECT
private slots:
    void subMinute() { QCOMPARE(formatLapTime(83550), QString("1:23.550")); }
    void exactlyOneMinute() {
        QCOMPARE(formatLapTime(60000), QString("1:00.000"));
    }
    void subMinuteOnly() {
        QCOMPARE(formatLapTime(23450), QString("0:23.450"));
    }
    void zero() { QCOMPARE(formatLapTime(0), QString("0:00.000")); }
    void multiMinute() { QCOMPARE(formatLapTime(183550), QString("3:03.550")); }
    void roundsToMilliseconds() {
        // 1:23.551 — printf %.3f rounds 23.5510 to 3 decimals
        QCOMPARE(formatLapTime(83551), QString("1:23.551"));
    }
    void tenMinutes() { QCOMPARE(formatLapTime(600000), QString("10:00.000")); }
    void justUnderAMinute() {
        QCOMPARE(formatLapTime(59999), QString("0:59.999"));
    }
};

// ────────────────────────────────────────────────────────────────────
// sessionMetaFromFilename
// ────────────────────────────────────────────────────────────────────

class SessionMetaFromFilenameTest : public QObject {
    Q_OBJECT
private slots:
    void parsesTwelveDigitTimestamp() {
        auto m = sessionMetaFromFilename("260805143022_MQ12Di_LMP2");
        QCOMPARE(m.date, "05/08/2026");
        QCOMPARE(m.time, "14:30:22");
        QCOMPARE(m.eventName, "260805143022_MQ12Di_LMP2");
    }
    void nonTimestampStemHasNoDate() {
        auto m = sessionMetaFromFilename("practice_session");
        QVERIFY(m.date.empty());
        QVERIFY(m.time.empty());
        QCOMPARE(m.eventName, "practice_session");
    }
    void shortStemHasNoDate() {
        auto m = sessionMetaFromFilename("260805");
        QVERIFY(m.date.empty());
    }
    void eventNameIsFullStem() {
        auto m = sessionMetaFromFilename("260101120000_Race");
        QCOMPARE(m.eventName, "260101120000_Race");
    }
    void emptyStemHasNoDate() {
        auto m = sessionMetaFromFilename("");
        QVERIFY(m.date.empty());
        QVERIFY(m.eventName.empty());
    }
    void mixedPrefixDoesNotCountAsTimestamp() {
        auto m = sessionMetaFromFilename("Q260805143022");
        QVERIFY(m.date.empty());
    }
};

// ────────────────────────────────────────────────────────────────────
// resample
// ────────────────────────────────────────────────────────────────────

class ResampleTest : public QObject {
    Q_OBJECT
private slots:
    void emptyInput() { QVERIFY(resample({}, 100, 50, 1.0).empty()); }
    void sameFreqIsIdentity() {
        std::vector<double> v{1, 2, 3, 4, 5};
        auto out = resample(v, 100, 100, 0.04);
        QCOMPARE(out.size(), size_t(5));
        for (size_t i = 0; i < v.size(); ++i) QCOMPARE(out[i], v[i]);
    }
    void linearUpsampling() {
        // 2 samples at 10 Hz over 0.1 s: values [0, 10]
        // Upsample to 20 Hz → 3 samples at t=0, 0.05, 0.1
        std::vector<double> v{0.0, 10.0};
        auto out = resample(v, 10, 20, 0.1);
        QCOMPARE(out.size(), size_t(3));
        QCOMPARE(out[0], 0.0);
        QCOMPARE(out[1], 5.0);  // midpoint
        QCOMPARE(out[2], 10.0);
    }
    void linearDownsampling() {
        // 11 samples at 100 Hz over 0.1 s: 0..10
        // Downsample to 50 Hz → 6 samples
        std::vector<double> v(11);
        std::iota(v.begin(), v.end(), 0.0);
        auto out = resample(v, 100, 50, 0.1);
        QCOMPARE(out.size(), size_t(6));
        QCOMPARE(out[0], 0.0);
        QCOMPARE(out[1], 2.0);
        QCOMPARE(out[5], 10.0);
    }
    void singleSampleRepeats() {
        std::vector<double> v{42.0};
        auto out = resample(v, 100, 50, 0.1);
        QCOMPARE(out.size(), size_t(6));
        for (double s : out) QCOMPARE(s, 42.0);
    }
    void rejectsInvalidDomains() {
        const std::vector<double> values{1.0, 2.0};
        QVERIFY(resample(values, 0.0, 50.0, 1.0).empty());
        QVERIFY(resample(values, 50.0, 0.0, 1.0).empty());
        QVERIFY(resample(values, 50.0, 50.0, 0.0).empty());
    }
};

// ────────────────────────────────────────────────────────────────────
// pdsBeaconSplits — rising-edge detection from a beacon channel
// ────────────────────────────────────────────────────────────────────

class BeaconSplitsTest : public QObject {
    Q_OBJECT
private slots:
    void detectsRisingEdges() {
        // 10 Hz, 5 s: pulse at t=1 and t=3
        std::vector<double> v(50, 0.0);
        for (int i = 10; i < 13; ++i) v[i] = 1;  // pulse at ~1 s
        for (int i = 30; i < 33; ++i) v[i] = 1;  // pulse at ~3 s
        auto splits = pdsBeaconSplits(v, 10);
        QCOMPARE(splits.size(), size_t(2));
        QCOMPARE(splits[0], 1.0);
        QCOMPARE(splits[1], 3.0);
    }
    void emptyInput() { QVERIFY(pdsBeaconSplits({}, 10).empty()); }
    void zeroFreq() {
        std::vector<double> v{1, 0, 1};
        QVERIFY(pdsBeaconSplits(v, 0).empty());
    }
    void allZero() {
        std::vector<double> v(100, 0.0);
        QVERIFY(pdsBeaconSplits(v, 10).empty());
    }
    void allOneIsSingleRisingEdge() {
        std::vector<double> v(100, 1.0);
        auto splits = pdsBeaconSplits(v, 10);
        QCOMPARE(splits.size(), size_t(1));
        QCOMPARE(splits[0], 0.0);
    }
};

// ────────────────────────────────────────────────────────────────────
// pdsLapTimeSplits — backward jumps in cumulative lap time
// ────────────────────────────────────────────────────────────────────

class LapTimeSplitsTest : public QObject {
    Q_OBJECT
private slots:
    void detectsBackwardJumps() {
        // 10 Hz, cumulative lap time in seconds.
        // Lap 1: 0→90 s, then reset to 0 for lap 2.
        std::vector<double> v;
        for (int i = 0; i < 900; ++i) v.push_back(double(i) / 10.0);  // 0..89.9
        for (int i = 0; i < 900; ++i) v.push_back(double(i) / 10.0);  // reset
        auto splits = pdsLapTimeSplits(v, 10);
        QVERIFY(splits.size() >= 1);
        QCOMPARE(splits[0], 90.0);
    }
    void noJumps() {
        std::vector<double> v;
        for (int i = 0; i < 1000; ++i) v.push_back(double(i) / 10.0);
        QVERIFY(pdsLapTimeSplits(v, 10).empty());
    }
    void tooShort() {
        std::vector<double> v{5.0};
        QVERIFY(pdsLapTimeSplits(v, 10).empty());
    }
    void enforcesClusterGap() {
        // Backward jump detection fires at the DROP, not the rise:
        // values[i-1] - values[i] > 5. So v[5]=100, v[6]=0 creates a split
        // at index 6 (t=0.6), not index 5. The second drop at index 9 is
        // within cluster gap = max(1, 10/2) = 5 samples, so it's skipped.
        std::vector<double> v(20, 0.0);
        v[5] = 100;
        v[6] = 0;  // drop → split at 0.6
        v[8] = 100;
        v[9] = 0;  // drop → 9-6=3 < 5, skipped
        auto splits = pdsLapTimeSplits(v, 10);
        QCOMPARE(splits.size(), size_t(1));
        QCOMPARE(splits[0], 0.6);
    }
};

// ────────────────────────────────────────────────────────────────────
// pdsLastLapTimeSplits — a posted last-lap time changing to a new value
// ────────────────────────────────────────────────────────────────────

class LastLapTimeSplitsTest : public QObject {
    Q_OBJECT
private slots:
    void detectsPostedTimeChange() {
        // 5 Hz, Previous_LT in milliseconds. Stays at 1:16.852, then jumps
        // to 1:17.034 at the next crossing.
        std::vector<double> v(50, 76852.0);
        v.insert(v.end(), 50, 77034.0);
        auto splits = pdsLastLapTimeSplits(v, 5);
        QCOMPARE(splits.size(), size_t(1));
        QCOMPARE(splits[0], 10.0);
    }
    void ignoresZeros() {
        std::vector<double> v(40, 0.0);
        QVERIFY(pdsLastLapTimeSplits(v, 5).empty());
    }
    void tooShort() {
        std::vector<double> v{76852.0};
        QVERIFY(pdsLastLapTimeSplits(v, 5).empty());
    }
};

// ────────────────────────────────────────────────────────────────────
// pdsLapNumberSplits — increments in lap number
// ────────────────────────────────────────────────────────────────────

class LapNumberSplitsTest : public QObject {
    Q_OBJECT
private slots:
    void detectsIncrements() {
        // 10 Hz, lap number: 1 for 10 s, 2 for 10 s, 3 for 10 s
        std::vector<double> v;
        for (int lap = 1; lap <= 3; ++lap)
            for (int i = 0; i < 100; ++i) v.push_back(double(lap));
        auto splits = pdsLapNumberSplits(v, 10);
        QCOMPARE(splits.size(), size_t(2));
        QCOMPARE(splits[0], 10.0);
        QCOMPARE(splits[1], 20.0);
    }
    void noIncrements() {
        std::vector<double> v(200, 1.0);
        QVERIFY(pdsLapNumberSplits(v, 10).empty());
    }
    void tooShort() {
        std::vector<double> v{1.0};
        QVERIFY(pdsLapNumberSplits(v, 10).empty());
    }
    void rejectsSkippedCounterValues() {
        const std::vector<double> values{13.0, 0.0, 14.0, 14.0, 15.0};
        const std::vector<double> splits = pdsLapNumberSplits(values, 10);
        QCOMPARE(splits.size(), size_t(1));
        QCOMPARE(splits.front(), 0.4);
    }
    void ignoresCounterRestartFromZero() {
        const std::vector<double> values{13.0, 0.0, 1.0, 1.0};
        QVERIFY(pdsLapNumberSplits(values, 10).empty());
    }
    void activeCounterBlocksTimerFallback() {
        const std::vector<double> counterValues{13.0, 13.0, 0.0, 1.0};
        QVERIFY(lapNumberCarriesState(counterValues));

        const std::vector<double> selected =
            selectLapSplits({}, {105.2}, true, {2.3, 13.5, 25.4}, {});
        QCOMPARE(selected.size(), size_t(3));
        QCOMPARE(selected.front(), 2.3);
        QCOMPARE(selected.back(), 25.4);
    }
    void zeroCounterAllowsTimerFallback() {
        const std::vector<double> counterValues(20, 0.0);
        QVERIFY(!lapNumberCarriesState(counterValues));

        const std::vector<double> selected =
            selectLapSplits({}, {}, false, {90.0, 180.0}, {});
        QCOMPARE(selected.size(), size_t(2));
        QCOMPARE(selected.front(), 90.0);
        QCOMPARE(selected.back(), 180.0);
    }
    void counterCrossingsBeatTimerResets() {
        const std::vector<double> selected = selectLapSplits(
            {}, {100.0, 200.0}, true, {12.0, 27.0, 43.0}, {101.0, 201.0});
        QCOMPARE(selected.size(), size_t(2));
        QCOMPARE(selected.front(), 100.0);
        QCOMPARE(selected.back(), 200.0);
    }
    void activeCounterBeatsBeaconCrossings() {
        const std::vector<double> selected =
            selectLapSplits({90.0, 180.0}, {100.0, 200.0}, true, {}, {});
        QCOMPARE(selected.size(), size_t(2));
        QCOMPARE(selected.front(), 100.0);
        QCOMPARE(selected.back(), 200.0);
    }
    void activeCounterWithoutCrossingsFallsThroughToBeacon() {
        const std::vector<double> selected =
            selectLapSplits({90.0, 180.0}, {}, true, {12.0}, {});
        QCOMPARE(selected.size(), size_t(2));
        QCOMPARE(selected.front(), 90.0);
        QCOMPARE(selected.back(), 180.0);
    }
};

// ────────────────────────────────────────────────────────────────────
// pdsDistanceSplits — resets in cumulative lap distance
// ────────────────────────────────────────────────────────────────────

class DistanceSplitsTest : public QObject {
    Q_OBJECT
private slots:
    void detectsResets() {
        // 10 Hz, distance increasing 0..5000 m, then reset
        std::vector<double> v;
        for (int i = 0; i < 500; ++i) v.push_back(double(i) * 10);  // 0..4990
        for (int i = 0; i < 500; ++i) v.push_back(double(i) * 10);  // reset
        auto splits = pdsDistanceSplits(v, 10);
        QVERIFY(splits.size() >= 1);
        QCOMPARE(splits[0], 50.0);
    }
    void noResets() {
        std::vector<double> v;
        for (int i = 0; i < 500; ++i) v.push_back(double(i) * 10);
        QVERIFY(pdsDistanceSplits(v, 10).empty());
    }
    void kilometreScaleWrapIsAReset() {
        // IMS oval ~4 km reported in kilometres never clears a 300 m floor.
        std::vector<double> v;
        for (int i = 0; i < 400; ++i) v.push_back(double(i) * 0.01);
        for (int i = 0; i < 400; ++i) v.push_back(double(i) * 0.01);
        const std::vector<double> splits = pdsDistanceSplits(v, 10);
        QVERIFY(splits.size() >= 1);
        QCOMPARE(splits[0], 40.0);
    }
};

// ────────────────────────────────────────────────────────────────────
// buildLapsFromSplits — lap construction and classification
// ────────────────────────────────────────────────────────────────────

class BuildLapsFromSplitsTest : public QObject {
    Q_OBJECT
private slots:
    void emptySplitsYieldsOneFragment() {
        auto laps = buildLapsFromSplits({}, 120.0);
        QCOMPARE(laps.size(), size_t(1));
        QVERIFY(!laps[0].complete);
        QCOMPARE(laps[0].startTime, 0.0);
        QCOMPARE(laps[0].endTime, 120.0);
    }
    void singleSplitYieldsOneFragment() {
        auto laps = buildLapsFromSplits({60.0}, 120.0);
        QCOMPARE(laps.size(), size_t(1));
        QVERIFY(!laps[0].complete);
    }
    void twoSplitsYieldOneCompleteLap() {
        auto laps = buildLapsFromSplits({30.0, 120.0}, 150.0);
        QCOMPARE(laps.size(), size_t(1));
        QVERIFY(laps[0].complete);
        QCOMPARE(laps[0].startTime, 30.0);
        QCOMPARE(laps[0].endTime, 120.0);
        QCOMPARE(laps[0].timeMs, 90000.0);
    }
    void threeSplitsYieldTwoCompleteLaps() {
        auto laps = buildLapsFromSplits({30.0, 120.0, 210.0}, 240.0);
        // Two complete laps + a tail fragment (30 s head → fragment)
        QVERIFY(laps.size() >= 2);
        QVERIFY(laps[0].complete);
        QVERIFY(laps[1].complete);
    }
    void markShortCrossingsRejectsAuthoritativeOutLaps() {
        std::vector<Lap> laps{Lap{1, 0.0, 18.0, 18000.0, true},
                              Lap{2, 18.0, 118.0, 100000.0, true},
                              Lap{3, 118.0, 218.0, 100000.0, true}};
        markShortCrossingsIncomplete(laps);
        QVERIFY(!laps[0].complete);
        QVERIFY(laps[1].complete);
        QVERIFY(laps[2].complete);
    }

    void shortLapMarkedIncomplete() {
        // buildLapsFromSplits only creates laps where b - a > 10, so a 15 s
        // lap is the shortest that can be created. With a 90 s median, the
        // 15 s lap is below median*0.5 = 45 and must be marked incomplete.
        auto laps = buildLapsFromSplits({10.0, 100.0, 115.0}, 130.0);
        QVERIFY(laps.size() >= 2);
        bool foundShort = false;
        for (const auto& lap : laps) {
            if (lap.endTime - lap.startTime < 30.0) {
                QVERIFY(!lap.complete);
                foundShort = true;
            }
        }
        QVERIFY(foundShort);
    }
    void doubleTriggerCollapsesOntoFirstCrossing() {
        // A beacon seen twice at 120 s and 123 s is one crossing. The lap
        // after it must start at 120 s, not 123 s: no gap between laps.
        auto laps = buildLapsFromSplits({30.0, 120.0, 123.0, 210.0}, 240.0);
        std::vector<Lap> complete;
        for (const auto& lap : laps)
            if (lap.complete) complete.push_back(lap);
        QCOMPARE(complete.size(), size_t(2));
        QCOMPARE(complete[0].startTime, 30.0);
        QCOMPARE(complete[0].endTime, 120.0);
        QCOMPARE(complete[1].startTime, 120.0);
        QCOMPARE(complete[1].endTime, 210.0);
    }
    void headFragmentMarkedIncomplete() {
        // Split at 60 s in a 180 s recording → head is a fragment
        auto laps = buildLapsFromSplits({60.0, 150.0}, 180.0);
        QVERIFY(laps.size() >= 1);
        QVERIFY(!laps.front().complete);  // head fragment
    }
    void zeroDurationReturnsEmpty() {
        QVERIFY(buildLapsFromSplits({30.0}, 0.0).empty());
    }
    void splitsOutsideDurationAreFiltered() {
        auto laps = buildLapsFromSplits({-5.0, 60.0, 200.0}, 120.0);
        // -5 and 200 are outside (0, 120), leaving only {60} → fragment
        QCOMPARE(laps.size(), size_t(1));
        QVERIFY(!laps[0].complete);
    }
    void previousLapMillisecondsConfirmCrossing() {
        const std::vector<Lap> laps{Lap{0, 10.0, 128.0, 118000.0, true}};
        std::vector<double> previousTimes(131, 0.0);
        previousTimes[128] = 117831.0;

        const std::vector<Lap> confirmed =
            pdsApplyPreviousLapTimes(laps, previousTimes, 1);
        QVERIFY(confirmed.front().complete);
        QCOMPARE(confirmed.front().timeMs, 117831.0);
    }
    void fragmentNeverAdoptsPreviousLapTime() {
        // Trailing fragment 1507.5→1635 s (127.5 s). The logger still reports
        // the previous *complete* lap (116.172 s) at the recording end, and
        // 15 % tolerance would accept it; the fragment must keep its own
        // duration.
        const std::vector<Lap> laps{Lap{11, 1507.5, 1635.0, 127500.0, false}};
        std::vector<double> previousTimes(1636, 116172.0);
        const std::vector<Lap> out =
            pdsApplyPreviousLapTimes(laps, previousTimes, 1);
        QVERIFY(!out.front().complete);
        QCOMPARE(out.front().timeMs, 127500.0);
    }
    void mismatchedPreviousLapMarksFragmentIncomplete() {
        const std::vector<Lap> laps{Lap{0, 491.5, 510.3, 18800.0, true}};
        std::vector<double> previousTimes(513, 0.0);
        previousTimes[510] = 30500.0;

        const std::vector<Lap> confirmed =
            pdsApplyPreviousLapTimes(laps, previousTimes, 1);
        QVERIFY(!confirmed.front().complete);
        QCOMPARE(confirmed.front().timeMs, 18800.0);
    }
    void authoritativeCounterSurvivesMismatchedPreviousLap() {
        const std::vector<Lap> laps{Lap{0, 491.5, 510.3, 18800.0, true}};
        std::vector<double> previousTimes(513, 0.0);
        previousTimes[510] = 30500.0;

        const std::vector<Lap> confirmed =
            pdsApplyPreviousLapTimes(laps, previousTimes, 1, false);
        QVERIFY(confirmed.front().complete);
        QCOMPARE(confirmed.front().timeMs, 18800.0);
    }
    void shortPositionCoverageMarksFragmentIncomplete() {
        const std::vector<Lap> laps{Lap{0, 0.0, 99.0, 99000.0, true},
                                    Lap{1, 101.0, 200.0, 99000.0, true}};
        std::vector<double> position(201, 0.0);
        for (int i = 0; i <= 99; ++i) position[size_t(i)] = double(i);
        for (int i = 100; i <= 200; ++i)
            position[size_t(i)] = 40.0 + double(i - 100) * 0.2;

        const std::vector<Lap> checked =
            pdsApplyLapDistanceCoverage(laps, position, 1);
        QVERIFY(checked[0].complete);
        QVERIFY(!checked[1].complete);
    }
    void cumulativeDistanceDoesNotRejectLaps() {
        const std::vector<Lap> laps{Lap{0, 0.0, 100.0, 100000.0, true},
                                    Lap{1, 100.0, 200.0, 100000.0, true}};
        std::vector<double> cumulative(201, 0.0);
        for (int i = 0; i <= 200; ++i) cumulative[size_t(i)] = double(i);

        const std::vector<Lap> checked =
            pdsApplyLapDistanceCoverage(laps, cumulative, 1);
        QVERIFY(checked[0].complete);
        QVERIFY(checked[1].complete);
    }
    void missingDistanceCoverageIsUnverifiable() {
        const std::vector<Lap> laps{Lap{0, 0.0, 90.0, 90000.0, true},
                                    Lap{1, 120.0, 210.0, 90000.0, true}};
        std::vector<double> position(100, 0.0);
        for (int i = 0; i < 100; ++i) position[size_t(i)] = double(i);

        const std::vector<Lap> checked =
            pdsApplyLapDistanceCoverage(laps, position, 1);
        QVERIFY(checked[0].complete);
        QVERIFY(checked[1].complete);
    }
    void outlierCoverageDoesNotRejectTypicalLaps() {
        // Three similar oval laps plus one GPS-spike pair that spans the
        // whole session range. Median coverage is low; racing laps stay.
        const std::vector<Lap> laps{Lap{0, 0.0, 40.0, 40000.0, true},
                                    Lap{1, 40.0, 80.0, 40000.0, true},
                                    Lap{2, 80.0, 120.0, 40000.0, true},
                                    Lap{3, 120.0, 160.0, 40000.0, true}};
        std::vector<double> position(161, 0.0);
        for (int i = 0; i <= 120; ++i) position[size_t(i)] = double(i % 41);
        for (int i = 121; i <= 160; ++i) position[size_t(i)] = double(i);

        const std::vector<Lap> checked =
            pdsApplyLapDistanceCoverage(laps, position, 1);
        QVERIFY(checked[0].complete);
        QVERIFY(checked[1].complete);
        QVERIFY(checked[2].complete);
        QVERIFY(checked[3].complete);
    }
    void similarDurationDoesNotEstablishMissingLapBoundaries() {
        TelemetrySource source;
        source.sourceLaps() = {Lap{1, 0.0, 80.0, 80000.0, false},
                               Lap{2, 80.0, 160.0, 80000.0, false},
                               Lap{3, 160.0, 241.0, 81000.0, false},
                               Lap{4, 241.0, 260.0, 19000.0, false}};
        const auto laps = source.detectLaps();
        for (const auto& lap : laps) QVERIFY(!lap.complete);
    }
    void longHeadAndTailFragmentsRemainIncomplete() {
        TelemetrySource source;
        source.sourceLaps() = {Lap{1, 0.0, 60.0, 60000.0, false},
                               Lap{2, 60.0, 160.0, 100000.0, true},
                               Lap{3, 160.0, 260.0, 100000.0, true},
                               Lap{4, 260.0, 330.0, 70000.0, false}};
        const auto laps = source.detectLaps();
        QCOMPARE(laps.size(), size_t(4));
        QVERIFY(!laps.front().complete);
        QVERIFY(laps[1].complete);
        QVERIFY(laps[2].complete);
        QVERIFY(!laps.back().complete);
    }
};

// ────────────────────────────────────────────────────────────────────
// scoreChannelMatch — channel name to alias matching
// ────────────────────────────────────────────────────────────────────

class ScoreChannelMatchTest : public QObject {
    Q_OBJECT
private slots:
    void exactMatchHighestScore() {
        int exact = scoreChannelMatch("speed", "speed", 8);
        QVERIFY(exact > 9000);
    }
    void exactMatchBeatsSubstring() {
        int exact = scoreChannelMatch("speed", "speed", 1);
        int substring = scoreChannelMatch("ground speed", "speed", 0);
        QVERIFY(exact > substring);
    }
    void substringMatch() {
        // "groundspeed" contains "speed" (len >= 4) → score = 7000 - priority
        int score = scoreChannelMatch("ground speed", "speed", 0);
        QCOMPARE(score, 7000);
    }
    void noMatchReturnsMin() {
        int score = scoreChannelMatch("throttle", "brake", 0);
        QCOMPARE(score, std::numeric_limits<int>::min());
    }
    void emptyChannelNameReturnsMin() {
        QCOMPARE(scoreChannelMatch("", "speed", 0),
                 std::numeric_limits<int>::min());
    }
    void lowerPriorityAliasScoresLower() {
        int high = scoreChannelMatch("speed", "speed", 0);
        int low = scoreChannelMatch("speed", "speed", 5);
        QVERIFY(high > low);
    }
    void reverseSubstringMatch() {
        // "groundspeed" (alias) contains "speed" (channel, len >= 6) → 6000
        int score = scoreChannelMatch("speed", "ground speed", 0);
        QCOMPARE(score, 6000);
    }
    void shortAliasDoesNotSubstringMatch() {
        QCOMPARE(scoreChannelMatch("tpsreal", "tps", 0),
                 std::numeric_limits<int>::min());
    }
    void emptyAliasReturnsMin() {
        QCOMPARE(scoreChannelMatch("speed", "", 0),
                 std::numeric_limits<int>::min());
    }
};

// ────────────────────────────────────────────────────────────────────
// dominantDriverId — most frequent positive numeric code
// ────────────────────────────────────────────────────────────────────

class DominantDriverIdTest : public QObject {
    Q_OBJECT
private slots:
    void mostFrequent() {
        std::vector<double> v{0, 3, 3, 3, 5, 5};
        QCOMPARE(dominantDriverId(v), 3.0);
    }
    void tiesGoToEarlier() {
        std::vector<double> v{0, 7, 7, 9, 9};
        // 7 appears first (index 1) vs 9 (index 3)
        QCOMPARE(dominantDriverId(v), 7.0);
    }
    void allNonPositive() {
        std::vector<double> v{0, -1, -2, 0};
        QCOMPARE(dominantDriverId(v), 0.0);
    }
    void empty() { QCOMPARE(dominantDriverId({}), 0.0); }
    void singlePositive() {
        std::vector<double> v{0, 0, 42, 0};
        QCOMPARE(dominantDriverId(v), 42.0);
    }
    void ignoresZeroAndNegatives() {
        std::vector<double> v{0, -5, 0, -5, 3, 3, 3};
        QCOMPARE(dominantDriverId(v), 3.0);
    }
    void preservesFractionalCodes() {
        std::vector<double> v{0, 2.5, 2.5, 3.75};
        QCOMPARE(dominantDriverId(v), 2.5);
    }
    void removesFloat32StorageNoise() {
        std::vector<double> v{double(float(2.1)), double(float(2.1)), 3.0};
        QCOMPARE(dominantDriverId(v, 6), 2.1);
    }
    void preservesFloat64Precision() {
        const double code = 2.12345678901234;
        std::vector<double> v{code, code, 3.0};
        QCOMPARE(dominantDriverId(v, 7), code);
    }
    void ignoresNonFiniteValues() {
        std::vector<double> v{std::numeric_limits<double>::quiet_NaN(),
                              std::numeric_limits<double>::infinity(), 4.5};
        QCOMPARE(dominantDriverId(v), 4.5);
    }
};

// ────────────────────────────────────────────────────────────────────
// VideoClock presentation mapping
// ────────────────────────────────────────────────────────────────────

class VideoClockTest : public QObject {
    Q_OBJECT
private slots:
    void mapsPositiveAndNegativePresentationOffsets() {
        VideoClock positive;
        positive.presentationOffsetNs = 100'000'000;
        QCOMPARE(positive.presentationTimeNs(400'000'000),
                 std::optional<std::uint64_t>(500'000'000));
        QCOMPARE(positive.telemetryTimeNs(500'000'000),
                 std::optional<std::uint64_t>(400'000'000));

        VideoClock negative;
        negative.presentationOffsetNs = -100'000'000;
        QCOMPARE(negative.presentationTimeNs(400'000'000),
                 std::optional<std::uint64_t>(300'000'000));
        QCOMPARE(negative.telemetryTimeNs(300'000'000),
                 std::optional<std::uint64_t>(400'000'000));
    }
    void picksLastPresentedFrameAtOrBeforeTime() {
        VideoClock clock;
        clock.presentationOffsetNs = 100'000'000;
        clock.presentationTimesNs = {100'000'000, 140'000'000, 220'000'000,
                                     300'000'000};
        QCOMPARE(clock.frameAt(60'000'000), std::optional<std::uint64_t>(1));
        QCOMPARE(clock.frameAt(119'000'000), std::optional<std::uint64_t>(1));
        QCOMPARE(clock.frameAt(120'000'000), std::optional<std::uint64_t>(2));
    }
    void appliesOffsetsPerVideoFile() {
        VideoClock clock;
        VideoFileReference active;
        active.filename = "active.mp4";
        active.presentationOffsetNs = 100'000'000;
        active.index = 0;
        VideoFileReference reference;
        reference.filename = "reference.mp4";
        reference.presentationOffsetNs = 250'000'000;
        reference.index = 1;
        clock.files = {active, reference};

        const VideoFileReference* activeFile = clock.fileNamed("active.mp4");
        const VideoFileReference* referenceFile =
            clock.fileNamed("reference.mp4");
        QVERIFY(activeFile != nullptr);
        QVERIFY(referenceFile != nullptr);
        QCOMPARE(activeFile->index, std::uint32_t(0));
        QCOMPARE(referenceFile->index, std::uint32_t(1));
        QVERIFY(clock.fileNamed("missing.mp4") == nullptr);

        QCOMPARE(clock.presentationTimeNs(400'000'000, 0),
                 std::optional<std::uint64_t>(500'000'000));
        QCOMPARE(clock.presentationTimeNs(400'000'000, 1),
                 std::optional<std::uint64_t>(650'000'000));
        QCOMPARE(clock.telemetryTimeNs(650'000'000, 1),
                 std::optional<std::uint64_t>(400'000'000));
    }
};

// ────────────────────────────────────────────────────────────────────
// TelemetrySource with synthetic data
// ────────────────────────────────────────────────────────────────────

class SyntheticSourceTest : public QObject {
    Q_OBJECT
private slots:
    void mapChannelsFindsSpeed() {
        TelemetrySource src;
        RawChannel ch;
        ch.name = "Ground Speed";
        ch.unit = "km/h";
        ch.samples = {0, 50, 100, 150};
        ch.frequencyHz = 100;
        src.channels().push_back(ch);

        auto mapping = src.mapChannels();
        QVERIFY(mapping.count("speed"));
        QCOMPARE(mapping["speed"], 0);
    }

    void mapChannelsFindsMultipleConcepts() {
        TelemetrySource src;
        RawChannel speed;
        speed.name = "Ground Speed";
        speed.unit = "km/h";
        speed.samples = {0, 100};
        RawChannel throttle;
        throttle.name = "Throttle Pos";
        throttle.unit = "%";
        throttle.samples = {0, 50};
        RawChannel gear;
        gear.name = "Gear";
        gear.unit = "";
        gear.samples = {1, 2, 3};
        src.channels() = {speed, throttle, gear};

        auto mapping = src.mapChannels();
        QVERIFY(mapping.count("speed"));
        QVERIFY(mapping.count("throttle"));
        QVERIFY(mapping.count("gear"));
        QCOMPARE(mapping["speed"], 0);
        QCOMPARE(mapping["throttle"], 1);
        QCOMPARE(mapping["gear"], 2);
    }

    void mapDrivingChannelsWithoutConfusingEngineAndVehicleSpeed() {
        // Names/units observed together in real VBOX CAN logs. Engine_Speed
        // precedes Vehicle_Speed in file order; a substring "speed" match
        // used to choose RPM and turn the unified speed trace into zeros.
        auto channel = [](const std::string& name, const std::string& unit,
                          double value) {
            RawChannel ch;
            ch.name = name;
            ch.unit = unit;
            ch.frequencyHz = 2.0;
            ch.durationSec = 1.0;
            ch.samples = {value, value};
            return ch;
        };
        TelemetrySource src;
        src.channels() = {
            channel("Engine_Speed", "RPM", 6000.0),
            channel("Throttle_Pedal", "%", 75.0),
            channel("Vehicle_Speed", "kmh", 250.0),
            channel("Brake_Pressure_Front", "bar", 40.0),
        };
        const auto mapping = src.mapChannels();
        QVERIFY(mapping.count("speed"));
        QCOMPARE(mapping.at("speed"), 2);
        QVERIFY(mapping.count("throttle"));
        QCOMPARE(mapping.at("throttle"), 1);
        QVERIFY(mapping.count("driver_throttle"));
        QCOMPARE(mapping.at("driver_throttle"), 1);
        QCOMPARE(mapping.at("brake"), 3);

        const UnifiedLap lap = src.unifyLap(0.0, 1.0);
        QCOMPARE(lap.speed[25], 250.0);
        QCOMPARE(lap.throttle[25], 0.75);
        QCOMPARE(lap.driverThrottle[25], 0.75);
        QCOMPARE(lap.brake[25], 40.0);
        QVERIFY(lap.distance.back() > 60.0);
    }

    void mapSpeedRejectsAngularUnitsAndAmbiguousSubstring() {
        for (const auto& namedUnit :
             std::vector<std::pair<std::string, std::string>>{
                 {"Speed", "RPM"},
                 {"Ground Speed", "rad/s"},
                 {"Engine_Speed", "RPM"},
                 {"Engine_Speed", ""}}) {
            TelemetrySource src;
            RawChannel channel;
            channel.name = namedUnit.first;
            channel.unit = namedUnit.second;
            channel.samples = {6000.0, 6000.0};
            src.channels() = {channel};
            QVERIFY2(!src.mapChannels().count("speed"),
                     namedUnit.first.c_str());
        }
    }

    void mapSpeedFallsBackToGroundVelocityWithoutWheelSpeed() {
        for (const char* name : {"GPS Speed", "velocity kmh"}) {
            TelemetrySource src;
            RawChannel engine;
            engine.name = "Engine_Speed";
            engine.unit = "RPM";
            engine.samples = {6000.0, 6000.0};
            RawChannel ground;
            ground.name = name;
            ground.unit = "km/h";
            ground.samples = {220.0, 220.0};
            ground.frequencyHz = 2.0;
            ground.durationSec = 1.0;
            src.channels() = {engine, ground};
            const auto mapping = src.mapChannels();
            QVERIFY(mapping.count("speed"));
            QCOMPARE(mapping.at("speed"), 1);
            QCOMPARE(src.unifyLap(0.0, 1.0).speed[25], 220.0);
        }
    }

    void mapFuelDoesNotUseTheRefuellingProbeFlag() {
        TelemetrySource src;
        RawChannel probe;
        probe.name = "Fuel_Probe";
        probe.samples = {0.0, 1.0};
        src.channels() = {probe};
        QVERIFY(!src.mapChannels().count("fuel"));
    }

    void mapChannelsHonorsExplicitOverride() {
        TelemetrySource src;
        RawChannel automatic;
        automatic.name = "Ground Speed";
        automatic.samples = {10, 20};
        RawChannel selected;
        selected.name = "Speed_Ref";
        selected.samples = {30, 40};
        src.channels() = {automatic, selected};

        const auto mapping = src.mapChannels({{"speed", "speed_ref"}});
        QVERIFY(mapping.count("speed"));
        QCOMPARE(mapping.at("speed"), 1);
    }

    void mapChannelsDoesNotGuessInvalidOverride() {
        TelemetrySource src;
        RawChannel automatic;
        automatic.name = "Ground Speed";
        automatic.samples = {10, 20};
        src.channels() = {automatic};

        const auto mapping = src.mapChannels({{"speed", "missing_channel"}});
        QVERIFY(!mapping.count("speed"));
    }

    void mapChannelsSkipsEmptyChannels() {
        TelemetrySource src;
        RawChannel empty;
        empty.name = "Speed";
        empty.samples = {};
        RawChannel real;
        real.name = "Throttle";
        real.samples = {0, 50};
        src.channels() = {empty, real};

        auto mapping = src.mapChannels();
        QVERIFY(!mapping.count("speed"));    // empty channel skipped
        QVERIFY(mapping.count("throttle"));  // non-empty matched
    }

    void mapChannelsNoMatchReturnsEmpty() {
        TelemetrySource src;
        RawChannel ch;
        ch.name = "Unknown Channel";
        ch.samples = {1, 2, 3};
        src.channels() = {ch};

        auto mapping = src.mapChannels();
        QVERIFY(mapping.empty());
    }

    void detectDriverIdFindsDominantId() {
        TelemetrySource src;
        RawChannel ch;
        ch.name = "DriverID";
        ch.samples = {0, 7, 7, 7, 3, 3};
        src.channels() = {ch};

        QCOMPARE(src.detectDriverId(), 7.0);
    }

    void detectDriverIdPreservesFloatBackedCode() {
        TelemetrySource src;
        RawChannel ch;
        ch.name = "DriverID";
        ch.sampleTypeCode = 6;
        ch.samples = {0.0, 2.5, 2.5, 3.0};
        src.channels() = {ch};

        QCOMPARE(src.detectDriverId(), 2.5);
    }

    void detectDriverIdNormalizesFloat32Code() {
        TelemetrySource src;
        RawChannel ch;
        ch.name = "DriverID";
        ch.sampleTypeCode = 6;
        ch.samples = {double(float(2.1)), double(float(2.1)), 3.0};
        src.channels() = {ch};

        QCOMPARE(src.detectDriverId(), 2.1);
    }

    void detectDriverIdNoChannelReturnsZero() {
        TelemetrySource src;
        RawChannel ch;
        ch.name = "Speed";
        ch.samples = {100, 200};
        src.channels() = {ch};

        QCOMPARE(src.detectDriverId(), 0.0);
    }

    void detectDriverIdHonorsChannelOverride() {
        TelemetrySource src;
        RawChannel automatic;
        automatic.name = "X2LNK_driverID";
        automatic.samples = {7, 7, 7};
        RawChannel selected;
        selected.name = "driver_id";
        selected.samples = {2, 2, 3};
        src.channels() = {automatic, selected};

        const ChannelOverrides overrides{{"driver_id", "driver_id"}};
        QCOMPARE(src.detectDriverId(overrides), 2.0);
    }

    void detectDriverIdInvalidOverrideDoesNotGuess() {
        TelemetrySource src;
        RawChannel automatic;
        automatic.name = "DriverID";
        automatic.samples = {7, 7, 7};
        src.channels() = {automatic};

        const ChannelOverrides overrides{{"driver_id", "Missing Selector"}};
        QCOMPARE(src.detectDriverId(overrides), 0.0);
    }

    void sampleAtInterpolatesSyntheticChannels() {
        TelemetrySource src;
        RawChannel channel;
        channel.samples = {0.0, 10.0, 20.0};
        channel.frequencyHz = 2.0;
        channel.durationSec = 1.0;
        src.channels() = {channel};

        double value = 0.0;
        QVERIFY(src.sampleAt(0, 0.25, &value));
        QCOMPARE(value, 5.0);
        QVERIFY(!src.sampleAt(0, 1.5, &value));
    }

    void sampleAtNearestDoesNotBlendOrdinals() {
        TelemetrySource src;
        RawChannel channel;
        channel.samples = {6.0, 3.0};
        channel.frequencyHz = 2.0;
        channel.durationSec = 1.0;
        src.channels() = {channel};

        double linear = 0.0;
        double early = 0.0;
        double late = 0.0;
        QVERIFY(src.sampleAt(0, 0.25, &linear, true));
        QVERIFY(src.sampleAt(0, 0.1, &early, false));
        QVERIFY(src.sampleAt(0, 0.4, &late, false));
        QCOMPARE(linear, 4.5);
        QCOMPARE(early, 6.0);
        QCOMPARE(late, 3.0);
    }

    void throttlePrefersPowertrainOverPedal() {
        TelemetrySource src;
        RawChannel pedal;
        pedal.name = "Driver Throttle Pos";
        pedal.samples = {1, 2};
        RawChannel tps;
        tps.name = "TPS";
        tps.samples = {3, 4};
        src.channels() = {pedal, tps};

        const auto mapping = src.mapChannels();
        QCOMPARE(mapping.at("throttle"), 1);
        QCOMPARE(mapping.at("driver_throttle"), 0);
    }

    void unifyLapDoesNotInventGears() {
        TelemetrySource src;
        RawChannel gear;
        gear.name = "Gear";
        gear.samples = {6.0, 1.0};
        gear.frequencyHz = 2.0;
        gear.durationSec = 1.0;
        src.channels() = {gear};

        const UnifiedLap lap = src.unifyLap(0.0, 1.0);
        QVERIFY(lap.gear.size() > 2);
        for (int value : lap.gear)
            QVERIFY2(value == 6 || value == 1,
                     "interpolated gear is not a real ratio");
    }

    void unifyLapTreatsKphAsKilometersPerHour() {
        TelemetrySource src;
        RawChannel speed;
        speed.name = "Speed";
        speed.unit = "kph";
        speed.samples = {100.0, 100.0};
        speed.frequencyHz = 2.0;
        speed.durationSec = 1.0;
        src.channels() = {speed};

        const UnifiedLap lap = src.unifyLap(0.0, 1.0);
        QCOMPARE(lap.speed[25], 100.0);
    }

    void unifyLapConvertsMegapascalBrake() {
        TelemetrySource src;
        RawChannel brake;
        brake.name = "Brake Pressure F";
        brake.unit = "MPa";
        brake.samples = {1.0, 1.0};
        brake.frequencyHz = 2.0;
        brake.durationSec = 1.0;
        src.channels() = {brake};

        const UnifiedLap lap = src.unifyLap(0.0, 1.0);
        QCOMPARE(lap.brake[25], 10.0);
    }

    void unifyLapLeavesMissingGpsAsNan() {
        TelemetrySource src;
        RawChannel speed;
        speed.name = "Speed";
        speed.unit = "km/h";
        speed.samples = {10.0, 10.0};
        speed.frequencyHz = 2.0;
        speed.durationSec = 1.0;
        src.channels() = {speed};

        const UnifiedLap lap = src.unifyLap(0.0, 1.0);
        QVERIFY(!std::isfinite(lap.gpsLat[0]));
        QVERIFY(!std::isfinite(lap.gpsLon[0]));
    }

    void unifyLapConvertsAngularMinutesToEastPositiveDegrees() {
        TelemetrySource src;
        RawChannel latitude;
        latitude.name = "latitude";
        latitude.unit = "min";
        latitude.samples = {1800.0, 1800.0};
        latitude.frequencyHz = 2.0;
        latitude.durationSec = 1.0;
        RawChannel longitude = latitude;
        longitude.name = "longitude";
        longitude.samples = {4800.0, 4800.0};
        src.channels() = {latitude, longitude};

        const UnifiedLap lap = src.unifyLap(0.0, 1.0);
        QCOMPARE(lap.gpsLat[25], 30.0);
        QCOMPARE(lap.gpsLon[25], -80.0);
    }

    void unifyLapDoesNotHoldLastGpsAcrossAGap() {
        TelemetrySource src;
        RawChannel latitude;
        latitude.name = "GPS Latitude";
        latitude.unit = "deg";
        latitude.samples = {10.0, 11.0};
        latitude.frequencyHz = 2.0;
        latitude.durationSec = 0.5;
        RawChannel longitude = latitude;
        longitude.name = "GPS Longitude";
        longitude.samples = {20.0, 21.0};
        src.channels() = {latitude, longitude};

        const UnifiedLap lap = src.unifyLap(0.0, 1.0);
        QVERIFY(std::isfinite(lap.gpsLat[0]));
        QVERIFY(!std::isfinite(lap.gpsLat[lap.gpsLat.size() - 1]));
        QVERIFY(!std::isfinite(lap.gpsLon[lap.gpsLon.size() - 1]));
    }

    void detectLapsUsesCurrentLapTimeReset() {
        TelemetrySource src;
        RawChannel timer;
        timer.name = "Current_Lap_Time";
        timer.frequencyHz = 10.0;
        timer.durationSec = 240.0;
        for (int lap = 0; lap < 3; ++lap)
            for (int i = 0; i < 800; ++i)
                timer.samples.push_back(double(i) * 100.0);
        src.channels() = {timer};

        const std::vector<Lap> laps = src.detectLaps();
        QVERIFY(std::any_of(laps.begin(), laps.end(),
                            [](const Lap& lap) { return lap.complete; }));
    }

    void detectLapsPrefersCurrentLapTimeOverDelta() {
        TelemetrySource src;
        RawChannel delta;
        delta.name = "Delta_Lap_Time";
        delta.frequencyHz = 10.0;
        delta.durationSec = 240.0;
        delta.samples.assign(2400, 0.0);
        RawChannel timer = delta;
        timer.name = "Current_Lap_Time";
        timer.samples.clear();
        for (int lap = 0; lap < 3; ++lap)
            for (int i = 0; i < 800; ++i)
                timer.samples.push_back(double(i) * 100.0);
        src.channels() = {delta, timer};

        const std::vector<Lap> laps = src.detectLaps();
        QVERIFY(std::any_of(laps.begin(), laps.end(),
                            [](const Lap& lap) { return lap.complete; }));
    }

    void detectLapsUsesPreviousLt() {
        TelemetrySource src;
        RawChannel previous;
        previous.name = "Previous_LT";
        previous.frequencyHz = 5.0;
        previous.durationSec = 240.0;
        previous.samples.insert(previous.samples.end(), 400, 76852.0);
        previous.samples.insert(previous.samples.end(), 400, 77034.0);
        previous.samples.insert(previous.samples.end(), 400, 77200.0);
        src.channels() = {previous};

        const std::vector<Lap> laps = src.detectLaps();
        QVERIFY(std::any_of(laps.begin(), laps.end(),
                            [](const Lap& lap) { return lap.complete; }));
    }

    void detectLapsUsesSyntheticCounter() {
        TelemetrySource src;
        RawChannel counter;
        counter.name = "Lap Number";
        counter.frequencyHz = 1.0;
        counter.durationSec = 45.0;
        counter.samples.insert(counter.samples.end(), 15, 1.0);
        counter.samples.insert(counter.samples.end(), 15, 2.0);
        counter.samples.insert(counter.samples.end(), 15, 3.0);
        src.channels() = {counter};

        const std::vector<Lap> laps = src.detectLaps();
        QVERIFY(std::any_of(laps.begin(), laps.end(),
                            [](const Lap& lap) { return lap.complete; }));
    }

    void detectLapsMarksAuthoritativeShortLapsIncomplete() {
        TelemetrySource src;
        src.sourceLaps() = {Lap{1, 0.0, 20.0, 20000.0, true},
                            Lap{2, 20.0, 120.0, 100000.0, true},
                            Lap{3, 120.0, 220.0, 100000.0, true}};

        const std::vector<Lap> laps = src.detectLaps();
        QCOMPARE(laps.size(), size_t(3));
        QVERIFY(!laps[0].complete);
        QVERIFY(laps[1].complete);
        QVERIFY(laps[2].complete);
    }

    void detectLapsPrefersAuthoritativeSourceMetadata() {
        TelemetrySource src;
        RawChannel counter;
        counter.name = "Lap Number";
        counter.frequencyHz = 1.0;
        counter.durationSec = 45.0;
        counter.samples.insert(counter.samples.end(), 15, 1.0);
        counter.samples.insert(counter.samples.end(), 15, 2.0);
        counter.samples.insert(counter.samples.end(), 15, 3.0);
        src.channels() = {counter};
        src.sourceLaps() = {Lap{42, 5.0, 25.0, 19750.0, true}};

        const std::vector<Lap> laps = src.detectLaps();
        QCOMPARE(laps.size(), size_t(1));
        QCOMPARE(laps[0].id, 42);
        QCOMPARE(laps[0].startTime, 5.0);
        QCOMPARE(laps[0].endTime, 25.0);
        QCOMPARE(laps[0].timeMs, 19750.0);
        QVERIFY(laps[0].complete);
        QVERIFY(!laps[0].sourceNumber.has_value());
    }

    void classificationDoesNotPromoteIncompleteSourceIntervals() {
        TelemetrySource src;
        src.sourceLaps() = {Lap{1, 0.0, 80.0, 80000.0, false},
                            Lap{2, 80.0, 160.0, 80000.0, false},
                            Lap{3, 160.0, 241.0, 81000.0, false},
                            Lap{4, 241.0, 260.0, 19000.0, false}};

        std::vector<Lap> laps = src.detectLaps();
        classifyLaps(laps);
        QCOMPARE(laps.size(), size_t(4));
        for (const auto& lap : laps) QVERIFY(!lap.complete);
    }

    void unifyLapNormalizesSyntheticChannels() {
        TelemetrySource src;
        RawChannel speed;
        speed.name = "Speed";
        speed.unit = "km/h";
        speed.samples = {0.0, 100.0, 200.0};
        speed.frequencyHz = 2.0;
        speed.durationSec = 1.0;
        RawChannel throttle = speed;
        throttle.name = "Throttle";
        throttle.unit = "%";
        throttle.samples = {0.0, 50.0, 100.0};
        RawChannel latitude = speed;
        latitude.name = "GPS Latitude";
        latitude.unit = "rad";
        latitude.samples = {1.0, 1.0, 1.0};
        RawChannel longitude = latitude;
        longitude.name = "GPS Longitude";
        longitude.unit = "deg";
        longitude.samples = {2.0, 2.0, 2.0};
        src.channels() = {speed, throttle, latitude, longitude};

        const UnifiedLap lap = src.unifyLap(0.0, 1.0);
        QCOMPARE(lap.size(), size_t(51));
        QCOMPARE(lap.speed[25], 100.0);
        QCOMPARE(lap.throttle[25], 0.5);
        QVERIFY(std::fabs(lap.gpsLat[25] - 57.29577951308232) < 1e-9);
        QCOMPARE(lap.gpsLon[25], 2.0);
    }

    void unifyLapRejectsDegenerateBounds() {
        TelemetrySource src;
        QVERIFY(src.unifyLap(1.0, 1.0).size() == 0);
        QVERIFY(src.unifyLap(2.0, 1.0).size() == 0);
    }

    void defaultConstructedSourceIsSafe() {
        TelemetrySource src;
        QVERIFY(src.channels().empty());
        QVERIFY(src.sourceLaps().empty());
        QVERIFY(src.path().empty());
        QVERIFY(src.formatName().empty());
        QVERIFY(src.mapChannels().empty());
        QVERIFY(src.detectLaps().empty());
        QCOMPARE(src.detectDriverId(), 0.0);
        double out = 0;
        QVERIFY(!src.sampleAt(0, 0.0, &out));
        std::string error;
        QVERIFY(!src.writeTelemetry("/tmp/unused.telemetry", &error));
        QVERIFY(!error.empty());
    }

    void unifyLapTreatsUnitlessSpeedAsKmh() {
        TelemetrySource src;
        RawChannel speed;
        speed.name = "Speed_Wspd_App";
        speed.samples = {250.0, 250.0};
        speed.frequencyHz = 2.0;
        speed.durationSec = 1.0;
        src.channels() = {speed};
        QCOMPARE(src.unifyLap(0.0, 1.0).speed[25], 250.0);
    }
    void unifyLapIgnoresImpossibleSpeedSamples() {
        TelemetrySource src;
        RawChannel speed;
        speed.name = "Speed";
        speed.unit = "km/h";
        speed.samples = {100.0, 10000.0, 100.0};
        speed.frequencyHz = 2.0;
        speed.durationSec = 1.0;
        src.channels() = {speed};

        const UnifiedLap lap = src.unifyLap(0.0, 1.0);
        for (double value : lap.speed) QVERIFY(value <= 500.0);
        for (double value : lap.distance) QVERIFY(std::isfinite(value));
        QVERIFY(lap.speed[25] <= 500.0);
    }

    void unifyLapConvertsMilesPerHour() {
        TelemetrySource src;
        RawChannel speed;
        speed.name = "Speed";
        speed.unit = "mph";
        speed.samples = {100.0, 100.0};
        speed.frequencyHz = 2.0;
        speed.durationSec = 1.0;
        src.channels() = {speed};
        const UnifiedLap lap = src.unifyLap(0.0, 1.0);
        QVERIFY(std::fabs(lap.speed[25] - 160.934) < 0.01);
    }

    void unifyLapConvertsMetresPerSecond() {
        TelemetrySource src;
        RawChannel speed;
        speed.name = "Speed";
        speed.unit = "m/s";
        speed.samples = {10.0, 10.0};
        speed.frequencyHz = 2.0;
        speed.durationSec = 1.0;
        src.channels() = {speed};
        QCOMPARE(src.unifyLap(0.0, 1.0).speed[25], 36.0);
    }

    void unifyLapConvertsPsiBrake() {
        TelemetrySource src;
        RawChannel brake;
        brake.name = "Brake Pressure F";
        brake.unit = "psi";
        brake.samples = {100.0, 100.0};
        brake.frequencyHz = 2.0;
        brake.durationSec = 1.0;
        src.channels() = {brake};
        QVERIFY(std::fabs(src.unifyLap(0.0, 1.0).brake[25] - 6.89476) < 1e-4);
    }

    void unifyLapConvertsKilopascalBrake() {
        TelemetrySource src;
        RawChannel brake;
        brake.name = "Brake Pressure F";
        brake.unit = "kPa";
        brake.samples = {100.0, 100.0};
        brake.frequencyHz = 2.0;
        brake.durationSec = 1.0;
        src.channels() = {brake};
        QCOMPARE(src.unifyLap(0.0, 1.0).brake[25], 1.0);
    }

    void unifyLapFallsBackToBrakePosition() {
        TelemetrySource src;
        RawChannel pedal;
        pedal.name = "Brake Pos";
        pedal.samples = {0.5, 0.5};
        pedal.frequencyHz = 2.0;
        pedal.durationSec = 1.0;
        src.channels() = {pedal};
        QCOMPARE(src.unifyLap(0.0, 1.0).brake[25], 50.0);
    }

    void unifyLapScalesPercentThrottleAndClutch() {
        TelemetrySource src;
        RawChannel throttle;
        throttle.name = "Throttle Pos";
        throttle.unit = "%";
        throttle.samples = {75.0, 75.0};
        throttle.frequencyHz = 2.0;
        throttle.durationSec = 1.0;
        RawChannel clutch = throttle;
        clutch.name = "Clutch Pos";
        clutch.samples = {25.0, 25.0};
        src.channels() = {throttle, clutch};
        const UnifiedLap lap = src.unifyLap(0.0, 1.0);
        QCOMPARE(lap.throttle[25], 0.75);
        QCOMPARE(lap.clutch[25], 0.25);
    }

    void unifyLapConvertsSteeringRadians() {
        TelemetrySource src;
        RawChannel steering;
        steering.name = "Steering Angle";
        steering.unit = "rad";
        steering.samples = {1.5707963267948966, 1.5707963267948966};
        steering.frequencyHz = 2.0;
        steering.durationSec = 1.0;
        src.channels() = {steering};
        QVERIFY(std::fabs(src.unifyLap(0.0, 1.0).steering[25] - 90.0) < 1e-6);
    }

    void unifyLapShiftsTwoToSevenGearEncoding() {
        TelemetrySource src;
        RawChannel gear;
        gear.name = "Gear";
        gear.samples = {2.0, 7.0};
        gear.frequencyHz = 2.0;
        gear.durationSec = 1.0;
        src.channels() = {gear};
        const UnifiedLap lap = src.unifyLap(0.0, 1.0);
        QVERIFY(lap.gear.front() == 1);
        QVERIFY(lap.gear.back() == 6);
    }

    void unifyLapKeepsDisplayGearWhenFlyingLapStartsAtTwo() {
        TelemetrySource src;
        RawChannel gear;
        gear.name = "Gear";
        gear.samples = {2.0, 3.0};
        gear.frequencyHz = 2.0;
        gear.durationSec = 1.0;
        src.channels() = {gear};
        const UnifiedLap lap = src.unifyLap(0.0, 1.0);
        QVERIFY(lap.gear.front() == 2);
        QVERIFY(lap.gear.back() == 3);
    }

    void unifyLapConvertsGpsAccuracyUnits() {
        TelemetrySource src;
        RawChannel accuracy;
        accuracy.name = "GPS Position Accuracy";
        accuracy.unit = "cm";
        accuracy.samples = {250.0, 250.0};
        accuracy.frequencyHz = 2.0;
        accuracy.durationSec = 1.0;
        src.channels() = {accuracy};
        QCOMPARE(src.unifyLap(0.0, 1.0).gpsPositionAccuracy[25], 2.5);
    }

    void unifyLapMapsDampersAndLateralG() {
        TelemetrySource src;
        RawChannel fl;
        fl.name = "Damper Travel FL";
        fl.samples = {12.0, 12.0};
        fl.frequencyHz = 2.0;
        fl.durationSec = 1.0;
        RawChannel lat;
        lat.name = "G Force Lat";
        lat.samples = {1.2, 1.2};
        lat.frequencyHz = 2.0;
        lat.durationSec = 1.0;
        src.channels() = {fl, lat};
        const auto mapping = src.mapChannels();
        QCOMPARE(mapping.at("damper_fl"), 0);
        QCOMPARE(mapping.at("g_lat"), 1);
        const UnifiedLap lap = src.unifyLap(0.0, 1.0);
        QCOMPARE(lap.damperFL[25], 12.0);
        QCOMPARE(lap.gForceLat[25], 1.2);
    }

    void unifyLapDistanceIsMonotonicAndStartsAtZero() {
        TelemetrySource src;
        RawChannel speed;
        speed.name = "Speed";
        speed.unit = "km/h";
        speed.samples = {72.0, 72.0};
        speed.frequencyHz = 2.0;
        speed.durationSec = 1.0;
        src.channels() = {speed};
        const UnifiedLap lap = src.unifyLap(0.0, 1.0);
        QCOMPARE(lap.distance.front(), 0.0);
        for (size_t i = 1; i < lap.distance.size(); ++i)
            QVERIFY(lap.distance[i] >= lap.distance[i - 1]);
        QVERIFY(std::fabs(lap.time[1] - 0.02) < 1e-9);
        QCOMPARE(lap.sampleRate, 50);
    }

    void sampleAtRejectsNegativeTime() {
        TelemetrySource src;
        RawChannel channel;
        channel.samples = {1.0, 2.0};
        channel.frequencyHz = 2.0;
        src.channels() = {channel};
        double value = 0.0;
        QVERIFY(!src.sampleAt(0, -0.1, &value));
        QVERIFY(!src.sampleAt(3, 0.0, &value));
    }
};

class MotecExportTest : public QObject {
    Q_OBJECT
private slots:
    void missingTelemetryHasNoVideoClock() {
        QVERIFY(!telemetryHasVideoClock(""));
        QVERIFY(!telemetryHasVideoClock("/no/such/file.telemetry"));
    }

    void compareReportsChannelDelta() {
        TelemetrySource left;
        TelemetrySource right;
        RawChannel speed;
        speed.name = "Speed";
        speed.unit = "km/h";
        speed.samples = {100.0, 110.0};
        speed.frequencyHz = 2.0;
        speed.durationSec = 1.0;
        RawChannel latitude;
        latitude.name = "GPS Latitude";
        latitude.samples = {43.8, 43.9};
        latitude.frequencyHz = 2.0;
        latitude.durationSec = 1.0;
        left.channels() = {speed, latitude};
        RawChannel shifted = speed;
        shifted.samples = {100.0, 120.0};
        right.channels() = {shifted, latitude};
        const std::string report =
            compareTelemetrySources(left, right, "aimd", "telemetry");
        QVERIFY(report.find("gps_lat") != std::string::npos);
        QVERIFY(report.find("d=10") != std::string::npos);
    }

    void writesLdThatReopens() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString vbo = directory.filePath(QStringLiteral("run.vbo"));
        QFile input(vbo);
        QVERIFY(input.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QVERIFY(input.write("[header]\n"
                            "time\n"
                            "velocity kmh\n"
                            "[column names]\n"
                            "time velocity\n"
                            "[data]\n"
                            "120000.0 10\n"
                            "120000.5 20\n"
                            "120001.0 30\n") > 0);
        input.close();

        std::string error;
        const auto source = TelemetrySource::open(vbo.toStdString(), &error);
        QVERIFY2(source, error.c_str());
        const QString native =
            directory.filePath(QStringLiteral(".run.vbo.telemetry"));
        QVERIFY2(source->writeTelemetry(native.toStdString(), &error),
                 error.c_str());
        QVERIFY(QFileInfo::exists(native));

        error.clear();
        const auto reopened =
            TelemetrySource::open(native.toStdString(), &error);
        QVERIFY2(reopened, error.c_str());
        QVERIFY(!reopened->channels().empty());
    }

    void unifyLapConvertsMilesPerHour() {
        TelemetrySource src;
        RawChannel speed;
        speed.name = "Speed";
        speed.unit = "mph";
        speed.samples = {100.0, 100.0};
        speed.frequencyHz = 2.0;
        speed.durationSec = 1.0;
        src.channels() = {speed};
        const UnifiedLap lap = src.unifyLap(0.0, 1.0);
        QVERIFY(std::fabs(lap.speed[25] - 160.934) < 0.01);
    }

    void unifyLapConvertsMetresPerSecond() {
        TelemetrySource src;
        RawChannel speed;
        speed.name = "Speed";
        speed.unit = "m/s";
        speed.samples = {10.0, 10.0};
        speed.frequencyHz = 2.0;
        speed.durationSec = 1.0;
        src.channels() = {speed};
        QCOMPARE(src.unifyLap(0.0, 1.0).speed[25], 36.0);
    }

    void unifyLapConvertsPsiBrake() {
        TelemetrySource src;
        RawChannel brake;
        brake.name = "Brake Pressure F";
        brake.unit = "psi";
        brake.samples = {100.0, 100.0};
        brake.frequencyHz = 2.0;
        brake.durationSec = 1.0;
        src.channels() = {brake};
        QVERIFY(std::fabs(src.unifyLap(0.0, 1.0).brake[25] - 6.89476) < 1e-4);
    }

    void unifyLapConvertsKilopascalBrake() {
        TelemetrySource src;
        RawChannel brake;
        brake.name = "Brake Pressure F";
        brake.unit = "kPa";
        brake.samples = {100.0, 100.0};
        brake.frequencyHz = 2.0;
        brake.durationSec = 1.0;
        src.channels() = {brake};
        QCOMPARE(src.unifyLap(0.0, 1.0).brake[25], 1.0);
    }

    void unifyLapFallsBackToBrakePosition() {
        TelemetrySource src;
        RawChannel pedal;
        pedal.name = "Brake Pos";
        pedal.samples = {0.5, 0.5};
        pedal.frequencyHz = 2.0;
        pedal.durationSec = 1.0;
        src.channels() = {pedal};
        QCOMPARE(src.unifyLap(0.0, 1.0).brake[25], 50.0);
    }

    void unifyLapScalesPercentThrottleAndClutch() {
        TelemetrySource src;
        RawChannel throttle;
        throttle.name = "Throttle Pos";
        throttle.unit = "%";
        throttle.samples = {75.0, 75.0};
        throttle.frequencyHz = 2.0;
        throttle.durationSec = 1.0;
        RawChannel clutch = throttle;
        clutch.name = "Clutch Pos";
        clutch.samples = {25.0, 25.0};
        src.channels() = {throttle, clutch};
        const UnifiedLap lap = src.unifyLap(0.0, 1.0);
        QCOMPARE(lap.throttle[25], 0.75);
        QCOMPARE(lap.clutch[25], 0.25);
    }

    void unifyLapConvertsSteeringRadians() {
        TelemetrySource src;
        RawChannel steering;
        steering.name = "Steering Angle";
        steering.unit = "rad";
        steering.samples = {1.5707963267948966, 1.5707963267948966};
        steering.frequencyHz = 2.0;
        steering.durationSec = 1.0;
        src.channels() = {steering};
        QVERIFY(std::fabs(src.unifyLap(0.0, 1.0).steering[25] - 90.0) < 1e-6);
    }

    void unifyLapShiftsTwoToSevenGearEncoding() {
        TelemetrySource src;
        RawChannel gear;
        gear.name = "Gear";
        gear.samples = {2.0, 7.0};
        gear.frequencyHz = 2.0;
        gear.durationSec = 1.0;
        src.channels() = {gear};
        const UnifiedLap lap = src.unifyLap(0.0, 1.0);
        QVERIFY(lap.gear.front() == 1);
        QVERIFY(lap.gear.back() == 6);
    }

    void unifyLapKeepsDisplayGearWhenFlyingLapStartsAtTwo() {
        TelemetrySource src;
        RawChannel gear;
        gear.name = "Gear";
        gear.samples = {2.0, 3.0};
        gear.frequencyHz = 2.0;
        gear.durationSec = 1.0;
        src.channels() = {gear};
        const UnifiedLap lap = src.unifyLap(0.0, 1.0);
        QVERIFY(lap.gear.front() == 2);
        QVERIFY(lap.gear.back() == 3);
    }

    void unifyLapConvertsGpsAccuracyUnits() {
        TelemetrySource src;
        RawChannel accuracy;
        accuracy.name = "GPS Position Accuracy";
        accuracy.unit = "cm";
        accuracy.samples = {250.0, 250.0};
        accuracy.frequencyHz = 2.0;
        accuracy.durationSec = 1.0;
        src.channels() = {accuracy};
        QCOMPARE(src.unifyLap(0.0, 1.0).gpsPositionAccuracy[25], 2.5);
    }

    void unifyLapMapsDampersAndLateralG() {
        TelemetrySource src;
        RawChannel fl;
        fl.name = "Damper Travel FL";
        fl.samples = {12.0, 12.0};
        fl.frequencyHz = 2.0;
        fl.durationSec = 1.0;
        RawChannel lat;
        lat.name = "G Force Lat";
        lat.samples = {1.2, 1.2};
        lat.frequencyHz = 2.0;
        lat.durationSec = 1.0;
        src.channels() = {fl, lat};
        const auto mapping = src.mapChannels();
        QCOMPARE(mapping.at("damper_fl"), 0);
        QCOMPARE(mapping.at("g_lat"), 1);
        const UnifiedLap lap = src.unifyLap(0.0, 1.0);
        QCOMPARE(lap.damperFL[25], 12.0);
        QCOMPARE(lap.gForceLat[25], 1.2);
    }

    void unifyLapDistanceIsMonotonicAndStartsAtZero() {
        TelemetrySource src;
        RawChannel speed;
        speed.name = "Speed";
        speed.unit = "km/h";
        speed.samples = {72.0, 72.0};
        speed.frequencyHz = 2.0;
        speed.durationSec = 1.0;
        src.channels() = {speed};
        const UnifiedLap lap = src.unifyLap(0.0, 1.0);
        QCOMPARE(lap.distance.front(), 0.0);
        for (size_t i = 1; i < lap.distance.size(); ++i)
            QVERIFY(lap.distance[i] >= lap.distance[i - 1]);
        QVERIFY(std::fabs(lap.time[1] - 0.02) < 1e-9);
        QCOMPARE(lap.sampleRate, 50);
    }

    void sampleAtRejectsNegativeTime() {
        TelemetrySource src;
        RawChannel channel;
        channel.samples = {1.0, 2.0};
        channel.frequencyHz = 2.0;
        src.channels() = {channel};
        double value = 0.0;
        QVERIFY(!src.sampleAt(0, -0.1, &value));
        QVERIFY(!src.sampleAt(3, 0.0, &value));
    }
};

class SidecarJoinTest : public QObject {
    Q_OBJECT
private slots:
    void shiftIsZeroWhenHostHasNoUtc() {
        QCOMPARE(sidecarJoinShiftNs(-1, 1742040000000000000LL), 0);
    }
    void shiftIsExtMinusHost() {
        QCOMPARE(sidecarJoinShiftNs(100, 140), 40);
        QCOMPARE(sidecarJoinShiftNs(140, 100), -40);
    }
    void overlapIsHalfOpen() {
        QVERIFY(nsRangesOverlap(0, 10, 5, 15));
        QVERIFY(!nsRangesOverlap(0, 10, 10, 20));
        QVERIFY(nsRangesOverlap(0, 10, 0, 10));
        QVERIFY(!nsRangesOverlap(0, 0, 0, 10));
    }
    void recognizesExtNames() {
        QVERIFY(isJsonlExtPath("Sebring.telemetry.ext.jsonl"));
        QVERIFY(isJsonlExtPath("race.mtx.jsonl.zstd"));
        QVERIFY(isJsonlPath("race.telemetry.jsonl"));
        QVERIFY(!isJsonlExtPath("race.telemetry"));
        QVERIFY(!isJsonlPath("race.pds"));
    }
    void gpsWeekAndItowBecomeUtcAtT0() {
        // Week 2429 + 493904000 ms − 18 s leap = 2026-07-31 17:11:26 UTC.
        QCOMPARE(utcStartNsFromGps(2429.0, 493904000.0, 0.0),
                 1785517886000000000LL);
        QCOMPARE(utcStartNsFromGps(2429.0, 493904000.0, 2.0),
                 1785517884000000000LL);
        QCOMPARE(utcStartNsFromGps(-1.0, 1.0, 0.0), -1);
    }
};

class JsonlSidecarTest : public QObject {
    Q_OBJECT
private slots:
    void opensMtxHeaderAndSpans() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path =
            directory.filePath(QStringLiteral("race.telemetry.ext.jsonl"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        file.write(
            "{\"mtx\":1,\"n\":\"Sebring 12H 2025\",\"q\":1000000,"
            "\"dur\":12600000000000,\"vis\":1,\"utc\":1742040000000000000,"
            "\"tz\":\"America/New_York\",\"r\":[{\"t\":\"LMP2 stints\"},"
            "{\"p\":[\"Avg lap\",\"1:52.1\"]}]}\n"
            "{\"k\":\"s\",\"n\":\"443-1\",\"s\":0,\"e\":5400000000000,"
            "\"vis\":1,\"c\":\"#e11d48\",\"p\":{\"title\":\"#443\","
            "\"sub\":\"EL · 1:52.1\"},\"m\":[[\"Laps\",\"28\"],"
            "[\"Best\",\"1:50.332\"]]}\n"
            "{\"n\":\"Heart Rate\",\"hz\":1,\"u\":\"bpm\",\"vis\":1,"
            "\"v\":[80,81],\"t0\":0}\n");
        file.close();

        std::string error;
        auto source = TelemetrySource::open(path.toStdString(), &error);
        QVERIFY2(source, error.c_str());
        QVERIFY(source->isExtension());
        QCOMPARE(QString::fromStdString(source->sidecarName()),
                 QStringLiteral("Sebring 12H 2025"));
        QVERIFY(source->groupVisible());
        QCOMPARE(source->utcStartNs(), 1742040000000000000LL);
        QCOMPARE(source->timezone(), std::string("America/New_York"));
        QCOMPARE(int(source->sidecarChrome().size()), 2);
        QCOMPARE(source->sidecarChrome()[0].kind, SidecarChrome::Kind::Text);
        QCOMPARE(source->sidecarChrome()[1].kind, SidecarChrome::Kind::Pill);
        QCOMPARE(int(source->spans().size()), 1);
        QCOMPARE(source->spans()[0].name, std::string("443-1"));
        QCOMPARE(source->spans()[0].title, std::string("#443"));
        QCOMPARE(int(source->channels().size()), 1);
        QVERIFY(source->channelDefaultVisible(0));
        double value = 0.0;
        QVERIFY(source->sampleAtNs(0, 0, &value));
        QCOMPARE(value, 80.0);
    }

    void refusesSidecarWithoutUtc() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path =
            directory.filePath(QStringLiteral("bad.mtx.jsonl"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        file.write(
            "{\"mtx\":1,\"n\":\"No utc\",\"q\":1000000,\"dur\":1000000,"
            "\"vis\":1,\"tz\":\"America/New_York\"}\n");
        file.close();
        std::string error;
        auto source = TelemetrySource::open(path.toStdString(), &error);
        QVERIFY(!source);
        QVERIFY(error.find("utc") != std::string::npos);
    }
};

// ────────────────────────────────────────────────────────────────────
// MonotonicSeries — interpolation helpers
// ────────────────────────────────────────────────────────────────────

class MonotonicSeriesTest : public QObject {
    Q_OBJECT
private slots:
    void viewAtEndpoints() {
        const std::vector<double> x{0.0, 1.0, 2.0, 3.0};
        const std::vector<double> y{10.0, 20.0, 30.0, 40.0};
        MonotonicView view{x.data(), y.data(), x.size()};
        QCOMPARE(view.at(0.0), 10.0);
        QCOMPARE(view.at(3.0), 40.0);
        QCOMPARE(view.at(-1.0), 10.0);
        QCOMPARE(view.at(5.0), 40.0);
    }

    void viewAtMidpoint() {
        const std::vector<double> x{0.0, 1.0, 2.0, 3.0};
        const std::vector<double> y{10.0, 20.0, 30.0, 40.0};
        MonotonicView view{x.data(), y.data(), x.size()};
        QCOMPARE(view.at(0.5), 15.0);
        QCOMPARE(view.at(1.5), 25.0);
        QCOMPARE(view.at(2.25), 32.5);
    }

    void viewInvert() {
        const std::vector<double> x{0.0, 1.0, 2.0, 3.0};
        const std::vector<double> y{10.0, 20.0, 30.0, 40.0};
        MonotonicView view{x.data(), y.data(), x.size()};
        QVERIFY(std::abs(view.invert(15.0) - 0.5) < 1e-9);
        QVERIFY(std::abs(view.invert(25.0) - 1.5) < 1e-9);
        QVERIFY(std::abs(view.invert(35.0) - 2.5) < 1e-9);
        QCOMPARE(view.invert(5.0), 0.0);
        QCOMPARE(view.invert(45.0), 3.0);
    }

    void viewLowerIndex() {
        const std::vector<double> x{0.0, 1.0, 2.0, 3.0};
        const std::vector<double> y{10.0, 20.0, 30.0, 40.0};
        MonotonicView view{x.data(), y.data(), x.size()};
        QCOMPARE(view.lowerIndex(0.0), size_t(0));
        QCOMPARE(view.lowerIndex(1.5), size_t(2));
        QCOMPARE(view.lowerIndex(3.0), size_t(3));
        QCOMPARE(view.lowerIndex(5.0), size_t(3));
    }

    void freeInterpolate() {
        const std::vector<double> x{0.0, 10.0, 20.0};
        const std::vector<double> y{0.0, 100.0, 200.0};
        QCOMPARE(interpolate(x, y, 5.0), 50.0);
        QCOMPARE(interpolate(x, y, 0.0), 0.0);
        QCOMPARE(interpolate(x, y, 20.0), 200.0);
    }

    void interpolateFractionUniform() {
        const std::vector<double> y{0.0, 10.0, 20.0, 30.0};
        QCOMPARE(interpolateFraction(y, 0.0), 0.0);
        QCOMPARE(interpolateFraction(y, 1.0), 30.0);
        QVERIFY(std::abs(interpolateFraction(y, 0.5) - 15.0) < 1e-9);
        QVERIFY(std::abs(interpolateFraction(y, 0.25) - 7.5) < 1e-9);
    }

    void interpolateFractionClamps() {
        const std::vector<double> y{0.0, 10.0, 20.0};
        QCOMPARE(interpolateFraction(y, -0.5), 0.0);
        QCOMPARE(interpolateFraction(y, 1.5), 20.0);
    }

    void interpolateFractionDegenerate() {
        std::vector<double> empty;
        QCOMPARE(interpolateFraction(empty, 0.5), 0.0);
        const std::vector<double> single{42.0};
        QCOMPARE(interpolateFraction(single, 0.5), 42.0);
    }

    void invertFractionRoundTrip() {
        const std::vector<double> y{0.0, 10.0, 20.0, 30.0};
        for (double f : {0.1, 0.25, 0.5, 0.75, 0.9}) {
            const double value = interpolateFraction(y, f);
            const double back = invertFraction(y, value);
            QVERIFY(std::abs(back - f) < 1e-9);
        }
    }

    void invertFractionDegenerate() {
        std::vector<double> empty;
        QCOMPARE(invertFraction(empty, 5.0), 0.0);
        const std::vector<double> flat{5.0, 5.0, 5.0};
        QCOMPARE(invertFraction(flat, 5.0), 0.0);
    }
};

// Run all test classes in one executable. QTEST_APPLESS_MAIN only runs one
// class; a custom main ensures every Q_OBJECT class is executed.
int main(int argc, char* argv[]) {
    int status = 0;
    {
        NormalizeChannelNameTest t;
        status |= QTest::qExec(&t, argc, argv);
    }
    {
        FormatLapTimeTest t;
        status |= QTest::qExec(&t, argc, argv);
    }
    {
        SessionMetaFromFilenameTest t;
        status |= QTest::qExec(&t, argc, argv);
    }
    {
        ResampleTest t;
        status |= QTest::qExec(&t, argc, argv);
    }
    {
        BeaconSplitsTest t;
        status |= QTest::qExec(&t, argc, argv);
    }
    {
        LapTimeSplitsTest t;
        status |= QTest::qExec(&t, argc, argv);
    }
    {
        LastLapTimeSplitsTest t;
        status |= QTest::qExec(&t, argc, argv);
    }
    {
        LapNumberSplitsTest t;
        status |= QTest::qExec(&t, argc, argv);
    }
    {
        DistanceSplitsTest t;
        status |= QTest::qExec(&t, argc, argv);
    }
    {
        BuildLapsFromSplitsTest t;
        status |= QTest::qExec(&t, argc, argv);
    }
    {
        ScoreChannelMatchTest t;
        status |= QTest::qExec(&t, argc, argv);
    }
    {
        DominantDriverIdTest t;
        status |= QTest::qExec(&t, argc, argv);
    }
    {
        VideoClockTest t;
        status |= QTest::qExec(&t, argc, argv);
    }
    {
        SyntheticSourceTest t;
        status |= QTest::qExec(&t, argc, argv);
    }
    {
        MotecExportTest t;
        status |= QTest::qExec(&t, argc, argv);
    }
    {
        SidecarJoinTest t;
        status |= QTest::qExec(&t, argc, argv);
    }
    {
        JsonlSidecarTest t;
        status |= QTest::qExec(&t, argc, argv);
    }
    {
        MonotonicSeriesTest t;
        status |= QTest::qExec(&t, argc, argv);
    }
    return status;
}
#include "CoreTest.moc"
