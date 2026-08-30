#include "app/LibraryModel.h"
#include "app/StoreModels.h"

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

    void setPrimarySelectLapSidebarAgree() {
        LibraryModel library;
        FilmstripSessionListModel filmstrip;
        const QString key = QStringLiteral("/tmp/indy.mp4");

        library.updateSelection(key, QString());
        FilmstripSessionRow row;
        row.sessionKey = key;
        row.reference = false;
        filmstrip.refresh({row});

        ActiveSessionRoles roles;
        roles.sessionKey = key;
        roles.videoIdentity = key;
        roles.filmstripKey = filmstrip.primarySessionKey();
        roles.sidebarKey = library.primarySessionKey();
        QVERIFY(roles.agree());

        const QString other = QStringLiteral("/tmp/other.mp4");
        library.updateSelection(other, QString());
        row.sessionKey = other;
        filmstrip.refresh({row});
        roles.sessionKey = other;
        roles.videoIdentity = other;
        roles.filmstripKey = filmstrip.primarySessionKey();
        roles.sidebarKey = library.primarySessionKey();
        QVERIFY(roles.agree());

        library.updateSelection(QString(), QString());
        filmstrip.refresh({});
        roles = ActiveSessionRoles{};
        roles.filmstripKey = filmstrip.primarySessionKey();
        roles.sidebarKey = library.primarySessionKey();
        QVERIFY(roles.agree());
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
