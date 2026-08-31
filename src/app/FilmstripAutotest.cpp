// Native, event-driven filmstrip acceptance. Compiled only with the harness.
#include "AutotestHarness.h"
#include "MpvVideoItem.h"
#include "TelemetryStore.h"
#include "LibraryModel.h"
#include "TrackMetadata.h"

#include <QFileInfo>

#include <QAbstractItemModel>
#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTimer>
#include <QUrl>
#include <algorithm>
#include <cmath>
#include <memory>

namespace {
QQuickItem* visualItem(QQuickItem* root, const QString& name) {
    if (!root) return nullptr;
    if (root->objectName() == name) return root;
    for (QQuickItem* child : root->childItems())
        if (auto* found = visualItem(child, name)) return found;
    return nullptr;
}
struct Check {
    QElapsedTimer elapsed;
    int phase = 0;
    int ticks = 0;
    QString key;
    int primary = -1;
    int reference = -1;
    int referenceAfterChange = -1;
    // Sidebar anchor: identity of the row under the top edge before a
    // rescan, so the sidebar cannot silently jump back to the top.
    QString sidebarTopIdentity;
    QString labelBeforeTrackYml;
    qreal sidebarTopOffset = 0;
    QQuickItem* strip = nullptr;
    bool viewPrepared = false;
    const omatrack::UnifiedLap* primaryData = nullptr;
    const omatrack::UnifiedLap* compareData = nullptr;
};
}  // namespace

