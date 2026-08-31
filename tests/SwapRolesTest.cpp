#include "app/SwapRoles.h"

#include <QTest>

class SwapRolesTest : public QObject {
    Q_OBJECT
private slots:
    void swappingKeepsNeighbourLapViewportSpace() {
        const auto map = [](double fraction) { return fraction * fraction; };
        QCOMPARE(omatrack::swappedViewportFraction(-0.2, false, 0.0, map),
                 -0.2);
        QCOMPARE(omatrack::swappedViewportFraction(1.2, false, 0.0, map), 1.2);
        QCOMPARE(omatrack::swappedViewportFraction(0.4, false, 0.0, map), 0.16);
    }
    void manualTranslationIsNotClampedAtTheLapEdge() {
        const auto map = [](double fraction) { return fraction; };
        QCOMPARE(omatrack::swappedViewportFraction(-0.1, true, 0.02, map),
                 -0.12);
        QCOMPARE(omatrack::swappedViewportFraction(1.1, true, 0.02, map), 1.08);
        QCOMPARE(omatrack::swappedViewportFraction(0.2, true, -0.02, map),
                 0.22);
    }
    void noOpWithoutReference() {
        QVERIFY(!omatrack::swapRolesPossible(false));
        QVERIFY(omatrack::swapRolesPossible(true));
    }
};

QTEST_GUILESS_MAIN(SwapRolesTest)
#include "SwapRolesTest.moc"
