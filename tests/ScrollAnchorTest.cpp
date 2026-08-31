#include "app/ScrollAnchor.h"

#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QStringListModel>
#include <QTest>

namespace {
// QStringListModel exposes "display" as its role name.
class Fixture {
public:
    QQmlEngine engine;
    QQuickWindow window;
    QStringListModel model;
    QQuickItem* view = nullptr;
    ScrollAnchor anchor;

    Fixture() {
        QStringList rows;
        for (int i = 0; i < 60; ++i)
            rows << QStringLiteral("row-%1").arg(i, 3, 10, QLatin1Char('0'));
        model.setStringList(rows);
        engine.rootContext();
        QQmlComponent component(&engine);
        component.setData(
            "import QtQuick\n"
            "ListView { width: 200; height: 100; reuseItems: true\n"
            "  delegate: Rectangle { required property string display; width: "
            "200; height: 20 } }",
            {});
        view = qobject_cast<QQuickItem*>(component.create());
        Q_ASSERT(view);
        view->setParentItem(window.contentItem());
        view->setProperty("model", QVariant::fromValue<QObject*>(&model));
        anchor.setRole(QStringLiteral("display"));
        anchor.setView(view);
        window.resize(200, 100);
        window.show();
        QTest::qWaitForWindowExposed(&window);
        settle();
    }
    void settle() { QTest::qWait(30); }
    QString topRow() const {
        int row = -1;
        QMetaObject::invokeMethod(
            view, "indexAt", Q_RETURN_ARG(int, row), Q_ARG(double, 1.0),
            Q_ARG(double, view->property("contentY").toReal() + 1.0));
        return row < 0 ? QString() : model.index(row).data().toString();
    }
};
}  // namespace

class ScrollAnchorTest : public QObject {
    Q_OBJECT
private slots:
    void insertingRowsAboveKeepsTheSameRowOnTop() {
        Fixture f;
        f.view->setProperty("contentY",
                            20.0 * 30 + 7);  // row-030 with 7px offset
        f.settle();
        QCOMPARE(f.topRow(), QStringLiteral("row-030"));
        f.model.insertRows(0, 5);
        for (int i = 0; i < 5; ++i)
            f.model.setData(f.model.index(i), QStringLiteral("new-%1").arg(i));
        f.settle();
        QCOMPARE(f.topRow(), QStringLiteral("row-030"));
        // Same pixel offset within the row. (ListView may express the shift
        // through originY rather than contentY, so compare against the item.)
        QQuickItem* item = nullptr;
        int row = -1;
        QMetaObject::invokeMethod(
            f.view, "indexAt", Q_RETURN_ARG(int, row), Q_ARG(double, 1.0),
            Q_ARG(double, f.view->property("contentY").toReal() + 1.0));
        QMetaObject::invokeMethod(f.view, "itemAtIndex",
                                  Q_RETURN_ARG(QQuickItem*, item),
                                  Q_ARG(int, row));
        QVERIFY(item);
        QCOMPARE(f.view->property("contentY").toReal() - item->y(), 7.0);
    }
    void removingRowsAboveKeepsTheSameRowOnTop() {
        Fixture f;
        f.view->setProperty("contentY", 20.0 * 30);
        f.settle();
        f.model.removeRows(0, 10);
        f.settle();
        QCOMPARE(f.topRow(), QStringLiteral("row-030"));
    }
    void removedAnchorFallsBackToNearestNeighbour() {
        Fixture f;
        f.view->setProperty("contentY", 20.0 * 30);
        f.settle();
        f.model.removeRows(30, 1);
        f.settle();
        const QString top = f.topRow();
        QVERIFY2(top == QStringLiteral("row-029") ||
                     top == QStringLiteral("row-031"),
                 qPrintable(top));
    }
    void resetKeepsTheSameRowOnTop() {
        Fixture f;
        f.view->setProperty("contentY", 20.0 * 30);
        f.settle();
        QStringList rows = f.model.stringList();
        rows.prepend(QStringLiteral("inserted-by-reset"));
        f.model.setStringList(rows);  // begin/endResetModel
        f.settle();
        QCOMPARE(f.topRow(), QStringLiteral("row-030"));
    }
    void atTheTopStaysAtTheTop() {
        Fixture f;
        QCOMPARE(f.view->property("contentY").toReal(), 0.0);
        f.model.insertRows(0, 3);
        f.settle();
        QCOMPARE(f.view->property("contentY").toReal(), 0.0);
    }
};
QTEST_MAIN(ScrollAnchorTest)
#include "ScrollAnchorTest.moc"
