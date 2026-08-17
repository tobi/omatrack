// Unit tests for the plain-struct contracts in TelemetryStore.h.
//
// These structs carry inline invariants that the renderer and QML depend on:
// LapEntry::countsForBest gates fastest-lap statistics, CornerZone::mid
// positions the inspector, DamperAlignment::valid/span drive the alignment
// tool, and CornerGraph::valid/hasCompare guard the corner renderer. A
// regression in any of these silently breaks the UI without a compile error.

#include "app/TelemetryStore.h"

#include <QtTest>

// ────────────────────────────────────────────────────────────────────
// LapEntry::countsForBest
// ────────────────────────────────────────────────────────────────────

class LapEntryTest : public QObject {
    Q_OBJECT
private slots:
    void completeNonPitCounts() {
        LapEntry e;
        e.isComplete = true;
        e.isPitLap = false;
        QVERIFY(e.countsForBest());
    }
    void incompleteDoesNotCount() {
        LapEntry e;
        e.isComplete = false;
        e.isPitLap = false;
        QVERIFY(!e.countsForBest());
    }
    void pitLapDoesNotCount() {
        LapEntry e;
        e.isComplete = true;
        e.isPitLap = true;
        QVERIFY(!e.countsForBest());
    }
    void incompletePitLapDoesNotCount() {
        LapEntry e;
        e.isComplete = false;
        e.isPitLap = true;
        QVERIFY(!e.countsForBest());
    }
    void defaultsToCounting() {
        LapEntry e;
        QVERIFY(e.countsForBest());  // default: complete=true, pitLap=false
    }
    void videoIdentityMustBeVerifiedBeforeSync() {
        VideoIdentityResult identity;
        QVERIFY(!identity.trusted());
        identity.status = VideoIdentityStatus::Unverified;
        QVERIFY(!identity.trusted());
        identity.status = VideoIdentityStatus::Mismatch;
        QVERIFY(!identity.trusted());
        identity.status = VideoIdentityStatus::VerifiedHash;
        QVERIFY(identity.trusted());
        identity.status = VideoIdentityStatus::TrustedRemoteObject;
        QVERIFY(identity.trusted());
    }
};

// ────────────────────────────────────────────────────────────────────
// CornerZone::mid
// ────────────────────────────────────────────────────────────────────

class CornerZoneTest : public QObject {
    Q_OBJECT
private slots:
    void midIsAverage() {
        CornerZone z;
        z.start = 0.2;
        z.end = 0.8;
        QCOMPARE(z.mid(), 0.5);
    }
    void midOfZeroWidth() {
        CornerZone z;
        z.start = 0.5;
        z.end = 0.5;
        QCOMPARE(z.mid(), 0.5);
    }
    void midOfFullLap() {
        CornerZone z;
        z.start = 0.0;
        z.end = 1.0;
        QCOMPARE(z.mid(), 0.5);
    }
};

// ────────────────────────────────────────────────────────────────────
// DamperAlignment::valid / span
// ────────────────────────────────────────────────────────────────────

class DamperAlignmentTest : public QObject {
    Q_OBJECT
private slots:
    void emptyIsInvalid() {
        DamperAlignment a;
        QVERIFY(!a.valid());
    }
    void singleSampleIsInvalid() {
        DamperAlignment a;
        a.primary.push_back(1.0);
        a.compare.push_back(1.0);
        QVERIFY(!a.valid());
    }
    void twoSamplesIsValid() {
        DamperAlignment a;
        a.primary.push_back(1.0);
        a.primary.push_back(2.0);
        a.compare.push_back(3.0);
        a.compare.push_back(4.0);
        QVERIFY(a.valid());
    }
    void spanIsMaxMinusMin() {
        DamperAlignment a;
        a.minimum = 1.0;
        a.maximum = 5.0;
        QCOMPARE(a.span(), 4.0);
    }
    void spanClampsToEpsilonWhenFlat() {
        DamperAlignment a;
        a.minimum = 3.0;
        a.maximum = 3.0;
        QVERIFY(a.span() > 0);
        QVERIFY(a.span() < 0.001);
    }
    void onlyPrimaryInvalid() {
        DamperAlignment a;
        a.primary.push_back(1.0);
        a.primary.push_back(2.0);
        QVERIFY(!a.valid());  // compare empty
    }
    void onlyCompareInvalid() {
        DamperAlignment a;
        a.compare.push_back(1.0);
        a.compare.push_back(2.0);
        QVERIFY(!a.valid());  // primary empty
    }
};

class TraceConfidenceBandTest : public QObject {
    Q_OBJECT
private slots:
    void emptyIsInvalid() { QVERIFY(!TraceConfidenceBand{}.valid()); }
    void oneLapIsInvalid() {
        TraceConfidenceBand band;
        band.lapCount = 1;
        band.lower = {0.0, 1.0};
        band.median = {0.5, 1.5};
        band.upper = {1.0, 2.0};
        QVERIFY(!band.valid());
    }
    void mismatchedLengthsAreInvalid() {
        TraceConfidenceBand band;
        band.lapCount = 3;
        band.lower = {0.0, 1.0};
        band.median = {0.5};
        band.upper = {1.0, 2.0};
        QVERIFY(!band.valid());
    }
    void twoLapsWithAlignedSeriesAreValid() {
        TraceConfidenceBand band;
        band.lapCount = 2;
        band.lower = {0.0, 1.0};
        band.median = {0.5, 1.5};
        band.upper = {1.0, 2.0};
        QVERIFY(band.valid());
    }
};

class CornerMarkerTest : public QObject {
    Q_OBJECT
private slots:
    void defaultHasNoReference() {
        CornerMarker marker;
        QCOMPARE(marker.referenceFraction, -1.0);
        QCOMPARE(marker.fraction, 0.0);
        QVERIFY(marker.key.isEmpty());
    }
};

// Run all test classes in one executable.
int main(int argc, char* argv[]) {
    int status = 0;
    {
        LapEntryTest t;
        status |= QTest::qExec(&t, argc, argv);
    }
    {
        CornerZoneTest t;
        status |= QTest::qExec(&t, argc, argv);
    }
    {
        DamperAlignmentTest t;
        status |= QTest::qExec(&t, argc, argv);
    }
    {
        TraceConfidenceBandTest t;
        status |= QTest::qExec(&t, argc, argv);
    }
    {
        CornerMarkerTest t;
        status |= QTest::qExec(&t, argc, argv);
    }

    return status;
}
#include "AppTypesTest.moc"
