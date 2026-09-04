#include "app/FilmstripLayout.h"

#include <QTest>

class FilmstripLayoutTest : public QObject {
    Q_OBJECT
private slots:
    void bookendsAlignAcrossDifferentSessions() {
        for (int count : {4, 7, 18, 100, 400}) {
            QCOMPARE(omatrack::filmstripCellWidth(1000, count, 2, -1, 0), 72.0);
            QCOMPARE(omatrack::filmstripCellWidth(1000, count, 2, 1, 0), 72.0);
            QCOMPARE(omatrack::filmstripCellX(1000, count, 2, true, 0, -1, 0),
                     0.0);
            QCOMPARE(
                omatrack::filmstripCellX(1000, count, 2, true, count - 1, 1, 1),
                928.0);
        }
    }
    void variableLapsUseTheRemainingBudget() {
        const double first = omatrack::filmstripCellWidth(1000, 4, 2, 0, 0.4);
        const double second = omatrack::filmstripCellWidth(1000, 4, 2, 0, 0.6);
        QVERIFY(second > first);
        QVERIFY(std::abs(first + second + 2 * 72 + 3 * 3 - 1000) < 1e-9);
        QCOMPARE(omatrack::filmstripCellWidth(1000, 4, 2, 0, 0), 12.0);
    }
    void onlyBookendsStillAnchorToOppositeEdges() {
        QCOMPARE(omatrack::filmstripCellX(1000, 2, 2, true, 1, 1, 0), 928.0);
    }
    void narrowStripDoesNotOverflow() {
        const double width = 30;
        const auto cells = omatrack::filmstripCells(width, 10, 2);
        QVERIFY(cells.bookend >= 0 && cells.minimum >= 0 &&
                cells.flexible >= 0);
        QVERIFY(2 * cells.bookend + 8 * cells.minimum + cells.flexible +
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
