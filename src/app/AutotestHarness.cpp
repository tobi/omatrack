// Offscreen self-test harness, armed only when OMATRACK_AUTOTEST is set.
// See AGENTS.md for the flag matrix; this is the project's acceptance surface.

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSet>
#include <QTimer>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHoverEvent>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QFileInfo>
#include <QImage>
#include <QVariantList>
#include <QVariantMap>
#include <QThread>
#include <cmath>

#include "core/TelemetryEngine.h"
#include "MpvVideoItem.h"
#include "TelemetryStore.h"
#include "TraceView.h"

#include "AutotestHarness.h"

namespace {
QStringList autotestFilePaths(TelemetryStore& store) {
    return store.libraryFilePaths();
}

// Repeater and ListView delegates are not QObject children of the view, so
// QObject::findChild() cannot reach them. Search the visual tree instead.
QQuickItem* autotestFindItem(QQuickItem* item, const QString& objectName) {
    if (!item) return nullptr;
    if (item->objectName() == objectName) return item;
    const QList<QQuickItem*> children = item->childItems();
    for (QQuickItem* child : children)
        if (QQuickItem* hit = autotestFindItem(child, objectName)) return hit;
    return nullptr;
}
}  // namespace

bool omatrack::autotest::install(QQmlApplicationEngine& engine,
                                 TelemetryStore& store) {
    const QByteArray autotestShot = qgetenv("OMATRACK_AUTOTEST");
    if (autotestShot.isEmpty()) return false;
    const bool autotestCompare =
        !qgetenv("OMATRACK_AUTOTEST_COMPARE").isEmpty();
    const bool autotestHover = !qgetenv("OMATRACK_AUTOTEST_HOVER").isEmpty();
    const bool autotestSelection =
        !qgetenv("OMATRACK_AUTOTEST_SELECTION").isEmpty();
    const bool autotestAlignment =
        !qgetenv("OMATRACK_AUTOTEST_ALIGNMENT").isEmpty();
    const bool autotestZoom = !qgetenv("OMATRACK_AUTOTEST_ZOOM").isEmpty();
    const bool autotestCorner = !qgetenv("OMATRACK_AUTOTEST_CORNER").isEmpty();
    const bool autotestConfidence =
        !qgetenv("OMATRACK_AUTOTEST_CONFIDENCE").isEmpty();
    const bool autotestVideoHud =
        !qgetenv("OMATRACK_AUTOTEST_VIDEO_HUD").isEmpty();
    const bool autotestRename = !qgetenv("OMATRACK_AUTOTEST_RENAME").isEmpty();
    const bool autotestBrakeSync =
        !qgetenv("OMATRACK_AUTOTEST_BRAKE_SYNC").isEmpty();
    const bool autotestCornerEdit =
        !qgetenv("OMATRACK_AUTOTEST_CORNER_EDIT").isEmpty();
    const bool autotestDualVideo =
        !qgetenv("OMATRACK_AUTOTEST_DUAL_VIDEO").isEmpty();
    const bool autotestStandaloneVideo =
        !qgetenv("OMATRACK_AUTOTEST_STANDALONE_VIDEO").isEmpty();
    const bool autotestVideoMetadata =
        !qgetenv("OMATRACK_AUTOTEST_VIDEO_METADATA").isEmpty();
    const bool autotestChannelBrowser =
        !qgetenv("OMATRACK_AUTOTEST_CHANNEL_BROWSER").isEmpty();
    const bool autotestWindows =
        !qgetenv("OMATRACK_AUTOTEST_WINDOWS").isEmpty();
    const bool autotestLoading =
        !qgetenv("OMATRACK_AUTOTEST_LOADING").isEmpty();
    const bool autotestLapLoading =
        !qgetenv("OMATRACK_AUTOTEST_LAP_LOADING").isEmpty();
    const bool autotestLapSwitch =
        !qgetenv("OMATRACK_AUTOTEST_LAP_SWITCH").isEmpty();
    const bool autotestIndexedVideoClick =
        !qgetenv("OMATRACK_AUTOTEST_INDEXED_VIDEO_CLICK").isEmpty();
    const QString startupVideoPath = qEnvironmentVariable("OMATRACK_VIDEO");
    const QString videoMetadataPath =
        qEnvironmentVariable("OMATRACK_AUTOTEST_VIDEO_METADATA") ==
                QStringLiteral("1")
            ? startupVideoPath
            : qEnvironmentVariable("OMATRACK_AUTOTEST_VIDEO_METADATA");
    const QString secondVideoPath =
        qEnvironmentVariable("OMATRACK_AUTOTEST_SECOND_VIDEO");
    if (!secondVideoPath.isEmpty()) store.openFile(secondVideoPath);
    const auto sequentialVideoReady = [&store, startupVideoPath,
                                       secondVideoPath]() {
        return secondVideoPath.isEmpty() ||
               (store.primaryVideoSource().toLocalFile() == startupVideoPath &&
                store.compareVideoSource().toLocalFile() == secondVideoPath);
    };
    const auto videoShortcutReady = std::make_shared<bool>(
        startupVideoPath.isEmpty() || autotestBrakeSync || autotestDualVideo);
    const QString shotPath = QString::fromUtf8(autotestShot);
    if (autotestLoading) {
        // The startup scan may already be over by the time QML is up, so a
        // fresh scan is requested right before the frame is captured: scan()
        // raises `loading` synchronously and the QML bindings follow it.
        QTimer::singleShot(0, &engine, [&store, &engine, shotPath]() {
            if (!store.loading()) store.scan();
            if (engine.rootObjects().isEmpty()) {
                qWarning() << "AUTOTEST loading: no root";
                qApp->exit(1);
                return;
            }
            QObject* root = engine.rootObjects().first();
            QObject* indicator = root->findChild<QObject*>(
                QStringLiteral("sessionLoadingIndicator"));
            auto* window = qobject_cast<QQuickWindow*>(root);
            const bool loadingReady = store.loading() && indicator &&
                                      indicator->property("visible").toBool() &&
                                      indicator->property("running").toBool() &&
                                      window;
            const QImage image = window ? window->grabWindow() : QImage();
            const bool saved = !image.isNull() && image.save(shotPath);
            qWarning() << "AUTOTEST loading:" << loadingReady
                       << "saved:" << saved << image.size();
            qApp->exit(loadingReady && saved ? 0 : 1);
        });
        return true;
    }
    // Regression guard for the folder/video metadata driver editors: typing a
    // name must not lose focus. The rows are a plain JS array, so a model
    // binding that rebuilds delegates per keystroke silently swallows every
    // character after the first.
    const QString driverTypingPath =
        qEnvironmentVariable("OMATRACK_AUTOTEST_DRIVER_TYPING");
    if (!driverTypingPath.isEmpty()) {
        auto* typingTimer = new QTimer(&engine);
        typingTimer->setInterval(200);
        QObject::connect(
            typingTimer, &QTimer::timeout, &engine,
            [typingTimer, &store, &engine, shotPath, driverTypingPath]() {
                if (store.loading() || !store.ready()) return;
                typingTimer->stop();
                typingTimer->deleteLater();
                QObject* root = engine.rootObjects().isEmpty()
                                    ? nullptr
                                    : engine.rootObjects().first();
                QObject* dialog =
                    root ? root->findChild<QObject*>(
                               QStringLiteral("videoMetadataDialog"))
                         : nullptr;
                auto* window = qobject_cast<QQuickWindow*>(root);
                if (!dialog || !window) {
                    qWarning() << "AUTOTEST driver typing: no dialog";
                    qApp->exit(1);
                    return;
                }
                const auto settle = [](int ms) {
                    QElapsedTimer timer;
                    timer.start();
                    while (timer.elapsed() < ms) {
                        QCoreApplication::processEvents(QEventLoop::AllEvents,
                                                        10);
                        QThread::msleep(5);
                    }
                };
                const bool opened = QMetaObject::invokeMethod(
                    dialog, "openForFolder", Q_ARG(QString, driverTypingPath));
                settle(400);
                // A fresh row is the one a user edits after "+ Add Driver
                // ID = Name"; inherited rows may not exist for every folder.
                const bool added =
                    QMetaObject::invokeMethod(dialog, "addDriverMapping");
                settle(400);
                if (!opened || !added) {
                    qWarning() << "AUTOTEST driver typing: dialog refused"
                               << opened << added;
                    qApp->exit(1);
                    return;
                }

                // Delegate items are not QObject children of the Repeater, so
                // findChild() cannot see them; walk the visual tree instead.
                QQuickItem* editor = autotestFindItem(
                    window->contentItem(), QStringLiteral("driverNameEditor0"));
                if (!editor) {
                    qWarning() << "AUTOTEST driver typing: no editor";
                    qApp->exit(1);
                    return;
                }
                const QString before = editor->property("text").toString();
                editor->forceActiveFocus();
                settle(100);
                const QString typed = QStringLiteral("Ana");
                for (const QChar letter : typed) {
                    QKeyEvent press(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier,
                                    QString(letter));
                    QCoreApplication::sendEvent(window, &press);
                    QKeyEvent release(QEvent::KeyRelease, Qt::Key_A,
                                      Qt::NoModifier, QString(letter));
                    QCoreApplication::sendEvent(window, &release);
                    settle(60);
                }
                QObject* focused = qGuiApp->focusObject();
                QQuickItem* editorNow = autotestFindItem(
                    window->contentItem(), QStringLiteral("driverNameEditor0"));
                const QString after =
                    editorNow ? editorNow->property("text").toString()
                              : QString();
                const bool focusKept = focused == editor && editorNow == editor;
                const bool textKept = after == before + typed;
                qWarning() << "AUTOTEST driver typing: focus kept:" << focusKept
                           << "text:" << after
                           << "expected:" << (before + typed);

                // A suggestion chip must reach the editor through the model,
                // not by assigning text and breaking its binding.
                QQuickItem* chip =
                    autotestFindItem(window->contentItem(),
                                     QStringLiteral("driverNameSuggestion0_0"));
                if (!chip) {
                    qWarning() << "AUTOTEST driver typing: no suggestion chip";
                    qApp->exit(1);
                    return;
                }
                const QString suggested = chip->property("text").toString();
                QMetaObject::invokeMethod(chip, "clicked");
                settle(200);
                QQuickItem* editorAfterChip = autotestFindItem(
                    window->contentItem(), QStringLiteral("driverNameEditor0"));
                const bool suggestionApplied =
                    editorAfterChip &&
                    editorAfterChip->property("text").toString() == suggested;
                qWarning() << "AUTOTEST driver suggestion:" << suggestionApplied
                           << suggested;

                // The preferences drivers list is the other place a driver
                // name is typed; it must survive the same keystrokes.
                auto* settings =
                    qobject_cast<QQuickWindow*>(root->findChild<QObject*>(
                        QStringLiteral("settingsWindow")));
                QObject* page = root->findChild<QObject*>(
                    QStringLiteral("preferencesDriversPage"));
                QAbstractItemModel* mappings = store.driverMappingsModel();
                if (!settings || !page || !mappings ||
                    mappings->rowCount() == 0) {
                    qWarning() << "AUTOTEST driver typing: no preferences page";
                    qApp->exit(1);
                    return;
                }
                settings->setProperty("currentSection", 1);
                settings->show();
                settle(300);
                const QModelIndex firstIdx = mappings->index(0, 0);
                const QString firstKey =
                    firstIdx.data(DriverMappingModel::KeyRole).toString();
                const QString firstDisplay =
                    firstIdx.data(DriverMappingModel::DisplayRole).toString();
                QMetaObject::invokeMethod(page, "beginEdit",
                                          Q_ARG(QVariant, firstKey),
                                          Q_ARG(QVariant, firstDisplay));
                settle(300);
                QQuickItem* rename =
                    autotestFindItem(settings->contentItem(),
                                     QStringLiteral("driverRenameEditor0"));
                if (!rename) {
                    qWarning() << "AUTOTEST driver typing: no rename editor";
                    qApp->exit(1);
                    return;
                }
                const QString renameBefore =
                    rename->property("text").toString();
                rename->forceActiveFocus();
                settle(100);
                for (const QChar letter : typed) {
                    QKeyEvent press(QEvent::KeyPress, Qt::Key_A, Qt::NoModifier,
                                    QString(letter));
                    QCoreApplication::sendEvent(settings, &press);
                    QKeyEvent release(QEvent::KeyRelease, Qt::Key_A,
                                      Qt::NoModifier, QString(letter));
                    QCoreApplication::sendEvent(settings, &release);
                    settle(60);
                }
                QQuickItem* renameNow =
                    autotestFindItem(settings->contentItem(),
                                     QStringLiteral("driverRenameEditor0"));
                const QString renameAfter =
                    renameNow ? renameNow->property("text").toString()
                              : QString();
                const bool renameFocusKept =
                    qGuiApp->focusObject() == rename && renameNow == rename;
                const bool renameTextKept = renameAfter == renameBefore + typed;
                qWarning() << "AUTOTEST preferences rename: focus kept:"
                           << renameFocusKept << "text:" << renameAfter
                           << "expected:" << (renameBefore + typed);
                const QImage image = window->grabWindow();
                const bool saved = !image.isNull() && image.save(shotPath);
                qApp->exit(focusKept && textKept && suggestionApplied &&
                                   renameFocusKept && renameTextKept && saved
                               ? 0
                               : 1);
            });
        typingTimer->start();
        return true;
    }
    auto* startTimer = new QTimer(&engine);
    startTimer->setInterval(200);
    QObject::connect(
        startTimer, &QTimer::timeout, &engine,
        [startTimer, &store, &engine, shotPath, startupVideoPath,
         autotestCompare, autotestWindows, autotestHover, autotestSelection,
         autotestAlignment, autotestZoom, autotestCorner, autotestConfidence,
         autotestRename, autotestVideoHud, autotestBrakeSync,
         autotestCornerEdit, autotestDualVideo, autotestVideoMetadata,
         autotestChannelBrowser, videoMetadataPath, autotestLapLoading,
         autotestStandaloneVideo, autotestIndexedVideoClick, autotestLapSwitch,
         secondVideoPath, sequentialVideoReady, videoShortcutReady]() {
            if (store.loading() || store.lapLoading() || !store.ready()) return;
            if (autotestIndexedVideoClick && !startupVideoPath.isEmpty()) {
                startTimer->stop();
                startTimer->deleteLater();
                QObject* root = engine.rootObjects().isEmpty()
                                    ? nullptr
                                    : engine.rootObjects().first();
                if (root)
                    QMetaObject::invokeMethod(
                        root, "setSessionActive",
                        Q_ARG(QVariant, QVariant(startupVideoPath)));
                auto* indexedVideoTimer = new QTimer(&engine);
                indexedVideoTimer->setInterval(100);
                QObject::connect(
                    indexedVideoTimer, &QTimer::timeout, &engine,
                    [indexedVideoTimer, &store, &engine, shotPath,
                     startupVideoPath]() {
                        if (store.lapLoading()) return;
                        indexedVideoTimer->stop();
                        indexedVideoTimer->deleteLater();
                        QObject* root = engine.rootObjects().isEmpty()
                                            ? nullptr
                                            : engine.rootObjects().first();
                        auto* window = qobject_cast<QQuickWindow*>(root);
                        const bool selected =
                            store.primarySessionKey() == startupVideoPath &&
                            !store.lapRowsForSession(startupVideoPath)
                                 .isEmpty() &&
                            !store.primaryVideoSource().isEmpty();
                        const QImage image =
                            window ? window->grabWindow() : QImage();
                        const bool saved =
                            !image.isNull() && image.save(shotPath);
                        qWarning()
                            << "AUTOTEST indexed video click:" << selected
                            << "saved:" << saved << image.size();
                        qApp->exit(selected && saved ? 0 : 1);
                    });
                indexedVideoTimer->start();
                return;
            }
            const QStringList keys = store.sessionKeys();
            if (keys.isEmpty()) {
                const QStringList paths = autotestFilePaths(store);
                const int index =
                    startTimer->property("autotestFileOpenIndex").toInt();
                if (index < paths.size()) {
                    startTimer->setProperty("autotestFileOpenIndex", index + 1);
                    store.openFile(paths.at(index));
                    return;
                }
            }
            startTimer->stop();
            startTimer->deleteLater();
            for (const QString& key : keys) {
                if (!secondVideoPath.isEmpty() &&
                    key != store.primarySessionKey())
                    continue;
                const int best = store.bestLapIdForSession(key);
                if (best >= 0) {
                    store.selectLap(key, best);
                    if (autotestLapLoading) {
                        QObject* root = engine.rootObjects().isEmpty()
                                            ? nullptr
                                            : engine.rootObjects().first();
                        QObject* indicator =
                            root ? root->findChild<QObject*>(
                                       QStringLiteral("lapLoadingIndicator"))
                                 : nullptr;
                        auto* window = qobject_cast<QQuickWindow*>(root);
                        const bool loadingReady =
                            store.lapLoading() && indicator &&
                            indicator->property("visible").toBool() &&
                            indicator->property("running").toBool() && window;
                        const QImage image =
                            window ? window->grabWindow() : QImage();
                        const bool saved =
                            !image.isNull() && image.save(shotPath);
                        qWarning() << "AUTOTEST lap loading:" << loadingReady
                                   << "saved:" << saved << image.size();
                        qApp->exit(loadingReady && saved ? 0 : 1);
                        return;
                    }
                    QElapsedTimer lapLoadTimer;
                    lapLoadTimer.start();
                    while (store.lapLoading() &&
                           lapLoadTimer.elapsed() < 30000) {
                        QCoreApplication::processEvents(QEventLoop::AllEvents,
                                                        20);
                        QThread::msleep(5);
                    }
                    if (autotestLapSwitch) {
                        // Simulate a user clicking through laps in
                        // the filmstrip: select 3-5 representative
                        // laps in the same session, waiting for each
                        // to load. This exercises the async lap-
                        // loading path, viewport/trace rebuild, and
                        // lap strip update.
                        const QVector<LapRow> allLaps =
                            store.lapRowsForSession(key);
                        QList<int> lapIds;
                        for (const LapRow& lm : allLaps)
                            if (lm.countsForBest) lapIds.append(lm.lapId);
                        // Select up to 5 laps, skipping the already-
                        // loaded fastest lap (lap 0 of the list).
                        int switched = 0;
                        const int lapCount = int(lapIds.size());
                        for (int idx = std::min(1, lapCount - 1);
                             idx < lapCount && switched < 5; ++idx) {
                            store.selectLap(key, lapIds[idx]);
                            QElapsedTimer switchTimer;
                            switchTimer.start();
                            while (store.lapLoading() &&
                                   switchTimer.elapsed() < 30000) {
                                QCoreApplication::processEvents(
                                    QEventLoop::AllEvents, 20);
                                QThread::msleep(5);
                            }
                            if (store.lapLoading()) break;
                            ++switched;
                        }
                        qWarning() << "AUTOTEST lap switch: switched"
                                   << switched << "laps in session" << key;
                    }
                    if (autotestStandaloneVideo &&
                        !startupVideoPath.isEmpty()) {
                        store.openFile(startupVideoPath);
                        QElapsedTimer standaloneTimer;
                        standaloneTimer.start();
                        while (store.lapLoading() &&
                               standaloneTimer.elapsed() < 30000) {
                            QCoreApplication::processEvents(
                                QEventLoop::AllEvents, 20);
                            QThread::msleep(5);
                        }
                        QObject* root = engine.rootObjects().isEmpty()
                                            ? nullptr
                                            : engine.rootObjects().first();
                        QObject* tracePane =
                            root ? root->findChild<QObject*>(
                                       QStringLiteral("tracePane"))
                                 : nullptr;
                        QObject* videoPane =
                            root ? root->findChild<QObject*>(
                                       QStringLiteral("videoPane"))
                                 : nullptr;
                        QObject* seekSlider =
                            root ? root->findChild<QObject*>(
                                       QStringLiteral("videoSeekSlider"))
                                 : nullptr;
                        QObject* seekBack =
                            root ? root->findChild<QObject*>(
                                       QStringLiteral("videoSeekBackButton"))
                                 : nullptr;
                        QObject* seekForward =
                            root ? root->findChild<QObject*>(
                                       QStringLiteral("videoSeekForwardButton"))
                                 : nullptr;
                        const bool standaloneReady =
                            root && tracePane && videoPane && seekSlider &&
                            seekBack && seekForward &&
                            store.primarySessionKey().isEmpty() &&
                            store.compareSessionKey().isEmpty() &&
                            root->property("standaloneVideoActive").toBool() &&
                            !tracePane->property("visible").toBool() &&
                            videoPane->property("visible").toBool() &&
                            seekSlider->property("visible").toBool() &&
                            seekBack->property("visible").toBool() &&
                            seekForward->property("visible").toBool();
                        if (root)
                            root->setProperty("standaloneVideoAutotestReady",
                                              standaloneReady);
                        qWarning()
                            << "AUTOTEST standalone video:" << standaloneReady;
                    }
                    if (!startupVideoPath.isEmpty() &&
                        !store.primaryVideoSource().isEmpty())
                        store.setCursorFrac(0.5);
                    if (autotestBrakeSync) {
                        const omatrack::UnifiedLap* lap =
                            store.primaryUnified();
                        if (lap && lap->brake.size() > 1) {
                            for (size_t sample = 1; sample < lap->brake.size();
                                 ++sample) {
                                if (lap->brake[sample] >= 10.0 &&
                                    lap->brake[sample - 1] < 5.0) {
                                    store.setCursorFrac(
                                        double(sample) /
                                        double(lap->brake.size() - 1));
                                    break;
                                }
                            }
                        }
                    }
                    const QString folderMetadataPath = qEnvironmentVariable(
                        "OMATRACK_AUTOTEST_FOLDER_"
                        "METADATA");
                    if (((autotestVideoMetadata &&
                          !videoMetadataPath.isEmpty()) ||
                         !folderMetadataPath.isEmpty()) &&
                        !engine.rootObjects().isEmpty()) {
                        QObject* dialog =
                            engine.rootObjects().first()->findChild<QObject*>(
                                QStringLiteral("videoMetadataDialog"));
                        if (dialog && !folderMetadataPath.isEmpty())
                            QMetaObject::invokeMethod(
                                dialog, "openForFolder",
                                Q_ARG(QString, folderMetadataPath));
                        else if (dialog)
                            QMetaObject::invokeMethod(
                                dialog, "openForVideo",
                                Q_ARG(QString, videoMetadataPath));
                        if (autotestChannelBrowser)
                            QTimer::singleShot(300, &engine, [&engine]() {
                                if (engine.rootObjects().isEmpty()) return;
                                QObject* field =
                                    engine.rootObjects()
                                        .first()
                                        ->findChild<QObject*>(
                                            QStringLiteral("driverChannelMappi"
                                                           "ng"));
                                if (field)
                                    QMetaObject::invokeMethod(field,
                                                              "browseChannels");
                            });
                    }
                    if (autotestRename && !engine.rootObjects().isEmpty()) {
                        QObject* root = engine.rootObjects().first();
                        QMetaObject::invokeMethod(
                            root, "openDriverRename",
                            Q_ARG(QVariant, store.primaryDriverMappingKey()),
                            Q_ARG(QVariant, store.primaryDriverName()));
                        auto* field = root->findChild<QObject*>(
                            QStringLiteral("driverRenameField"));
                        auto* dialog = root->findChild<QObject*>(
                            QStringLiteral("driverRenameDialog"));
                        if (field && dialog) {
                            field->setProperty(
                                "text", QStringLiteral("Autotest Driver"));
                            QMetaObject::invokeMethod(dialog, "accept");
                        }
                    }
                    bool comparisonSelected = !secondVideoPath.isEmpty();
                    if (autotestCompare && secondVideoPath.isEmpty()) {
                        for (const QString& otherKey : keys) {
                            if (comparisonSelected) break;
                            if (otherKey == key) continue;
                            const QVector<LapRow> otherLaps =
                                store.lapRowsForSession(otherKey);
                            for (const LapRow& otherLap : otherLaps) {
                                if (!otherLap.isFastest) continue;
                                // The two loads must not race:
                                // let the active lap settle
                                // before the reference is
                                // queued, exactly as a user's
                                // two clicks would.
                                store.selectLap(otherKey, otherLap.lapId);
                                QElapsedTimer settle;
                                settle.start();
                                while (store.lapLoading() &&
                                       settle.elapsed() < 30000) {
                                    QCoreApplication::processEvents(
                                        QEventLoop::AllEvents, 20);
                                    QThread::msleep(5);
                                }
                                store.compareLap(key, best);
                                comparisonSelected = true;
                                break;
                            }
                        }
                    }
                    if (!comparisonSelected && !autotestStandaloneVideo)
                        store.clearCompare();
                    QElapsedTimer compareLoadTimer;
                    compareLoadTimer.start();
                    while (store.lapLoading() &&
                           compareLoadTimer.elapsed() < 30000) {
                        QCoreApplication::processEvents(QEventLoop::AllEvents,
                                                        20);
                        QThread::msleep(5);
                    }
                    if (autotestCompare)
                        qWarning()
                            << "AUTOTEST comparison:" << store.comparing()
                            << "reference" << store.compareSessionKey();
                    // Never clobber Track Atlas ranges or a
                    // saved omatrack.yml override with
                    // brake-zone guesses.
                    if (!autotestStandaloneVideo && store.corners().isEmpty())
                        store.autoGenerateCorners();
                    if (autotestCornerEdit && !engine.rootObjects().isEmpty()) {
                        QObject* root = engine.rootObjects().first();
                        const int before = store.corners().size();
                        const int added = store.addCorner(0.94, 0.98);
                        const bool addedReady =
                            added >= 0 && store.corners().size() == before + 1;
                        store.setCornerName(added,
                                            QStringLiteral("Autotest Kink"));
                        const bool renamedReady =
                            added >= 0 && store.corners().value(added).name ==
                                              QStringLiteral("Autotest Kink");
                        store.deleteCorner(added);
                        // A right-click must open the Material
                        // menu: a QWidget QMenu would abort
                        // this process.
                        bool menuReady = false;
                        if (auto* trace = root->findChild<TraceView*>(
                                QStringLiteral("traceView"))) {
                            if (trace->width() > 0 && trace->height() > 0) {
                                const QPointF spot(trace->width() * 0.5,
                                                   trace->height() * 0.5);
                                QMouseEvent press(
                                    QEvent::MouseButtonPress, spot, spot, spot,
                                    Qt::RightButton, Qt::RightButton,
                                    Qt::NoModifier);
                                auto popupVisible = [&](const char* name) {
                                    QCoreApplication::sendEvent(trace, &press);
                                    // Opening runs through
                                    // the event loop;
                                    // sample after it
                                    // settles.
                                    QCoreApplication::processEvents();
                                    QObject* popup = root->findChild<QObject*>(
                                        QLatin1String(name));
                                    const bool shown =
                                        popup &&
                                        popup->property("visible").toBool();
                                    if (popup)
                                        QMetaObject::invokeMethod(popup,
                                                                  "close");
                                    QCoreApplication::processEvents();
                                    return shown;
                                };
                                store.setEditingCorners(false);
                                const bool channelReady =
                                    popupVisible("channelMenu");
                                store.setEditingCorners(true);
                                const bool cornerReady =
                                    popupVisible("cornerMenu");
                                qWarning()
                                    << "AUTOTEST channel menu:" << channelReady
                                    << "corner menu:" << cornerReady;
                                menuReady = channelReady && cornerReady;
                            }
                        }
                        qWarning() << "AUTOTEST corner menu:" << menuReady;
                        root->setProperty("cornerEditAutotestReady",
                                          addedReady && renamedReady &&
                                              menuReady &&
                                              store.corners().size() == before);
                    }
                    if (autotestAlignment && comparisonSelected)
                        store.setReferenceAlignment(0.02);
                    if (autotestSelection && !engine.rootObjects().isEmpty()) {
                        auto* trace =
                            engine.rootObjects().first()->findChild<TraceView*>(
                                QStringLiteral("traceView"));
                        if (trace && trace->width() > 0 &&
                            trace->height() > 0) {
                            const QPointF start(trace->width() * 0.28,
                                                trace->height() * 0.35);
                            const QPointF finish(trace->width() * 0.62,
                                                 trace->height() * 0.35);
                            // Left-drag is the range gesture now:
                            // press, move with the button held, then
                            // release away from the press point.
                            QMouseEvent begin(QEvent::MouseButtonPress, start,
                                              start, start, Qt::LeftButton,
                                              Qt::LeftButton, Qt::NoModifier);
                            QCoreApplication::sendEvent(trace, &begin);
                            QMouseEvent drag(QEvent::MouseMove, finish, finish,
                                             finish, Qt::NoButton,
                                             Qt::LeftButton, Qt::NoModifier);
                            QCoreApplication::sendEvent(trace, &drag);
                            QMouseEvent release(QEvent::MouseButtonRelease,
                                                finish, finish, finish,
                                                Qt::LeftButton, Qt::NoButton,
                                                Qt::NoModifier);
                            QCoreApplication::sendEvent(trace, &release);
                        }
                    }
                    if (autotestCorner && !engine.rootObjects().isEmpty()) {
                        auto* root = engine.rootObjects().first();
                        auto* trace = root->findChild<TraceView*>(
                            QStringLiteral("traceView"));
                        const auto& corners = store.corners();
                        const double beforeStart = store.viewStart();
                        const double beforeEnd = store.viewEnd();
                        if (trace && !corners.isEmpty()) {
                            const double middle =
                                (corners.first().start + corners.first().end) *
                                0.5;
                            const double labelWidth = trace->labelWidth();
                            const QPointF position(
                                labelWidth +
                                    middle * std::max(1.0, trace->width() -
                                                               labelWidth),
                                10.0);
                            QMouseEvent click(QEvent::MouseButtonPress,
                                              position, position, position,
                                              Qt::LeftButton, Qt::LeftButton,
                                              Qt::NoModifier);
                            QCoreApplication::sendEvent(trace, &click);
                            QMouseEvent release(QEvent::MouseButtonRelease,
                                                position, position, position,
                                                Qt::LeftButton, Qt::NoButton,
                                                Qt::NoModifier);
                            QCoreApplication::sendEvent(trace, &release);
                        }
                        // The framing is set synchronously by the
                        // click; later benchmarks move the viewport,
                        // so measure it now. Only the overlay fade
                        // and the restore need the event loop.
                        const double span = store.viewEnd() - store.viewStart();
                        const auto& zones = store.corners();
                        const double mid =
                            zones.isEmpty()
                                ? 0.5
                                : (zones.first().start + zones.first().end) *
                                      0.5;
                        const double centre =
                            span > 0.0 ? (mid - store.viewStart()) / span
                                       : -1.0;
                        const bool zoomed = span < (beforeEnd - beforeStart) &&
                                            centre > 0.0 && centre < 0.5;
                        QTimer::singleShot(
                            300, &engine,
                            [&engine, &store, root, shotPath, centre, zoomed,
                             beforeStart, beforeEnd]() {
                                QCoreApplication::processEvents();
                                const int focused = store.focusedCorner();
                                QObject* overlay = root->findChild<QObject*>(
                                    QStringLiteral("cornerFocusOverlay"));
                                const bool overlayVisible =
                                    overlay &&
                                    overlay->property("visible").toBool();

                                // The frame a user actually sees on
                                // click: the neighbouring lap has not
                                // loaded yet, so the space past the
                                // lap bounds must read as black.
                                if (auto* window =
                                        qobject_cast<QQuickWindow*>(root)) {
                                    const QFileInfo info(shotPath);
                                    const QString focusShot =
                                        info.path() + "/" +
                                        info.completeBaseName() + "_focus." +
                                        info.suffix();
                                    const QImage image = window->grabWindow();
                                    qWarning()
                                        << "AUTOTEST corner focus "
                                           "frame:"
                                        << image.save(focusShot) << focusShot;
                                }
                                store.clearCornerFocus();
                                const bool restored =
                                    store.focusedCorner() < 0 &&
                                    std::abs(store.viewStart() - beforeStart) <
                                        1e-6 &&
                                    std::abs(store.viewEnd() - beforeEnd) <
                                        1e-6;
                                qWarning()
                                    << "AUTOTEST corner focus:" << focused
                                    << "overlay:" << overlayVisible
                                    << "zoomed:" << zoomed << centre
                                    << "restored:" << restored;
                                // Leave the workspace focused and a
                                // marker hovered so the screenshot
                                // shows the state under test.
                                store.focusCorner(focused);
                                const double cursorBeforeHover =
                                    store.cursorFrac();
                                bool hoverPreservedCursor = false;
                                auto* view = root->findChild<TraceView*>(
                                    QStringLiteral("traceView"));
                                const QVector<CornerMarker>& markers =
                                    store.cornerMarkers();
                                if (view && !markers.isEmpty()) {
                                    const double lw = view->labelWidth();
                                    const double x =
                                        lw +
                                        (markers.last().fraction -
                                         store.viewStart()) /
                                            std::max(1e-6,
                                                     store.viewEnd() -
                                                         store.viewStart()) *
                                            std::max(1.0, view->width() - lw);
                                    const QPointF spot(x,
                                                       view->height() - 22.0);
                                    // A plain move (no button) is the
                                    // pointer path a real hover takes
                                    // through the item.
                                    QMouseEvent move(QEvent::MouseMove, spot,
                                                     spot, spot, Qt::NoButton,
                                                     Qt::NoButton,
                                                     Qt::NoModifier);
                                    QCoreApplication::sendEvent(view, &move);
                                    QCoreApplication::processEvents();
                                    hoverPreservedCursor =
                                        std::abs(store.cursorFrac() -
                                                 cursorBeforeHover) < 1e-6;
                                }
                                qWarning() << "AUTOTEST trace hover preserves "
                                              "cursor:"
                                           << hoverPreservedCursor;
                                root->setProperty(
                                    "cornerFocusAutotestReady",
                                    focused >= 0 && overlayVisible && zoomed &&
                                        restored && hoverPreservedCursor);
                            });
                    }
                    if (autotestConfidence) {
                        QTimer::singleShot(6500, &engine, [&engine, &store]() {
                            if (engine.rootObjects().isEmpty()) return;
                            QObject* root = engine.rootObjects().first();
                            auto* trace = root->findChild<TraceView*>(
                                QStringLiteral("traceView"));
                            auto* button = root->findChild<QQuickItem*>(
                                QStringLiteral("confidenceButton"));
                            if (button) {
                                const QPointF center(button->width() * 0.5,
                                                     button->height() * 0.5);
                                QMouseEvent press(
                                    QEvent::MouseButtonPress, center, center,
                                    center, Qt::LeftButton, Qt::LeftButton,
                                    Qt::NoModifier);
                                QCoreApplication::sendEvent(button, &press);
                                QMouseEvent release(
                                    QEvent::MouseButtonRelease, center, center,
                                    center, Qt::LeftButton, Qt::NoButton,
                                    Qt::NoModifier);
                                QCoreApplication::sendEvent(button, &release);
                                QCoreApplication::processEvents();
                            }
                            const bool buttonReady =
                                button &&
                                button->property("checked").toBool() &&
                                store.traceConfidenceMode();
                            if (trace) {
                                trace->forceActiveFocus();
                                QCoreApplication::processEvents();
                                QKeyEvent press(QEvent::KeyPress,
                                                Qt::Key_Period, Qt::NoModifier);
                                QCoreApplication::sendEvent(trace, &press);
                            }
                            root->setProperty("confidenceButtonAutotestReady",
                                              buttonReady);
                            root->setProperty(
                                "confidencePressAutotestReady",
                                trace && store.traceConfidenceMode());
                            qWarning()
                                << "AUTOTEST confidence button:" << buttonReady
                                << "key press:"
                                << root->property(
                                           "confidencePressAutotest"
                                           "Ready")
                                       .toBool();
                        });
                    }
                    if (autotestVideoHud && !engine.rootObjects().isEmpty()) {
                        // The telemetry HUD only exists over
                        // fullscreen video; put it on screen so its
                        // scene-graph geometry is exercised.
                        QObject* root = engine.rootObjects().first();
                        root->setProperty("videoOverlayVisible", true);
                        QMetaObject::invokeMethod(root, "videoSetFullscreen",
                                                  Q_ARG(QVariant, true));
                    }
                    if (autotestHover && !engine.rootObjects().isEmpty()) {
                        auto* overlay = engine.rootObjects()
                                            .first()
                                            ->findChild<TraceCursorOverlay*>(
                                                QStringLiteral("traceOverlay"));
                        if (overlay && overlay->width() > 0 &&
                            overlay->height() > 0) {
                            // The overlay is scene-graph geometry: the
                            // measurable per-frame cost is building
                            // it, the GPU draws it in one batch.
                            constexpr int frames = 120;
                            for (int frame = 0; frame < frames; ++frame)
                                store.setCursorFrac(double(frame) /
                                                    (frames - 1));
                            const QVariantMap result =
                                overlay->benchmarkGeometry(frames);
                            qWarning()
                                << "AUTOTEST hover overlay "
                                   "average_ms:"
                                << result.value("averageMs").toDouble()
                                << "quads:" << result.value("quads").toInt();
                        }
                    }
                    if (autotestZoom && !engine.rootObjects().isEmpty()) {
                        auto* trace =
                            engine.rootObjects().first()->findChild<TraceView*>(
                                QStringLiteral("traceView"));
                        if (trace && trace->width() > 0 &&
                            trace->height() > 0) {
                            const double wheelSpanBefore = store.viewSpan();
                            const QPointF wheelPosition(trace->width() * 0.5,
                                                        trace->height() * 0.5);
                            QWheelEvent wheel(wheelPosition, wheelPosition,
                                              QPoint(), QPoint(0, 120),
                                              Qt::NoButton, Qt::NoModifier,
                                              Qt::ScrollUpdate, false);
                            QGuiApplication::sendEvent(trace, &wheel);
                            const bool wheelZoomReady =
                                store.viewSpan() < wheelSpanBefore;
                            engine.rootObjects().first()->setProperty(
                                "wheelZoomAutotestReady", wheelZoomReady);
                            qWarning()
                                << "AUTOTEST wheel zoom:" << wheelZoomReady;
                            store.setViewStart(0.0);
                            store.setViewEnd(1.0);
                            QElapsedTimer clock;
                            clock.start();
                            constexpr int frames = 80;
                            double worstMs = 0.0;
                            int quads = 0;
                            int lanes = 0;
                            for (int frame = 0; frame < frames; ++frame) {
                                const double phase =
                                    double(frame) / double(frames - 1);
                                const double span =
                                    0.20 +
                                    0.65 * (0.5 + 0.5 * std::sin(phase *
                                                                 6.283185307));
                                const double start = (1.0 - span) * phase;
                                store.setViewStart(start);
                                store.setViewEnd(start + span);
                                const QVariantMap result =
                                    trace->benchmarkGeometry(1);
                                worstMs = std::max(
                                    worstMs,
                                    result.value("averageMs").toDouble());
                                quads = result.value("quads").toInt();
                                lanes = result.value("lanes").toInt();
                            }
                            qWarning()
                                << "AUTOTEST zoom geometry "
                                   "average_ms:"
                                << (clock.nsecsElapsed() / 1.0e6) / frames
                                << "worst_ms:" << worstMs << "quads:" << quads
                                << "lanes:" << lanes;
                            store.setViewStart(0.0);
                            store.setViewEnd(1.0);
                        }
                    }
                    if (!startupVideoPath.isEmpty() &&
                        !store.primaryVideoSource().isEmpty() &&
                        !autotestBrakeSync && !autotestDualVideo) {
                        auto* keyTimer = new QTimer(&engine);
                        keyTimer->setInterval(50);
                        QObject::connect(
                            keyTimer, &QTimer::timeout, &engine,
                            [keyTimer, &engine, videoShortcutReady]() {
                                if (engine.rootObjects().isEmpty()) return;
                                QObject* root = engine.rootObjects().first();
                                auto* video = root->findChild<MpvVideoItem*>(
                                    QStringLiteral("videoPlayer"));
                                // After the app's own seek to the telemetry
                                // cursor has landed, or the skips below start
                                // from 0 s and drag the cursor to lap start.
                                if (!video || !video->loaded() ||
                                    video->exactSeekCount() < 1 ||
                                    video->seeking())
                                    return;
                                auto activate =
                                    [root](
                                        const QString& objectName,
                                        const QKeySequence& expectedSequence) {
                                        QObject* shortcut =
                                            root->findChild<QObject*>(
                                                objectName);
                                        return shortcut &&
                                               shortcut->property("enabled")
                                                   .toBool() &&
                                               shortcut->property("sequence")
                                                       .value<QKeySequence>() ==
                                                   expectedSequence &&
                                               QMetaObject::invokeMethod(
                                                   shortcut, "activated");
                                    };
                                const int seekCount = video->exactSeekCount();
                                const bool paused = video->paused();
                                const bool backward = activate(
                                    QStringLiteral("videoSeekBackwardShortcut"),
                                    QKeySequence(Qt::Key_Left));
                                const bool forward = activate(
                                    QStringLiteral("videoSeekForwardShortcut"),
                                    QKeySequence(Qt::Key_Right));
                                const bool pauseToggle = activate(
                                    QStringLiteral("videoPauseShortcut"),
                                    QKeySequence(Qt::Key_Space));
                                const bool pauseChanged =
                                    video->paused() != paused;
                                bool resumed = true;
                                if (video->paused())
                                    resumed = activate(
                                        QStringLiteral("videoPauseShortcut"),
                                        QKeySequence(Qt::Key_Space));
                                *videoShortcutReady =
                                    forward && backward && pauseToggle &&
                                    pauseChanged && resumed &&
                                    video->exactSeekCount() >= seekCount + 2 &&
                                    !video->paused();
                                qWarning() << "AUTOTEST video shortcuts:"
                                           << *videoShortcutReady;
                                keyTimer->stop();
                                keyTimer->deleteLater();
                            });
                        keyTimer->start();
                    }
                    if (autotestDualVideo) {
                        auto* playbackTimer = new QTimer(&engine);
                        playbackTimer->setInterval(50);
                        QObject::connect(
                            playbackTimer, &QTimer::timeout, &engine,
                            [playbackTimer, &engine, &store]() {
                                if (engine.rootObjects().isEmpty()) return;
                                QObject* root = engine.rootObjects().first();
                                auto* primary = root->findChild<MpvVideoItem*>(
                                    QStringLiteral("videoPlayer"));
                                auto* reference =
                                    root->findChild<MpvVideoItem*>(
                                        QStringLiteral("videoPlayerReferen"
                                                       "ce"));
                                if (!primary || !reference ||
                                    !primary->loaded() || !reference->loaded())
                                    return;
                                root->setProperty("dualCursorBaseline",
                                                  store.cursorFrac());
                                if (primary->paused())
                                    QMetaObject::invokeMethod(
                                        root, "videoTogglePaused");
                                playbackTimer->stop();
                                playbackTimer->deleteLater();
                                QTimer::singleShot(500, &engine, [&engine]() {
                                    if (engine.rootObjects().isEmpty()) return;
                                    QObject* root =
                                        engine.rootObjects().first();
                                    auto* reference =
                                        root->findChild<MpvVideoItem*>(
                                            QStringLiteral("videoPlaye"
                                                           "rReferen"
                                                           "c"
                                                           "e"));
                                    if (reference)
                                        root->setProperty(
                                            "dualPauseSeekBasel"
                                            "ine",
                                            reference->exactSeekCount());
                                });
                            });
                        playbackTimer->start();
                        // Reparent the live OpenGL video stage
                        // into fullscreen and back before final
                        // assertions.
                        QTimer::singleShot(4000, &engine, [&engine]() {
                            for (QObject* root : engine.rootObjects())
                                QMetaObject::invokeMethod(
                                    root, "videoSetFullscreen",
                                    Q_ARG(QVariant, QVariant(true)));
                        });
                        QTimer::singleShot(4700, &engine, [&engine]() {
                            for (QObject* root : engine.rootObjects())
                                QMetaObject::invokeMethod(
                                    root, "videoSetFullscreen",
                                    Q_ARG(QVariant, QVariant(false)));
                        });
                        // Pausing is an intentional hard-sync point:
                        // the reference must exact-seek through the
                        // selected shared alignment map, then playback
                        // resumes without periodic seeks.
                        QTimer::singleShot(5200, &engine, [&engine]() {
                            if (engine.rootObjects().isEmpty()) return;
                            QObject* root = engine.rootObjects().first();
                            auto* reference = root->findChild<MpvVideoItem*>(
                                QStringLiteral("videoPlayerReference"));
                            const int baseline =
                                root->property("dualPauseSeekBaseline").toInt();
                            root->setProperty(
                                "dualContinuousBeforePause",
                                reference && baseline >= 0 &&
                                    reference->exactSeekCount() == baseline);
                            root->setProperty(
                                "dualPauseSeekBaseline",
                                reference ? reference->exactSeekCount() : -1);
                            QMetaObject::invokeMethod(root,
                                                      "videoTogglePaused");
                        });
                        QTimer::singleShot(5700, &engine, [&engine, &store]() {
                            if (engine.rootObjects().isEmpty()) return;
                            QObject* root = engine.rootObjects().first();
                            auto* primary = root->findChild<MpvVideoItem*>(
                                QStringLiteral("videoPlayer"));
                            auto* reference = root->findChild<MpvVideoItem*>(
                                QStringLiteral("videoPlayerReference"));
                            const int pauseBaseline =
                                root->property("dualPauseSeekBaseline").toInt();
                            const double error =
                                reference
                                    ? std::abs(store.compareVideoTime() -
                                               reference->position())
                                    : std::numeric_limits<double>::infinity();
                            const bool pausedAligned =
                                primary && reference && primary->paused() &&
                                reference->paused() && pauseBaseline >= 0 &&
                                reference->exactSeekCount() > pauseBaseline &&
                                error <= 0.05 &&
                                root->property("dualContinuousBeforePause")
                                    .toBool();
                            root->setProperty("dualPauseAlignmentReady",
                                              pausedAligned);
                            qWarning()
                                << "AUTOTEST dual pause alignment:"
                                << pausedAligned << "error" << error
                                << "exact seeks"
                                << (reference ? reference->exactSeekCount() -
                                                    pauseBaseline
                                              : -1);
                            QMetaObject::invokeMethod(root,
                                                      "videoTogglePaused");
                            root->setProperty(
                                "dualReferenceSeekBaseline",
                                reference ? reference->exactSeekCount() : -1);
                        });
                    }
                    const int finalDelay = autotestDualVideo    ? 7000
                                           : autotestConfidence ? 8000
                                                                : 2500;
                    // The final assertions run `finalDelay` after the video
                    // has loaded, not after startup: an exact seek into a
                    // multi-gigabyte recording plus decoder start-up can eat
                    // a fixed window before the first frame plays, and that
                    // is mpv's load time, not a sync error. Capped so a video
                    // that never loads still fails instead of hanging.
                    const auto finalAssertions = [&store, &engine, shotPath,
                                                  startupVideoPath,
                                                  autotestWindows,
                                                  autotestRename,
                                                  autotestBrakeSync,
                                                  autotestCornerEdit,
                                                  autotestCorner,
                                                  autotestConfidence,
                                                  autotestVideoHud,
                                                  autotestZoom,
                                                  autotestDualVideo,
                                                  autotestStandaloneVideo,
                                                  sequentialVideoReady,
                                                  videoShortcutReady]() {
                        QList<QQuickWindow*> windows;
                        for (QObject* root : engine.rootObjects()) {
                            if (auto* window =
                                    qobject_cast<QQuickWindow*>(root))
                                windows.append(window);
                            if (autotestWindows) {
                                for (QQuickWindow* child :
                                     root->findChildren<QQuickWindow*>())
                                    if (!windows.contains(child))
                                        windows.append(child);
                            }
                        }
                        bool videoReady = startupVideoPath.isEmpty();
                        if (autotestStandaloneVideo) {
                            QObject* root = engine.rootObjects().isEmpty()
                                                ? nullptr
                                                : engine.rootObjects().first();
                            videoReady = root && root->property(
                                                         "standaloneVideoAut"
                                                         "otestReady")
                                                     .toBool();
                        } else if (!videoReady) {
                            for (QObject* root : engine.rootObjects()) {
                                auto* video = root->findChild<MpvVideoItem*>(
                                    QStringLiteral("videoPl"
                                                   "ayer"));
                                if (!video || !video->ready() ||
                                    !video->loaded() ||
                                    video->duration() <= 0.0)
                                    continue;
                                videoReady =
                                    std::abs(video->volume() - 75.0) <= 0.01;
                                if (!store.primaryVideoSource().isEmpty()) {
                                    const double target =
                                        store.primaryVideoTime();
                                    const double error =
                                        std::abs(video->position() - target);
                                    qWarning() << "AUTO"
                                                  "TEST"
                                                  " vid"
                                                  "eo "
                                                  "sync"
                                                  ":"
                                               << video->position()
                                               << "targ"
                                                  "et"
                                               << target
                                               << "erro"
                                                  "r"
                                               << error;
                                    videoReady = videoReady && error <= 0.1;
                                    const omatrack::UnifiedLap* lap =
                                        store.primaryUnified();
                                    if (autotestBrakeSync) {
                                        // Scan the brake channel
                                        // for the peak pressure
                                        // sample so the check
                                        // pauses on real heavy
                                        // braking, not whatever
                                        // cursor position the
                                        // video happened to land
                                        // on.
                                        size_t peakSample = 0;
                                        double peakBrake = 0.0;
                                        if (lap && !lap->brake.empty()) {
                                            for (size_t i = 0;
                                                 i < lap->brake.size(); ++i) {
                                                if (lap->brake[i] > peakBrake) {
                                                    peakBrake = lap->brake[i];
                                                    peakSample = i;
                                                }
                                            }
                                            // Seek cursor to
                                            // the peak brake
                                            // sample. Guard
                                            // against size()==1
                                            // to avoid 0/0 NaN.
                                            const double frac =
                                                lap->brake.size() > 1
                                                    ? double(peakSample) /
                                                          double(lap->brake
                                                                     .size() -
                                                                 1)
                                                    : 0.0;
                                            store.setCursorFrac(frac);
                                        }
                                        const size_t sample =
                                            lap && !lap->brake.empty()
                                                ? std::min(
                                                      size_t(std::llround(
                                                          store.cursorFrac() *
                                                          double(lap->brake
                                                                     .size() -
                                                                 1))),
                                                      lap->brake.size() - 1)
                                                : 0;
                                        const bool brakeReady =
                                            lap && !lap->brake.empty() &&
                                            video->paused() &&
                                            lap->brake[sample] >= 10.0;
                                        qWarning() << "AUTOTEST brake "
                                                      "sync:"
                                                   << brakeReady << "lap time"
                                                   << (lap && !lap->time.empty()
                                                           ? lap->time[sample]
                                                           : -1.0)
                                                   << "peak brake" << peakBrake
                                                   << "at sample" << peakSample;
                                        videoReady = videoReady && brakeReady;
                                    } else {
                                        const double baseline =
                                            autotestDualVideo
                                                ? root->property(
                                                          "dualCurs"
                                                          "or"
                                                          "Baselin"
                                                          "e")
                                                      .toDouble()
                                                : 0.5;
                                        const double playbackStart =
                                            lap && !lap->time.empty()
                                                ? baseline +
                                                      (autotestDualVideo
                                                           ? 2.0
                                                           : 1.0) /
                                                          lap->time.back()
                                                : 1.0;
                                        videoReady =
                                            videoReady &&
                                            (autotestVideoHud ||
                                             (!video->paused() &&
                                              store.cursorFrac() >
                                                  playbackStart + 0.002));
                                    }
                                }
                                qWarning() << "AUTOTEST"
                                              " video "
                                              "volume:"
                                           << video->volume();
                                break;
                            }
                            if (!videoReady)
                                qWarning() << "AUTOTEST"
                                              " video "
                                              "failed "
                                              "to "
                                              "load or "
                                              "sync";
                        }
                        if (autotestVideoHud) {
                            QObject* root = engine.rootObjects().isEmpty()
                                                ? nullptr
                                                : engine.rootObjects().first();
                            auto* overlay =
                                root ? root->findChild<QQuickItem*>(
                                           QStringLiteral("videoTelemetryOverl"
                                                          "ay"))
                                     : nullptr;
                            QQuickItem* parent =
                                overlay ? overlay->parentItem() : nullptr;
                            const double unscaledWidth =
                                parent ? std::min(
                                             {parent->width() - 16.0, 1000.0,
                                              std::max(520.0,
                                                       parent->width() * 0.72)})
                                       : 0.0;
                            const double expectedWidth =
                                std::max(0.0, unscaledWidth * 0.65);
                            const double expectedHeight = expectedWidth * 0.21;
                            const double expectedX =
                                parent ? (parent->width() - expectedWidth) * 0.5
                                       : 0.0;
                            const double expectedY =
                                parent ? std::max(
                                             0.0,
                                             std::min(parent->height() -
                                                          expectedHeight,
                                                      parent->height() * 0.9 -
                                                          expectedHeight * 0.5))
                                       : 0.0;
                            const bool hudReady =
                                overlay && parent && overlay->isVisible() &&
                                std::abs(overlay->width() - expectedWidth) <=
                                    1.0 &&
                                std::abs(overlay->height() - expectedHeight) <=
                                    1.0 &&
                                std::abs(overlay->x() - expectedX) <= 1.0 &&
                                std::abs(overlay->y() - expectedY) <= 1.0;
                            qWarning()
                                << "AUTOTEST video HUD layout:" << hudReady
                                << "size"
                                << QSizeF(overlay ? overlay->width() : 0.0,
                                          overlay ? overlay->height() : 0.0)
                                << "position"
                                << QPointF(overlay ? overlay->x() : 0.0,
                                           overlay ? overlay->y() : 0.0);
                            videoReady = videoReady && hudReady;
                        }
                        if (windows.isEmpty())
                            qWarning() << "AUTOTEST "
                                          "found no "
                                          "QQuickWindo"
                                          "w";

                        bool renameReady = true;
                        if (autotestRename) {
                            QObject* root = engine.rootObjects().isEmpty()
                                                ? nullptr
                                                : engine.rootObjects().first();
                            renameReady = root &&
                                          root->findChild<QObject*>(
                                              QStringLiteral("headerD"
                                                             "riverEd"
                                                             "it")) &&
                                          store.driverDisplayName(
                                              store.primarySessionKey()) ==
                                              QStringLiteral(
                                                  "Autotes"
                                                  "t "
                                                  "Drive"
                                                  "r");
                            qWarning() << "AUTOTEST "
                                          "driver "
                                          "rename:"
                                       << renameReady;
                        }
                        qWarning() << "AUTOTEST gps "
                                      "available:"
                                   << store.hasGpsData();
                        bool dualVideoReady = true;
                        if (autotestDualVideo) {
                            QObject* root = engine.rootObjects().isEmpty()
                                                ? nullptr
                                                : engine.rootObjects().first();
                            auto* primary =
                                root ? root->findChild<MpvVideoItem*>(
                                           QStringLiteral("v"
                                                          "i"
                                                          "d"
                                                          "e"
                                                          "o"
                                                          "P"
                                                          "l"
                                                          "a"
                                                          "y"
                                                          "e"
                                                          "r"))
                                     : nullptr;
                            auto* reference =
                                root ? root->findChild<MpvVideoItem*>(
                                           QStringLiteral("v"
                                                          "i"
                                                          "d"
                                                          "e"
                                                          "o"
                                                          "P"
                                                          "l"
                                                          "a"
                                                          "y"
                                                          "e"
                                                          "r"
                                                          "R"
                                                          "e"
                                                          "f"
                                                          "e"
                                                          "r"
                                                          "e"
                                                          "n"
                                                          "c"
                                                          "e"))
                                     : nullptr;
                            const double target = store.compareVideoTime();
                            const double error =
                                reference
                                    ? std::abs(reference->position() - target)
                                    : -1.0;
                            auto* controls = root
                                                 ? root->findChild<QQuickItem*>(
                                                       QStringLiteral("v"
                                                                      "i"
                                                                      "d"
                                                                      "e"
                                                                      "o"
                                                                      "C"
                                                                      "o"
                                                                      "n"
                                                                      "t"
                                                                      "r"
                                                                      "o"
                                                                      "l"
                                                                      "s"))
                                                 : nullptr;
                            const bool fullscreenRestored =
                                root && !root->property(
                                                 "vi"
                                                 "de"
                                                 "oF"
                                                 "ul"
                                                 "ls"
                                                 "cr"
                                                 "ee"
                                                 "n")
                                             .toBool();
                            const bool chromeReady =
                                controls &&
                                controls->findChild<QQuickItem*>(
                                    QStringLiteral("videoPl"
                                                   "ayPause"
                                                   "Butto"
                                                   "n")) &&
                                root->findChild<QQuickItem*>(
                                    QStringLiteral("videoMu"
                                                   "teButto"
                                                   "n")) &&
                                controls->findChild<QQuickItem*>(
                                    QStringLiteral("videoFu"
                                                   "llscree"
                                                   "nButto"
                                                   "n"));
                            const int seekBaseline = root ? root->property(
                                                                    "d"
                                                                    "u"
                                                                    "a"
                                                                    "l"
                                                                    "R"
                                                                    "e"
                                                                    "f"
                                                                    "e"
                                                                    "r"
                                                                    "e"
                                                                    "n"
                                                                    "c"
                                                                    "e"
                                                                    "S"
                                                                    "e"
                                                                    "e"
                                                                    "k"
                                                                    "B"
                                                                    "a"
                                                                    "s"
                                                                    "e"
                                                                    "l"
                                                                    "i"
                                                                    "n"
                                                                    "e")
                                                                .toInt()
                                                          : -1;
                            const bool continuousPlayback =
                                reference && seekBaseline >= 0 &&
                                reference->exactSeekCount() == seekBaseline;
                            const bool pauseAlignmentReady =
                                root &&
                                root->property("dualPauseAlignmentReady")
                                    .toBool();
                            dualVideoReady =
                                root &&
                                root->property(
                                        "dualVid"
                                        "eo")
                                    .toBool() &&
                                primary && primary->ready() &&
                                primary->loaded() &&
                                primary->duration() > 0.0 && reference &&
                                reference->ready() && reference->loaded() &&
                                reference->duration() > 0.0 &&
                                primary->source() != reference->source() &&
                                reference->muted() && !primary->paused() &&
                                !reference->paused() && error <= 0.15 &&
                                pauseAlignmentReady && continuousPlayback &&
                                !store.comparisonAlignmentBasis().isEmpty() &&
                                fullscreenRestored && chromeReady &&
                                sequentialVideoReady();
                            qWarning()
                                << "AUTOTEST "
                                   "dual video:"
                                << dualVideoReady << "primary"
                                << (primary ? primary->source().fileName()
                                            : QString())
                                << (primary ? primary->position() : -1.0)
                                << "reference"
                                << (reference ? reference->source().fileName()
                                              : QString())
                                << (reference ? reference->position() : -1.0)
                                << "target" << target << "error" << error
                                << "loaded" << (primary && primary->loaded())
                                << (reference && reference->loaded())
                                << "paused"
                                << (primary ? primary->paused() : false)
                                << (reference ? reference->paused() : false)
                                << "store"
                                << store.primaryVideoSource().fileName()
                                << store.compareVideoSource().fileName();
                            qWarning() << "AUTOTEST "
                                          "dual sync "
                                          "model:"
                                       << store.comparisonAlignmentBasis()
                                       << "gps anchors"
                                       << store.comparisonGpsAnchors() << "rate"
                                       << (reference ? reference->playbackRate()
                                                     : -1.0)
                                       << "seeks "
                                          "during "
                                          "playback"
                                       << (reference && seekBaseline >= 0
                                               ? reference->exactSeekCount() -
                                                     seekBaseline
                                               : -1);
                        }
                        bool cornerMutationReady = true;
                        if (autotestCornerEdit) {
                            QObject* root = engine.rootObjects().isEmpty()
                                                ? nullptr
                                                : engine.rootObjects().first();
                            cornerMutationReady =
                                root && root->property(
                                                "cornerEditAutotest"
                                                "Ready")
                                            .toBool();
                            qWarning() << "AUTOTEST corner mutations:"
                                       << cornerMutationReady;
                        }
                        const bool cornerFocusReady =
                            !autotestCorner ||
                            (!engine.rootObjects().isEmpty() &&
                             engine.rootObjects()
                                 .first()
                                 ->property("cornerFocusAutotestRe"
                                            "ady")
                                 .toBool());
                        const CornerFocusSummary cornerSummary =
                            autotestCorner ? store.cornerFocusSummary()
                                           : CornerFocusSummary();
                        const bool cornerConsistencyReady =
                            !autotestCorner ||
                            (!cornerSummary.consistencyLoading &&
                             cornerSummary.consistencyLapCount >= 2 &&
                             cornerSummary.consistencyValidLapCount >= 2 &&
                             cornerSummary.brakeConsistencyAvailable);
                        if (autotestCorner)
                            qWarning()
                                << "AUTOTEST corner consistency:"
                                << cornerConsistencyReady << "laps"
                                << cornerSummary.consistencyLapCount
                                << "braking"
                                << cornerSummary.consistencyBrakeLapCount
                                << "sigma" << cornerSummary.brakePointStdDev
                                << "range" << cornerSummary.brakePointRange;
                        const bool zoomReady =
                            !autotestZoom || (!engine.rootObjects().isEmpty() &&
                                              engine.rootObjects()
                                                  .first()
                                                  ->property("wheelZoomA"
                                                             "utotestRea"
                                                             "dy")
                                                  .toBool());
                        bool confidenceReady = true;
                        TraceView* confidenceTrace = nullptr;
                        if (autotestConfidence) {
                            QObject* root = engine.rootObjects().isEmpty()
                                                ? nullptr
                                                : engine.rootObjects().first();
                            confidenceTrace =
                                root ? root->findChild<TraceView*>(
                                           QStringLiteral("traceView"))
                                     : nullptr;
                            int highlightedFilmstripLaps = 0;
                            int fixedEdgeLaps = 0;
                            bool fixedEdgeLapWidths = true;
                            auto* window = qobject_cast<QQuickWindow*>(root);
                            QQuickItem* visualRoot =
                                window ? window->contentItem() : nullptr;
                            const QVector<LapRow> activeLaps =
                                store.lapRowsForSession(
                                    store.primarySessionKey());
                            for (const LapRow& lap : activeLaps) {
                                QQuickItem* item = autotestFindItem(
                                    visualRoot,
                                    QStringLiteral("activeFilmstripLap-%1")
                                        .arg(lap.lapId));
                                if (!item) continue;
                                if (item->property("confidenceLap").toBool())
                                    ++highlightedFilmstripLaps;
                                if (item->property("fixedWidthLap").toBool()) {
                                    ++fixedEdgeLaps;
                                    fixedEdgeLapWidths &=
                                        std::abs(item->width() - 30.0) < 0.1;
                                }
                            }
                            const TraceConfidenceBand* speedBand =
                                store.traceConfidenceBand(
                                    QStringLiteral("speed"));
                            const std::vector<double>& consistency =
                                store.traceConsistency();
                            double maximumConsistency = 0.0;
                            for (const double value : consistency)
                                if (std::isfinite(value))
                                    maximumConsistency =
                                        std::max(maximumConsistency, value);
                            const UnifiedLap* primary = store.primaryUnified();
                            const bool consistencyReady =
                                primary &&
                                consistency.size() == primary->size() &&
                                maximumConsistency > 0.05 &&
                                maximumConsistency <= 1.0;
                            const QVariantMap benchmark =
                                confidenceTrace
                                    ? confidenceTrace->benchmarkGeometry(60)
                                    : QVariantMap();
                            const double averageMs =
                                benchmark.value("averageMs").toDouble();
                            confidenceReady =
                                root && confidenceTrace &&
                                root->property(
                                        "confidenceButtonAutotestRe"
                                        "ady")
                                    .toBool() &&
                                root->property(
                                        "confidencePressAutotestRea"
                                        "dy")
                                    .toBool() &&
                                store.traceConfidenceMode() &&
                                !store.traceConfidenceLoading() &&
                                store.traceConfidenceLapCount() >= 2 &&
                                speedBand && speedBand->valid() &&
                                consistencyReady &&
                                highlightedFilmstripLaps ==
                                    store.traceConfidenceLapCount() &&
                                fixedEdgeLaps >= 2 && fixedEdgeLapWidths &&
                                averageMs < 8.33;
                            qWarning()
                                << "AUTOTEST confidence overlay:"
                                << confidenceReady << "laps"
                                << store.traceConfidenceLapCount()
                                << "filmstrip highlighted"
                                << highlightedFilmstripLaps << "fixed edge laps"
                                << fixedEdgeLaps << "max consistency"
                                << maximumConsistency << "average_ms"
                                << averageMs << "quads"
                                << benchmark.value("quads").toInt();
                        }
                        const QFileInfo requested(shotPath);
                        for (QQuickWindow* window : windows) {
                            QString output = shotPath;
                            if (autotestWindows) {
                                QString suffix = requested.suffix();
                                if (!suffix.isEmpty()) suffix.prepend('.');
                                output = requested.path() + "/" +
                                         requested.completeBaseName() + "_" +
                                         (window->objectName().isEmpty()
                                              ? QStringLiteral("window")
                                              : window->objectName()) +
                                         suffix;
                            }
                            const QImage image = window->grabWindow();
                            if (image.save(output))
                                qWarning() << "AUTOTEST"
                                              " saved:"
                                           << output << image.size();
                            else
                                qWarning() << "AUTOTEST"
                                              " save "
                                              "failed:"
                                           << output;
                        }
                        if (autotestConfidence && confidenceTrace) {
                            QKeyEvent release(QEvent::KeyRelease,
                                              Qt::Key_Period, Qt::NoModifier);
                            QCoreApplication::sendEvent(confidenceTrace,
                                                        &release);
                            QCoreApplication::processEvents();
                            const bool releaseRestoredLatch =
                                store.traceConfidenceMode();
                            auto* button =
                                engine.rootObjects()
                                    .first()
                                    ->findChild<QQuickItem*>(
                                        QStringLiteral("confidenceButton"));
                            if (button) {
                                const QPointF center(button->width() * 0.5,
                                                     button->height() * 0.5);
                                QMouseEvent press(
                                    QEvent::MouseButtonPress, center, center,
                                    center, Qt::LeftButton, Qt::LeftButton,
                                    Qt::NoModifier);
                                QCoreApplication::sendEvent(button, &press);
                                QMouseEvent buttonRelease(
                                    QEvent::MouseButtonRelease, center, center,
                                    center, Qt::LeftButton, Qt::NoButton,
                                    Qt::NoModifier);
                                QCoreApplication::sendEvent(button,
                                                            &buttonRelease);
                                QCoreApplication::processEvents();
                            }
                            const bool buttonDisabled =
                                button &&
                                !button->property("checked").toBool() &&
                                !store.traceConfidenceMode();
                            confidenceReady = confidenceReady &&
                                              releaseRestoredLatch &&
                                              buttonDisabled;
                            qWarning() << "AUTOTEST confidence key "
                                          "release preserves button:"
                                       << releaseRestoredLatch
                                       << "button off:" << buttonDisabled;
                        }
                        videoReady = videoReady && renameReady &&
                                     cornerMutationReady && cornerFocusReady &&
                                     cornerConsistencyReady &&
                                     confidenceReady && zoomReady &&
                                     dualVideoReady && *videoShortcutReady;
                        const int exitCode =
                            videoReady                                     ? 0
                            : !cornerMutationReady                         ? 3
                            : !cornerFocusReady || !cornerConsistencyReady ? 4
                            : !confidenceReady                             ? 5
                                                                           : 2;
                        qApp->exit(exitCode);
                    };
                    if (startupVideoPath.isEmpty() || autotestStandaloneVideo) {
                        QTimer::singleShot(finalDelay, &engine,
                                           finalAssertions);
                    } else {
                        auto* loadedTimer = new QTimer(&engine);
                        loadedTimer->setInterval(50);
                        auto* waited = new int(0);
                        QObject::connect(
                            loadedTimer, &QTimer::timeout, &engine,
                            [loadedTimer, waited, &engine, finalDelay,
                             finalAssertions]() {
                                *waited += 50;
                                bool loaded = false;
                                for (QObject* root : engine.rootObjects()) {
                                    auto* video =
                                        root->findChild<MpvVideoItem*>(
                                            QStringLiteral("videoPlayer"));
                                    loaded = video && video->loaded();
                                    if (loaded) break;
                                }
                                if (!loaded && *waited < 20000) return;
                                if (!loaded)
                                    qWarning() << "AUTOTEST video never loaded";
                                loadedTimer->stop();
                                loadedTimer->deleteLater();
                                delete waited;
                                QTimer::singleShot(finalDelay, &engine,
                                                   finalAssertions);
                            });
                        loadedTimer->start();
                    }
                    return;
                }
            }
            qWarning() << "AUTOTEST no session found";
            qApp->exit(1);
        });
    startTimer->start();
    return true;
}
