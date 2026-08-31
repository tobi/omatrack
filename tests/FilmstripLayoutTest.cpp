#include "app/FilmstripLayout.h"

#include <QTest>

class FilmstripLayoutTest : public QObject {
    Q_OBJECT
private slots:
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
