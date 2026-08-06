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

// ────────────────────────────────────────────────────────────────────
// CornerGraphSeries::valid
// ────────────────────────────────────────────────────────────────────

class CornerGraphSeriesTest : public QObject {
    Q_OBJECT
private slots:
    void emptyIsInvalid() {
        CornerGraphSeries s;
        QVERIFY(!s.valid());
    }
    void singleSampleIsInvalid() {
        CornerGraphSeries s;
        s.speed = {100.0};
        QVERIFY(!s.valid());
    }
    void twoSamplesIsValid() {
        CornerGraphSeries s;
        s.speed = {100.0, 80.0};
        QVERIFY(s.valid());
    }
};

// ────────────────────────────────────────────────────────────────────
// CornerDamperWindow::valid
// ────────────────────────────────────────────────────────────────────

class CornerDamperWindowTest : public QObject {
    Q_OBJECT
private slots:
    void emptyIsInvalid() {
        CornerDamperWindow w;
        QVERIFY(!w.valid());
    }
    void bothPopulatedIsValid() {
        CornerDamperWindow w;
        w.primary = {1.0, 2.0, 3.0};
        w.compare = {4.0, 5.0, 6.0};
        QVERIFY(w.valid());
    }
    void onlyPrimaryIsInvalid() {
        CornerDamperWindow w;
        w.primary = {1.0, 2.0};
        QVERIFY(!w.valid());
    }
};

// ────────────────────────────────────────────────────────────────────
// CornerGraph::valid / hasCompare
// ────────────────────────────────────────────────────────────────────

class CornerGraphTest : public QObject {
    Q_OBJECT
private slots:
    void emptyIsInvalid() {
        CornerGraph g;
        QVERIFY(!g.valid());
    }
    void primaryOnlyIsValid() {
        CornerGraph g;
        g.primary.speed = {100.0, 80.0};
        QVERIFY(g.valid());
        QVERIFY(!g.hasCompare());
    }
    void withCompareIsValid() {
        CornerGraph g;
        g.primary.speed = {100.0, 80.0};
        g.compare.speed = {90.0, 70.0};
        QVERIFY(g.valid());
        QVERIFY(g.hasCompare());
    }
    void compareOnlyIsInvalid() {
        CornerGraph g;
        g.compare.speed = {90.0, 70.0};
        QVERIFY(!g.valid());  // primary must also be valid
    }
};

// Run all test classes in one executable.
int main(int argc, char* argv[]) {
    int status = 0;
    { LapEntryTest t; status |= QTest::qExec(&t, argc, argv); }
    { CornerZoneTest t; status |= QTest::qExec(&t, argc, argv); }
    { DamperAlignmentTest t; status |= QTest::qExec(&t, argc, argv); }
    { CornerGraphSeriesTest t; status |= QTest::qExec(&t, argc, argv); }
    { CornerDamperWindowTest t; status |= QTest::qExec(&t, argc, argv); }
    { CornerGraphTest t; status |= QTest::qExec(&t, argc, argv); }
    return status;
}
#include "AppTypesTest.moc"