bool omatrack::autotest::installFilmstrip(QQmlApplicationEngine& engine,
                                          TelemetryStore& store) {
    const QString source = qEnvironmentVariable("OMATRACK_AUTOTEST_FILMSTRIP");
    if (source.isEmpty()) return false;
    const QString movie =
        qEnvironmentVariable("OMATRACK_AUTOTEST_FILMSTRIP_VIDEO");
    const QString shot = qEnvironmentVariable("OMATRACK_AUTOTEST");
    const bool checkRescan =
        qEnvironmentVariableIsSet("OMATRACK_AUTOTEST_RESCAN_STATE");
    const QString pendingOpen =
        qEnvironmentVariable("OMATRACK_AUTOTEST_PENDING_OPEN");
    const bool checkRestore =
        qEnvironmentVariableIsSet("OMATRACK_AUTOTEST_RESTORE_STATE");
    const int restoredPrimary =
        qEnvironmentVariableIntValue("OMATRACK_AUTOTEST_PRIMARY_LAP");
    const int restoredReference =
        qEnvironmentVariableIntValue("OMATRACK_AUTOTEST_REFERENCE_LAP");
    // Precedence check: a TRACK.yml written into the recording's folder
    // before the rescan must win over the recording's own venue afterwards,
    // without replacing the loaded lap objects.
    const QString trackYmlName =
        qEnvironmentVariable("OMATRACK_AUTOTEST_TRACK_YML_NAME");
    auto check = std::make_shared<Check>();
    check->elapsed.start();
    auto* timer = new QTimer(&engine);
    timer->setInterval(100);
    QObject::connect(
        timer, &QTimer::timeout, &engine,
        [&engine, &store, timer, check, source, movie, shot, checkRescan,
         pendingOpen, checkRestore, restoredPrimary, restoredReference,
         trackYmlName]() {
            const auto fail = [timer](const QString& message) {
                timer->stop();
                qWarning().noquote() << "AUTOTEST filmstrip FAILED:" << message;
                QCoreApplication::exit(1);
            };
            if (check->elapsed.elapsed() >
                120000) {  // sanitizer builds are slow
                fail(QStringLiteral("timeout in phase %1").arg(check->phase));
                return;
            }
            if (engine.rootObjects().isEmpty() || store.loading() ||
                store.lapLoading())
                return;
            auto* root = engine.rootObjects().first();
            auto* window = qobject_cast<QQuickWindow*>(root);
            if (!window) {
                fail(QStringLiteral("no window"));
                return;
            }
            auto* content = window->contentItem();
            if (check->phase == 0) {
                if (checkRestore) {
                    if (!store.ready()) return;
                    if (store.primarySessionKey() != source ||
                        store.compareSessionKey() != source ||
                        store.primaryLapIndex() != restoredPrimary ||
                        store.compareLapIndex() != restoredReference ||
                        !store.primaryUnified() || !store.compareUnified()) {
                        fail(
                            QStringLiteral("startup did not restore both laps "
                                           "of the same recording"));
                        return;
                    }
                    qWarning() << "AUTOTEST startup restored both roles from "
                                  "one recording";
                } else {
                    store.openFile(source);
                }
                check->phase = 1;
                return;
            }
            if (check->phase == 1) {
                const auto* session = store.primarySession();
                if (!session || !store.primaryUnified()) return;
                check->key = store.primarySessionKey();
                check->primary = store.primaryLapIndex();
                for (const auto& lap : session->laps()) {
                    if (lap.countsForBest() && lap.lapId != check->primary) {
                        check->reference = lap.lapId;
                        break;
                    }
                }
                if (check->reference < 0) {
                    fail(QStringLiteral("need two complete laps"));
                    return;
                }
                check->phase = 2;
                store.compareLap(check->key, check->reference);
                return;
            }
            if (check->phase == 2) {
                if (!store.comparing()) return;
                check->strip =
                    visualItem(content, QStringLiteral("lapFilmstrip"));
                if (!check->strip ||
                    store.filmstripSessionsModel()->rowCount() != 2) {
                    fail(QStringLiteral(
                        "same-session comparison must show both roles"));
                    return;
                }
                if (store.primarySessionKey() != store.compareSessionKey()) {
                    fail(
                        QStringLiteral("reference was not the same recording"));
                    return;
                }
                if (!check->viewPrepared &&
                    (checkRescan || !pendingOpen.isEmpty())) {
                    store.setComparisonSyncStrategy(
                        QStringLiteral("manual-dampers"));
                    store.setReferenceAlignment(0.02);
                    store.setViewStart(0.2);
                    store.setViewEnd(0.75);
                    store.setCursorFrac(0.37);
                    check->primaryData = store.primaryUnified();
                    check->compareData = store.compareUnified();
                    check->viewPrepared = true;
                    if (checkRescan) {
                        // Scroll the sidebar so a rescan has something to
                        // lose. The fixture library is short, so a modest
                        // offset is enough to detect a jump to the top.
                        if (auto* tree =
                                visualItem(window->contentItem(),
                                           QStringLiteral("libraryTree"))) {
                            const qreal contentHeight =
                                tree->property("contentHeight").toReal();
                            const qreal target = std::max(
                                0.0,
                                std::min(60.0, contentHeight - tree->height()));
                            tree->setProperty("contentY", target);
                            int row = -1;
                            QMetaObject::invokeMethod(
                                tree, "indexAt", Q_RETURN_ARG(int, row),
                                Q_ARG(double, 1.0),
                                Q_ARG(double, target + 1.0));
                            QQuickItem* item = nullptr;
                            QMetaObject::invokeMethod(
                                tree, "itemAtIndex",
                                Q_RETURN_ARG(QQuickItem*, item),
                                Q_ARG(int, row));
                            auto* model = tree->property("model")
                                              .value<QAbstractItemModel*>();
                            if (target <= 0 || row < 0 || !item || !model) {
                                fail(QStringLiteral(
                                    "sidebar fixture too short to test scroll "
                                    "preservation"));
                                return;
                            }
                            const auto names = model->roleNames();
                            check->sidebarTopIdentity =
                                model->index(row, 0)
                                    .data(names.key("rowIdentity"))
                                    .toString();
                            check->sidebarTopOffset = target - item->y();
                        }
                        if (!trackYmlName.isEmpty()) {
                            check->labelBeforeTrackYml = store.primaryLabel();
                            const QString dir =
                                QFileInfo(source).absolutePath();
                            QString error;
                            if (!omatrack::track_metadata::update(
                                    dir,
                                    QVariantMap{
                                        {QStringLiteral("track"),
                                         QVariantMap{{QStringLiteral("name"),
                                                      trackYmlName}}}},
                                    &error)) {
                                fail(
                                    QStringLiteral(
                                        "could not write fixture TRACK.yml: ") +
                                    error);
                                return;
                            }
                        }
                        check->phase = 8;
                        check->ticks = 0;
                        store.scan();
                        return;
                    }
                }
                root->setProperty("videoVisible", true);
                root->setProperty("telemetryVideoActive", true);
                auto* player = root->findChild<MpvVideoItem*>(
                    QStringLiteral("videoPlayer"));
                if (player && !movie.isEmpty())
                    player->openMedia(QUrl::fromLocalFile(movie));
                QMetaObject::invokeMethod(root, "videoSetFullscreen",
                                          Q_ARG(QVariant, QVariant(true)));
                check->phase = 3;
                check->ticks = 0;
                return;
            }
            if (check->phase == 3) {
                auto* player = root->findChild<MpvVideoItem*>(
                    QStringLiteral("videoPlayer"));
                if (!movie.isEmpty() && (!player || !player->loaded() ||
                                         player->videoAspectRatio() <= 0))
                    return;
                if (player && player->loaded()) player->setPaused(true);
                if (++check->ticks < 8) return;
                auto* strip =
                    visualItem(content, QStringLiteral("lapFilmstrip"));
                auto* controls =
                    visualItem(content, QStringLiteral("videoControls"));
                const bool placement =
                    strip == check->strip && strip->isVisible() &&
                    strip->parentItem()->objectName() ==
                        QStringLiteral("fullscreenFilmstripSlot") &&
                    strip->height() >= 75 && controls &&
                    strip->mapToItem(content, QPointF(0, strip->height()))
                            .y() <=
                        controls->mapToItem(content, QPointF(0, 0)).y();
                if (!placement || store.primaryLapIndex() != check->primary ||
                    store.compareLapIndex() != check->reference) {
                    fail(
                        QStringLiteral("fullscreen changed strip identity, "
                                       "selection, or overlapped controls"));
                    return;
                }
                if (!window->grabWindow().save(shot)) {
                    fail(QStringLiteral("screenshot failed"));
                    return;
                }
                QMetaObject::invokeMethod(root, "videoSetFullscreen",
                                          Q_ARG(QVariant, QVariant(false)));
                check->phase = 4;
                check->ticks = 0;
                return;
            }
            if (check->phase == 4) {
                if (++check->ticks < 5) return;
                auto* strip =
                    visualItem(content, QStringLiteral("lapFilmstrip"));
                if (strip != check->strip ||
                    strip->parentItem()->objectName() !=
                        QStringLiteral("dockedFilmstripSlot")) {
                    fail(QStringLiteral("docking rebuilt the filmstrip"));
                    return;
                }
                window->grabWindow().save(shot + QStringLiteral(".docked.png"));
                auto* label =
                    visualItem(content, QStringLiteral("activeFilmstripLabel"));
                if (!label) {
                    fail(QStringLiteral("no filmstrip label"));
                    return;
                }
                const QPointF position = label->mapToItem(
                    content, QPointF(label->width() / 2, label->height() / 2));
                const QPointF global = window->mapToGlobal(position.toPoint());
                QMouseEvent press(QEvent::MouseButtonPress, position, global,
                                  Qt::RightButton, Qt::RightButton,
                                  Qt::NoModifier);
                QMouseEvent release(QEvent::MouseButtonRelease, position,
                                    global, Qt::RightButton, Qt::NoButton,
                                    Qt::NoModifier);
                QCoreApplication::sendEvent(window, &press);
                QCoreApplication::sendEvent(window, &release);
                check->phase = 5;
                check->ticks = 0;
                return;
            }
            if (check->phase == 5) {
                if (++check->ticks < 5) return;
                auto* menu = check->strip->findChild<QObject*>(
                    QStringLiteral("filmstripSwapMenu"));
                if (!menu || !menu->property("visible").toBool()) {
                    fail(QStringLiteral(
                        "label right-click did not open the swap menu"));
                    return;
                }
                QMetaObject::invokeMethod(menu, "close");
                check->phase = 6;
                check->ticks = 0;
                return;
            }
            if (check->phase == 6) {
                if (++check->ticks < 5) return;
                check->strip->forceActiveFocus();
                auto* shortcut = root->findChild<QObject*>(
                    QStringLiteral("swapReferenceShortcut"));
                if (!shortcut || !shortcut->property("enabled").toBool()) {
                    fail(QStringLiteral(
                        "X action unavailable outside an editor"));
                    return;
                }
                if (!pendingOpen.isEmpty()) {
                    // Both calls happen in this GUI event. The worker cannot
                    // deliver its result before the subsequent swap intent.
                    store.openFile(pendingOpen);
                }
                if (window->isActive()) {
                    QKeyEvent press(QEvent::KeyPress, Qt::Key_X, Qt::NoModifier,
                                    QStringLiteral("x"));
                    QKeyEvent release(QEvent::KeyRelease, Qt::Key_X,
                                      Qt::NoModifier, QStringLiteral("x"));
                    QCoreApplication::sendEvent(window, &press);
                    QCoreApplication::sendEvent(window, &release);
                } else {
                    // The headless output intentionally does not steal desktop
                    // focus. Exercise the real QML shortcut handler/action,
                    // without pretending inactive windows receive hardware
                    // keys.
                    qWarning()
                        << "AUTOTEST filmstrip: inactive headless window; "
                           "activating X action without stealing focus";
                    QMetaObject::invokeMethod(shortcut, "activated");
                }
                check->phase = 7;
                check->ticks = 0;
                return;
            }
            if (check->phase == 7) {
                if (++check->ticks < 5) return;
                if (store.primaryLapIndex() != check->reference ||
                    store.compareLapIndex() != check->primary ||
                    store.primarySessionKey() != check->key ||
                    store.compareSessionKey() != check->key ||
                    store.filmstripSessionsModel()->rowCount() != 2) {
                    fail(QStringLiteral(
                        "X did not swap the two visible lap roles"));
                    return;
                }
                if (checkRescan || !pendingOpen.isEmpty()) {
                    if (std::abs(store.referenceAlignment() + 0.02) > 1e-6 ||
                        std::abs(store.cursorFrac() - 0.35) > 1e-6 ||
                        std::abs(store.viewStart() - 0.18) > 1e-6 ||
                        std::abs(store.viewEnd() - 0.73) > 1e-6) {
                        fail(QStringLiteral(
                                 "swap did not invert manual alignment and map "
                                 "cursor/viewport to the old reference: "
                                 "alignment %1 cursor %2 view %3..%4 strategy "
                                 "%5")
                                 .arg(store.referenceAlignment())
                                 .arg(store.cursorFrac())
                                 .arg(store.viewStart())
                                 .arg(store.viewEnd())
                                 .arg(store.comparisonSyncStrategy()));
                        return;
                    }
                }
                window->grabWindow().save(shot +
                                          QStringLiteral(".swapped.png"));
                if (checkRescan || !pendingOpen.isEmpty()) {
                    for (const auto& lap : store.primarySession()->laps()) {
                        if (lap.countsForBest() &&
                            lap.lapId != store.primaryLapIndex() &&
                            lap.lapId != store.compareLapIndex()) {
                            check->referenceAfterChange = lap.lapId;
                            break;
                        }
                    }
                    if (check->referenceAfterChange < 0) {
                        fail(
                            QStringLiteral("need three complete laps to test "
                                           "alignment invalidation"));
                        return;
                    }
                    check->phase = 9;
                    store.compareLap(check->key, check->referenceAfterChange);
                    return;
                }
                timer->stop();
                qWarning() << "AUTOTEST filmstrip: same-session roles, "
                              "fullscreen, docking, label menu and swap action "
                              "passed; saved"
                           << shot;
                QCoreApplication::exit(0);
            }
            if (check->phase == 8) {
                if (store.primarySessionKey() != check->key ||
                    store.compareSessionKey() != check->key ||
                    store.primaryLapIndex() != check->primary ||
                    store.compareLapIndex() != check->reference ||
                    store.primaryUnified() != check->primaryData ||
                    store.compareUnified() != check->compareData ||
                    std::abs(store.viewStart() - 0.2) > 1e-9 ||
                    std::abs(store.viewEnd() - 0.75) > 1e-9 ||
                    std::abs(store.cursorFrac() - 0.37) > 1e-9 ||
                    std::abs(store.referenceAlignment() - 0.02) > 1e-9) {
                    fail(QStringLiteral(
                        "rescan replaced active/reference data or lost "
                        "selection, viewport, cursor or manual alignment"));
                    return;
                }
                if (!trackYmlName.isEmpty()) {
                    // The refresh runs on a worker after the scan lands.
                    if (store.primaryLabel() != trackYmlName) {
                        if (++check->ticks < 50) return;
                        fail(
                            QStringLiteral("TRACK.yml track name did not take "
                                           "precedence after rescan (%1 -> %2)")
                                .arg(check->labelBeforeTrackYml,
                                     store.primaryLabel()));
                        return;
                    }
                    // The sidebar row must agree with the header.
                    auto* library = store.libraryModel();
                    QString rowTrack;
                    for (int row = 0; library && row < library->rowCount();
                         ++row) {
                        const QModelIndex index = library->index(row, 0);
                        if (index.data(LibraryModel::PathRole).toString() ==
                            source) {
                            rowTrack =
                                index.data(LibraryModel::TrackRole).toString();
                            break;
                        }
                    }
                    if (rowTrack != trackYmlName) {
                        fail(QStringLiteral("sidebar row track '%1' disagrees "
                                            "with the header '%2'")
                                 .arg(rowTrack, trackYmlName));
                        return;
                    }
                    qWarning()
                        << "AUTOTEST rescan applied TRACK.yml precedence:"
                        << check->labelBeforeTrackYml << "->"
                        << store.primaryLabel();
                }
                if (!check->sidebarTopIdentity.isEmpty()) {
                    auto* tree = visualItem(window->contentItem(),
                                            QStringLiteral("libraryTree"));
                    auto* model = tree ? tree->property("model")
                                             .value<QAbstractItemModel*>()
                                       : nullptr;
                    if (!tree || !model) {
                        fail(QStringLiteral("sidebar tree vanished"));
                        return;
                    }
                    const qreal contentY = tree->property("contentY").toReal();
                    int row = -1;
                    QMetaObject::invokeMethod(
                        tree, "indexAt", Q_RETURN_ARG(int, row),
                        Q_ARG(double, 1.0), Q_ARG(double, contentY + 1.0));
                    QQuickItem* item = nullptr;
                    QMetaObject::invokeMethod(tree, "itemAtIndex",
                                              Q_RETURN_ARG(QQuickItem*, item),
                                              Q_ARG(int, row));
                    const QString identity =
                        row < 0
                            ? QString()
                            : model->index(row, 0)
                                  .data(model->roleNames().key("rowIdentity"))
                                  .toString();
                    if (identity != check->sidebarTopIdentity || !item ||
                        std::abs((contentY - item->y()) -
                                 check->sidebarTopOffset) > 1.0) {
                        fail(
                            QStringLiteral("sidebar scroll jumped after rescan "
                                           "(top row %1 -> %2)")
                                .arg(check->sidebarTopIdentity, identity));
                        return;
                    }
                    qWarning() << "AUTOTEST rescan kept the sidebar row under "
                                  "the top edge";
                }
                check->phase = 2;
                qWarning() << "AUTOTEST rescan preserved both loaded lap "
                              "objects and view/cursor/alignment";
            }
            if (check->phase == 9) {
                if (store.compareLapIndex() != check->referenceAfterChange ||
                    std::abs(store.referenceAlignment()) > 1e-9) {
                    fail(
                        QStringLiteral("a different lap pair inherited the old "
                                       "manual alignment"));
                    return;
                }
                timer->stop();
                qWarning() << "AUTOTEST state: scan/swap preserved intent and "
                              "a new lap pair cleared stale manual alignment";
                QCoreApplication::exit(0);
            }
        });
    timer->start();
    return true;
}
