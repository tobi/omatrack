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

    void sameRecordingInTwoSectionsPreservesTheSurvivingRow() {
        const QVariantMap file{
            {QStringLiteral("role"), QStringLiteral("file")},
            {QStringLiteral("path"), QStringLiteral("/data/lap.pds")},
            {QStringLiteral("key"), QStringLiteral("/data/lap.pds")},
            {QStringLiteral("name"), QStringLiteral("lap.pds")},
        };
        const auto section = [file](const QString& path) {
            return QVariantMap{
                {QStringLiteral("role"), QStringLiteral("source")},
                {QStringLiteral("path"), path},
                {QStringLiteral("name"), path},
                {QStringLiteral("children"), QVariantList{file}},
            };
        };
        LibraryModel model;
        const auto recent = section(QStringLiteral("/recent"));
        const auto folder = section(QStringLiteral("/data"));
        model.setTree({recent, folder});
        QCOMPARE(model.rowCount(), 4);
        const QPersistentModelIndex folderFile(model.index(3));
        QSignalSpy reset(&model, &QAbstractItemModel::modelReset);
        model.setTree({folder});
        QCOMPARE(model.rowCount(), 2);
        QVERIFY(folderFile.isValid());
        QCOMPARE(folderFile.row(), 1);
        QCOMPARE(reset.size(), 0);
    }

    void arrivingSessionMetadataDoesNotReplaceTheFileRow() {
        QVariantMap file{
            {QStringLiteral("role"), QStringLiteral("file")},
            {QStringLiteral("path"), QStringLiteral("/data/lap.pds")},
            {QStringLiteral("name"), QStringLiteral("lap.pds")},
        };
        LibraryModel model;
        model.setTree({file});
        const QPersistentModelIndex row(model.index(0));
        QSignalSpy removed(&model, &QAbstractItemModel::rowsRemoved);
        file.insert(QStringLiteral("key"), QStringLiteral("loaded-session"));
        file.insert(QStringLiteral("hasSession"), true);
        model.setTree({file});
        QVERIFY(row.isValid());
        QCOMPARE(removed.size(), 0);
    }

    void duplicateListKeysDoNotMovePastTheEnd() {
        LapListModel model;
        LapRow first;
        first.lapId = 1;
        first.label = QStringLiteral("first");
        LapRow second = first;
        second.label = QStringLiteral("second");
        model.refresh({first, second});
        QCOMPARE(model.rowCount(), 2);
        model.refresh({second, first, second});
        QCOMPARE(model.rowCount(), 3);
        model.refresh({first});
        QCOMPARE(model.rowCount(), 1);
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

    void filmstripKeepsBothRolesForOneRecording() {
        FilmstripSessionListModel model;
        FilmstripSessionRow primary;
        primary.sessionKey = QStringLiteral("one-recording.mp4");
        FilmstripSessionRow reference = primary;
        reference.reference = true;
        model.refresh({primary, reference});
        QCOMPARE(model.rowCount(), 2);
        const QPersistentModelIndex active(model.index(0));
        const QPersistentModelIndex compare(model.index(1));
        QSignalSpy reset(&model, &QAbstractItemModel::modelReset);
        primary.driverName = QStringLiteral("Updated driver");
        reference.driverName = primary.driverName;
        model.refresh({primary, reference});
        QVERIFY(active.isValid());
        QVERIFY(compare.isValid());
        QVERIFY(
            !active.data(FilmstripSessionListModel::ReferenceRole).toBool());
        QVERIFY(
            compare.data(FilmstripSessionListModel::ReferenceRole).toBool());
        QCOMPARE(reset.size(), 0);
    }

    void eventFilterOwnsTrackAndDayAndRestoresManualFacets() {
        LibraryFilterModel filter;
        filter.setSelectedTrack(QStringLiteral("Sebring"));
        QVERIFY(filter.anyFilterActive());
        filter.setEventFilter(true, QStringLiteral("Daytona"),
                              QStringLiteral("2026-08-30"));
        QCOMPARE(filter.selectedTrack(), QStringLiteral("Daytona"));
        QCOMPARE(filter.selectedDay(), QStringLiteral("2026-08-30"));
        // Event settings change while on: follow them, keep the stash.
        filter.setEventFilter(true, QStringLiteral("Daytona"),
                              QStringLiteral("2026-08-31"));
        QCOMPARE(filter.selectedDay(), QStringLiteral("2026-08-31"));
        // Leaving restores the manual facets; the event's track does not
        // linger.
        filter.setEventFilter(false, QStringLiteral("Daytona"),
                              QStringLiteral("2026-08-31"));
        QCOMPARE(filter.selectedTrack(), QStringLiteral("Sebring"));
        QCOMPARE(filter.selectedDay(), QString());
        QVERIFY(!filter.eventFilterActive());
        // Clearing everything while on leaves the event filter and clears the
        // day.
        filter.setEventFilter(true, QStringLiteral("Daytona"),
                              QStringLiteral("2026-08-30"));
        filter.clearAllFilters();
        QVERIFY(!filter.eventFilterActive());
        QVERIFY(!filter.anyFilterActive());
        QCOMPARE(filter.selectedTrack(), QString());
        QCOMPARE(filter.selectedDay(), QString());
        // A later, unrelated exit must not resurrect "Sebring".
        filter.setEventFilter(true, QStringLiteral("Daytona"),
                              QStringLiteral("2026-08-30"));
        filter.setEventFilter(false, {}, {});
        QCOMPARE(filter.selectedTrack(), QString());
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

    void cornerUpdateGeometryDoesNotReset() {
        CornerListModel model;
        QVector<CornerRow> rows(2);
        rows[0].name = QStringLiteral("Turn 1");
        rows[0].start = 0.10;
        rows[0].end = 0.20;
        rows[1].name = QStringLiteral("Turn 2");
        rows[1].start = 0.40;
        rows[1].end = 0.55;
        model.refresh(rows);

        QSignalSpy reset(&model, &QAbstractItemModel::modelReset);
        QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
        model.updateGeometry(0, 0.12, 0.22);
        QCOMPARE(reset.size(), 0);
        QCOMPARE(changed.size(), 1);
        QCOMPARE(model.rowCount(), 2);
        QCOMPARE(
            model.data(model.index(0), CornerListModel::StartRole).toDouble(),
            0.12);
        QCOMPARE(
            model.data(model.index(0), CornerListModel::EndRole).toDouble(),
            0.22);
        QCOMPARE(
            model.data(model.index(1), CornerListModel::StartRole).toDouble(),
            0.40);
    }

    void cornerUpdateGeometryNoOpWhenUnchanged() {
        CornerListModel model;
        QVector<CornerRow> rows(1);
        rows[0].name = QStringLiteral("Turn 1");
        rows[0].start = 0.1;
        rows[0].end = 0.2;
        model.refresh(rows);
        QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
        model.updateGeometry(0, 0.1, 0.2);
        QCOMPARE(changed.size(), 0);
    }
};

QTEST_GUILESS_MAIN(LibraryModelTest)
#include "LibraryModelTest.moc"
