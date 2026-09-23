#include "app/FilmstripLayout.h"

#include <QTest>

#include <cmath>

class FilmstripLayoutTest : public QObject {
    Q_OBJECT
private slots:
    void pitStopsKeepOneSizeWhereverTheyAre() {
        for (int count : {4, 7, 18, 100}) {
            QCOMPARE(omatrack::filmstripCellWidth(1000, count, 1, true, 0),
                     omatrack::kFilmstripPitStopCell);
            QCOMPARE(omatrack::filmstripCellWidth(1000, count, 2, true, 0),
                     omatrack::kFilmstripPitStopCell);
        }
    }
    void variableCellsFillTheLaneAroundAPitStop() {
        // Out, flying, in, PIT, out, flying: the pit cell sits mid-strip and
        // the first and last cells still reach both lane edges.
        const double weights[] = {0.1, 0.3, 0.1, 0.0, 0.2, 0.3};
        const bool fixed[] = {false, false, false, true, false, false};
        double offset = 0;
        int fixedBefore = 0;
        double end = 0;
        for (int i = 0; i < 6; ++i) {
            const double x = omatrack::filmstripCellX(1000, 6, 1, i,
                                                      fixedBefore, offset);
            const double w = omatrack::filmstripCellWidth(1000, 6, 1,
                                                          fixed[i], weights[i]);
            if (i == 0) QCOMPARE(x, 0.0);
            QVERIFY(x >= end - 1e-9);
            end = x + w;
            if (fixed[i])
                ++fixedBefore;
            else
                offset += weights[i];
        }
        QVERIFY(std::abs(end - 1000) < 1e-9);
    }
    void variableLapsUseTheRemainingBudget() {
        const double first =
            omatrack::filmstripCellWidth(1000, 4, 0, false, 0.4);
        const double second =
            omatrack::filmstripCellWidth(1000, 4, 0, false, 0.6);
        QVERIFY(second > first);
        QCOMPARE(omatrack::filmstripCellWidth(1000, 4, 0, false, 0), 12.0);
    }
    void narrowStripDoesNotOverflow() {
        const double width = 30;
        const auto cells = omatrack::filmstripCells(width, 10, 2);
        QVERIFY(cells.fixed >= 0 && cells.minimum >= 0 && cells.flexible >= 0);
        QVERIFY(2 * cells.fixed + 8 * cells.minimum + cells.flexible +
                    9 * cells.spacing <=
                width + 1e-8);
    }
    void splitVideosUseExistingLetterboxing() {
        QCOMPARE(omatrack::filmstripReservedHeight(1920, 1080, 16.0 / 9.0,
                                                   16.0 / 9.0, 1, 75, 38),
                 0.0);
    }
    void singleVideoWithoutLetterboxingGetsItsOwnLane() {
        QCOMPARE(omatrack::filmstripReservedHeight(1920, 1080, 16.0 / 9.0, 0, 4,
                                                   75, 38),
                 129.0);
    }
    void wideVideoCanLeaveRoomForBothRoles() {
        QCOMPARE(
            omatrack::filmstripReservedHeight(1920, 1200, 2.4, 0, 4, 75, 38),
            0.0);
    }
    void portraitAndUnknownAspectDoNotPretendThereIsBottomSpace() {
        for (double aspect : {9.0 / 16.0, 0.0}) {
            QCOMPARE(omatrack::filmstripReservedHeight(1920, 1080, aspect, 0, 4,
                                                       75, 38),
                     129.0);
        }
    }
    void pictureInPictureStaysAboveTheFilmstrip() {
        for (int mode : {2, 3})
            QCOMPARE(omatrack::filmstripReservedHeight(1920, 1080, 2.4, 2.4,
                                                       mode, 75, 38),
                     129.0);
    }
    void referenceOnlyUsesReferenceAspect() {
        QCOMPARE(
            omatrack::filmstripReservedHeight(1920, 1200, 1, 2.4, 5, 75, 38),
            0.0);
    }
    void emptyAndSmallViewportsStayBounded() {
        QCOMPARE(omatrack::filmstripReservedHeight(1920, 1080, 1, 1, 4, 0, 38),
                 0.0);
        QCOMPARE(omatrack::filmstripReservedHeight(320, 100, 1, 1, 4, 75, 38),
                 100.0);
    }
};

QTEST_GUILESS_MAIN(FilmstripLayoutTest)
#include "FilmstripLayoutTest.moc"
