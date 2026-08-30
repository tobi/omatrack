#include "app/LibraryModel.h"

#include <QAbstractItemModel>
#include <QSignalSpy>
#include <QTest>

class LibraryModelTest : public QObject {
    Q_OBJECT
private slots:
    void refreshDoesNotReset() {
        LibraryModel model;
        QVariantList sources;
        sources.append(QVariantMap{
            {QStringLiteral("role"), QStringLiteral("source")},
            {QStringLiteral("name"), QStringLiteral("Local")},
            {QStringLiteral("path"), QStringLiteral("/tmp/a")},
            {QStringLiteral("children"),
             QVariantList{QVariantMap{
                 {QStringLiteral("role"), QStringLiteral("file")},
                 {QStringLiteral("name"), QStringLiteral("a.ld")},
                 {QStringLiteral("path"), QStringLiteral("/tmp/a/a.ld")},
                 {QStringLiteral("key"), QStringLiteral("/tmp/a/a.ld")},
                 {QStringLiteral("hasSession"), true},
             }}},
        });
        model.setTree(sources);
        QCOMPARE(model.rowCount(), 2);

        QSignalSpy reset(&model, &QAbstractItemModel::modelReset);
        QSignalSpy inserted(&model, &QAbstractItemModel::rowsInserted);
        sources.append(QVariantMap{
            {QStringLiteral("role"), QStringLiteral("source")},
            {QStringLiteral("name"), QStringLiteral("USB — Stick")},
            {QStringLiteral("path"), QStringLiteral("/media/stick")},
            {QStringLiteral("children"), QVariantList{}},
        });
        model.setTree(sources);
        QCOMPARE(reset.size(), 0);
        QVERIFY(inserted.size() >= 1);
        QCOMPARE(model.rowCount(), 3);
    }

    void filterDoesNotReset() {
        LibraryModel model;
        LibraryFilterModel filter;
        filter.setSourceModel(&model);
        model.setTree(QVariantList{QVariantMap{
            {QStringLiteral("role"), QStringLiteral("file")},
            {QStringLiteral("name"), QStringLiteral("a.ld")},
            {QStringLiteral("path"), QStringLiteral("/tmp/a.ld")},
            {QStringLiteral("key"), QStringLiteral("/tmp/a.ld")},
            {QStringLiteral("track"), QStringLiteral("Sebring")},
            {QStringLiteral("sessionDayKey"), QStringLiteral("2026-08-30")},
        }});
        QSignalSpy reset(&filter, &QAbstractItemModel::modelReset);
        filter.setSelectedTrack(QStringLiteral("Sebring"));
        filter.setSelectedDay(QStringLiteral("2026-08-30"));
        QCOMPARE(reset.size(), 0);
        QCOMPARE(filter.rowCount(), 1);
        filter.setSelectedTrack(QStringLiteral("Road America"));
        QCOMPARE(filter.rowCount(), 0);
        QCOMPARE(reset.size(), 0);
    }
};

QTEST_GUILESS_MAIN(LibraryModelTest)
#include "LibraryModelTest.moc"
