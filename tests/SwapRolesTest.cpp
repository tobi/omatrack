#include "app/SwapRoles.h"

#include <QTest>

class SwapRolesTest : public QObject {
    Q_OBJECT
private slots:
    void noOpWithoutReference() {
        QVERIFY(!omatrack::swapRolesPossible(false));
        QVERIFY(omatrack::swapRolesPossible(true));
    }
};

QTEST_GUILESS_MAIN(SwapRolesTest)
#include "SwapRolesTest.moc"
