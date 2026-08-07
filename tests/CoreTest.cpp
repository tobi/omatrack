// Unit tests for the omatrack Qt-free analysis core.
//
// Covers the public free functions (normalizeChannelName, formatLapTime,
// sessionMetaFromFilename, resample), the detail-namespace lap-detection and
// channel-matching helpers, and TelemetrySource methods with synthetic data.

#include "core/TelemetryEngine.h"
#include "core/TelemetryEngineInternal.h"

#include <QtTest>

#include <cmath>
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
        QCOMPARE(selected.size(), size_t(1));
        QCOMPARE(selected.front(), 105.2);
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
};

// ────────────────────────────────────────────────────────────────────
// dominantDriverId — most frequent positive integer
// ────────────────────────────────────────────────────────────────────

class DominantDriverIdTest : public QObject {
    Q_OBJECT
private slots:
    void mostFrequent() {
        std::vector<double> v{0, 3, 3, 3, 5, 5};
        QCOMPARE(dominantDriverId(v), 3);
    }
    void tiesGoToEarlier() {
        std::vector<double> v{0, 7, 7, 9, 9};
        // 7 appears first (index 1) vs 9 (index 3)
        QCOMPARE(dominantDriverId(v), 7);
    }
    void allNonPositive() {
        std::vector<double> v{0, -1, -2, 0};
        QCOMPARE(dominantDriverId(v), 0);
    }
    void empty() { QCOMPARE(dominantDriverId({}), 0); }
    void singlePositive() {
        std::vector<double> v{0, 0, 42, 0};
        QCOMPARE(dominantDriverId(v), 42);
    }
    void ignoresZeroAndNegatives() {
        std::vector<double> v{0, -5, 0, -5, 3, 3, 3};
        QCOMPARE(dominantDriverId(v), 3);
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

        QCOMPARE(src.detectDriverId(), 7);
    }

    void detectDriverIdNoChannelReturnsZero() {
        TelemetrySource src;
        RawChannel ch;
        ch.name = "Speed";
        ch.samples = {100, 200};
        src.channels() = {ch};

        QCOMPARE(src.detectDriverId(), 0);
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
        QVERIFY(src.path().empty());
        QVERIFY(src.formatName().empty());
        QCOMPARE(src.mediaTimeOffsetSec(), 0.0);
        QVERIFY(src.mapChannels().empty());
        QVERIFY(src.detectLaps().empty());
        QCOMPARE(src.detectDriverId(), 0);
        double out = 0;
        QVERIFY(!src.sampleAt(0, 0.0, &out));
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
        SyntheticSourceTest t;
        status |= QTest::qExec(&t, argc, argv);
    }
    return status;
}
#include "CoreTest.moc"
