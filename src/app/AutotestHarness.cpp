// Offscreen self-test harness, armed only when OMATRACK_AUTOTEST is set.
// See AGENTS.md for the flag matrix; this is the project's acceptance surface.

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QTimer>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHoverEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QFileInfo>
#include <QImage>
#include <QPainter>
#include <QVariantList>
#include <QVariantMap>
#include <QThread>
#include <cmath>

#include "core/TelemetryEngine.h"
#include "MpvVideoItem.h"
#include "TelemetryStore.h"
#include "TraceView.h"

#include "AutotestHarness.h"

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
    const QString shotPath = QString::fromUtf8(autotestShot);
    if (autotestLoading) {
        QTimer::singleShot(100, &engine, [&store, &engine, shotPath]() {
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
    auto* startTimer = new QTimer(&engine);
    startTimer->setInterval(200);
    QObject::connect(
        startTimer, &QTimer::timeout, &engine,
        [startTimer, &store, &engine, shotPath, startupVideoPath,
         autotestCompare, autotestWindows, autotestHover, autotestSelection,
         autotestAlignment, autotestZoom, autotestCorner, autotestRename,
         autotestBrakeSync, autotestCornerEdit, autotestDualVideo,
         autotestVideoMetadata, autotestChannelBrowser, videoMetadataPath,
         autotestLapLoading, autotestStandaloneVideo, sequentialVideoReady]() {
            if (store.loading() || store.lapLoading() || !store.ready()) return;
            startTimer->stop();
            startTimer->deleteLater();
            const QVariantList groups = store.trackGroups();
            for (const QVariant& gv : groups) {
                const QVariantMap g = gv.toMap();
                const QVariantList dates = g.value("dates").toList();
                for (const QVariant& dv : dates) {
                    const QVariantMap d = dv.toMap();
                    const QVariantList sessions = d.value("sessions").toList();
                    for (const QVariant& sv : sessions) {
                        const QVariantMap s = sv.toMap();
                        const QString key = s.value("key").toString();
                        const QVariantList laps = store.lapsForSession(key);
                        int best = -1;
                        for (const QVariant& lv : laps) {
                            const QVariantMap l = lv.toMap();
                            if (l.value("isFastest").toBool()) {
                                best = l.value("lapId").toInt();
                                break;
                            }
                        }
                        if (best >= 0) {
                            store.selectLap(key, best);
                            if (autotestLapLoading) {
                                QObject* root =
                                    engine.rootObjects().isEmpty()
                                        ? nullptr
                                        : engine.rootObjects().first();
                                QObject* indicator =
                                    root ? root->findChild<QObject*>(
                                               QStringLiteral(
                                                   "lapLoadingIndicator"))
                                         : nullptr;
                                auto* window =
                                    qobject_cast<QQuickWindow*>(root);
                                const bool loadingReady =
                                    store.lapLoading() && indicator &&
                                    indicator->property("visible").toBool() &&
                                    indicator->property("running").toBool() &&
                                    window;
                                const QImage image =
                                    window ? window->grabWindow() : QImage();
                                const bool saved =
                                    !image.isNull() && image.save(shotPath);
                                qWarning()
                                    << "AUTOTEST lap loading:" << loadingReady
                                    << "saved:" << saved << image.size();
                                qApp->exit(loadingReady && saved ? 0 : 1);
                                return;
                            }
                            QElapsedTimer lapLoadTimer;
                            lapLoadTimer.start();
                            while (store.lapLoading() &&
                                   lapLoadTimer.elapsed() < 30000) {
                                QCoreApplication::processEvents(
                                    QEventLoop::AllEvents, 20);
                                QThread::msleep(5);
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
                                QObject* root =
                                    engine.rootObjects().isEmpty()
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
                                const bool standaloneReady =
                                    root && tracePane && videoPane &&
                                    store.primarySessionKey().isEmpty() &&
                                    store.compareSessionKey().isEmpty() &&
                                    root->property("standaloneVideoActive")
                                        .toBool() &&
                                    !tracePane->property("visible").toBool() &&
                                    videoPane->property("visible").toBool();
                                if (root)
                                    root->setProperty(
                                        "standaloneVideoAutotestReady",
                                        standaloneReady);
                                qWarning() << "AUTOTEST standalone video:"
                                           << standaloneReady;
                            }
                            if (!startupVideoPath.isEmpty() &&
                                !store.primaryVideoSource().isEmpty())
                                store.setCursorFrac(0.5);
                            if (autotestBrakeSync) {
                                const omatrack::UnifiedLap* lap =
                                    store.primaryUnified();
                                if (lap && lap->brake.size() > 1) {
                                    for (size_t sample = 1;
                                         sample < lap->brake.size(); ++sample) {
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
                            const QString folderMetadataPath =
                                qEnvironmentVariable(
                                    "OMATRACK_AUTOTEST_FOLDER_"
                                    "METADATA");
                            if (((autotestVideoMetadata &&
                                  !videoMetadataPath.isEmpty()) ||
                                 !folderMetadataPath.isEmpty()) &&
                                !engine.rootObjects().isEmpty()) {
                                QObject* dialog =
                                    engine.rootObjects()
                                        .first()
                                        ->findChild<QObject*>(QStringLiteral(
                                            "videoMetadataDialog"));
                                if (dialog && !folderMetadataPath.isEmpty())
                                    QMetaObject::invokeMethod(
                                        dialog, "openForFolder",
                                        Q_ARG(QString, folderMetadataPath));
                                else if (dialog)
                                    QMetaObject::invokeMethod(
                                        dialog, "openForVideo",
                                        Q_ARG(QString, videoMetadataPath));
                                if (autotestChannelBrowser)
                                    QTimer::singleShot(
                                        300, &engine, [&engine]() {
                                            if (engine.rootObjects().isEmpty())
                                                return;
                                            QObject* field =
                                                engine.rootObjects()
                                                    .first()
                                                    ->findChild<QObject*>(
                                                        QStringLiteral(
                                                            "driverChannelMappi"
                                                            "ng"));
                                            if (field)
                                                QMetaObject::invokeMethod(
                                                    field, "browseChannels");
                                        });
                            }
                            if (autotestRename &&
                                !engine.rootObjects().isEmpty()) {
                                QObject* root = engine.rootObjects().first();
                                QMetaObject::invokeMethod(
                                    root, "openDriverRename",
                                    Q_ARG(QVariant,
                                          store.primaryDriverMappingKey()),
                                    Q_ARG(QVariant, store.primaryDriverName()));
                                auto* field = root->findChild<QObject*>(
                                    QStringLiteral("driverRenameField"));
                                auto* dialog = root->findChild<QObject*>(
                                    QStringLiteral("driverRenameDialog"));
                                if (field && dialog) {
                                    field->setProperty(
                                        "text",
                                        QStringLiteral("Autotest Driver"));
                                    QMetaObject::invokeMethod(dialog, "accept");
                                }
                            }
                            bool comparisonSelected = false;
                            if (autotestCompare) {
                                for (const QVariant& otherGroupValue : groups) {
                                    if (comparisonSelected) break;
                                    const QVariantList otherDates =
                                        otherGroupValue.toMap()
                                            .value("dates")
                                            .toList();
                                    for (const QVariant& otherDateValue :
                                         otherDates) {
                                        if (comparisonSelected) break;
                                        const QVariantList otherSessions =
                                            otherDateValue.toMap()
                                                .value("sessions")
                                                .toList();
                                        for (const QVariant& otherSessionValue :
                                             otherSessions) {
                                            const QString otherKey =
                                                otherSessionValue.toMap()
                                                    .value("key")
                                                    .toString();
                                            if (otherKey == key) continue;
                                            const QVariantList otherLaps =
                                                store.lapsForSession(otherKey);
                                            for (const QVariant& otherLapValue :
                                                 otherLaps) {
                                                const QVariantMap otherLap =
                                                    otherLapValue.toMap();
                                                if (!otherLap
                                                         .value("isFast"
                                                                "est")
                                                         .toBool())
                                                    continue;
                                                // First selected
                                                // session is the fixed
                                                // reference; second is
                                                // the active run.
                                                store.selectLap(
                                                    otherKey,
                                                    otherLap.value("lapId")
                                                        .toInt());
                                                store.compareLap(key, best);
                                                comparisonSelected = true;
                                                break;
                                            }
                                            if (comparisonSelected) break;
                                        }
                                    }
                                }
                            }
                            if (!comparisonSelected && !autotestStandaloneVideo)
                                store.clearCompare();
                            QElapsedTimer compareLoadTimer;
                            compareLoadTimer.start();
                            while (store.lapLoading() &&
                                   compareLoadTimer.elapsed() < 30000) {
                                QCoreApplication::processEvents(
                                    QEventLoop::AllEvents, 20);
                                QThread::msleep(5);
                            }
                            // Never clobber Track Atlas ranges or a
                            // saved omatrack.yml override with
                            // brake-zone guesses.
                            if (!autotestStandaloneVideo &&
                                store.cornerList().isEmpty())
                                store.autoGenerateCorners();
                            if (autotestCornerEdit &&
                                !engine.rootObjects().isEmpty()) {
                                QObject* root = engine.rootObjects().first();
                                const int before = store.cornerList().size();
                                const int added = store.addCorner(0.94, 0.98);
                                const bool addedReady =
                                    added >= 0 &&
                                    store.cornerList().size() == before + 1;
                                store.setCornerName(
                                    added, QStringLiteral("Autotest Kink"));
                                const bool renamedReady =
                                    added >= 0 &&
                                    store.cornerList()
                                            .value(added)
                                            .toMap()
                                            .value(QStringLiteral("name"))
                                            .toString() ==
                                        QStringLiteral("Autotest Kink");
                                store.deleteCorner(added);
                                // A right-click must open the Material
                                // menu: a QWidget QMenu would abort
                                // this process.
                                bool menuReady = false;
                                if (auto* trace = root->findChild<TraceView*>(
                                        QStringLiteral("traceView"))) {
                                    if (trace->width() > 0 &&
                                        trace->height() > 0) {
                                        const QPointF spot(
                                            trace->width() * 0.5,
                                            trace->height() * 0.5);
                                        QMouseEvent press(
                                            QEvent::MouseButtonPress, spot,
                                            spot, spot, Qt::RightButton,
                                            Qt::RightButton, Qt::NoModifier);
                                        auto popupVisible = [&](const char*
                                                                    name) {
                                            QCoreApplication::sendEvent(trace,
                                                                        &press);
                                            // Opening runs through
                                            // the event loop;
                                            // sample after it
                                            // settles.
                                            QCoreApplication::processEvents();
                                            QObject* popup =
                                                root->findChild<QObject*>(
                                                    QLatin1String(name));
                                            const bool shown =
                                                popup &&
                                                popup->property("visible")
                                                    .toBool();
                                            if (popup)
                                                QMetaObject::invokeMethod(
                                                    popup, "close");
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
                                            << "AUTOTEST channel menu:"
                                            << channelReady
                                            << "corner menu:" << cornerReady;
                                        menuReady = channelReady && cornerReady;
                                    }
                                }
                                qWarning()
                                    << "AUTOTEST corner menu:" << menuReady;
                                root->setProperty(
                                    "cornerEditAutotestReady",
                                    addedReady && renamedReady && menuReady &&
                                        store.cornerList().size() == before);
                                auto* window = root->findChild<QQuickWindow*>(
                                    QStringLiteral("cornerWindow"));
                                if (window) {
                                    window->show();
                                    window->requestActivate();
                                    const QString zonesShot =
                                        QFileInfo(shotPath).path() + "/" +
                                        QFileInfo(shotPath).completeBaseName() +
                                        "_zones.png";
                                    QTimer::singleShot(
                                        900, &engine, [window, zonesShot]() {
                                            const QImage image =
                                                window->grabWindow();
                                            qWarning() << "AUTOTEST zone "
                                                          "editor:"
                                                       << image.save(zonesShot)
                                                       << zonesShot;
                                        });
                                    QTimer::singleShot(1000, &engine, [root]() {
                                        QMetaObject::invokeMethod(
                                            root, "dismissCornerPopover");
                                    });
                                }
                            }
                            if (autotestAlignment && comparisonSelected)
                                store.setReferenceAlignment(0.02);
                            if (autotestSelection &&
                                !engine.rootObjects().isEmpty()) {
                                auto* trace =
                                    engine.rootObjects()
                                        .first()
                                        ->findChild<TraceView*>(
                                            QStringLiteral("traceView"));
                                if (trace && trace->width() > 0 &&
                                    trace->height() > 0) {
                                    const QPointF start(trace->width() * 0.28,
                                                        trace->height() * 0.35);
                                    const QPointF finish(
                                        trace->width() * 0.62,
                                        trace->height() * 0.35);
                                    QMouseEvent begin(
                                        QEvent::MouseButtonDblClick, start,
                                        start, start, Qt::LeftButton,
                                        Qt::LeftButton, Qt::NoModifier);
                                    QCoreApplication::sendEvent(trace, &begin);
                                    QHoverEvent move(QEvent::HoverMove, finish,
                                                     finish, start,
                                                     Qt::NoModifier);
                                    QCoreApplication::sendEvent(trace, &move);
                                    QMouseEvent commit(
                                        QEvent::MouseButtonPress, finish,
                                        finish, finish, Qt::LeftButton,
                                        Qt::LeftButton, Qt::NoModifier);
                                    QCoreApplication::sendEvent(trace, &commit);
                                    QMouseEvent release(
                                        QEvent::MouseButtonRelease, finish,
                                        finish, finish, Qt::LeftButton,
                                        Qt::NoButton, Qt::NoModifier);
                                    QCoreApplication::sendEvent(trace,
                                                                &release);
                                }
                            }
                            if (autotestCorner &&
                                !engine.rootObjects().isEmpty()) {
                                auto* trace =
                                    engine.rootObjects()
                                        .first()
                                        ->findChild<TraceView*>(
                                            QStringLiteral("traceView"));
                                const QVariantList corners = store.cornerList();
                                if (trace && !corners.isEmpty()) {
                                    const QVariantMap corner =
                                        corners.first().toMap();
                                    const double middle =
                                        (corner.value("start").toDouble() +
                                         corner.value("end").toDouble()) *
                                        0.5;
                                    const QPointF position(
                                        62.0 +
                                            middle *
                                                std::max(1.0,
                                                         trace->width() - 62.0),
                                        10.0);
                                    QMouseEvent click(
                                        QEvent::MouseButtonPress, position,
                                        position, position, Qt::LeftButton,
                                        Qt::LeftButton, Qt::NoModifier);
                                    QCoreApplication::sendEvent(trace, &click);
                                    QMouseEvent release(
                                        QEvent::MouseButtonRelease, position,
                                        position, position, Qt::LeftButton,
                                        Qt::NoButton, Qt::NoModifier);
                                    QCoreApplication::sendEvent(trace,
                                                                &release);
                                }
                            }
                            if (autotestHover &&
                                !engine.rootObjects().isEmpty()) {
                                auto* overlay =
                                    engine.rootObjects()
                                        .first()
                                        ->findChild<TraceCursorOverlay*>(
                                            QStringLiteral("traceOverlay"));
                                if (overlay && overlay->width() > 0 &&
                                    overlay->height() > 0) {
                                    QImage benchmark(
                                        QSize(int(overlay->width()),
                                              int(overlay->height())),
                                        QImage::Format_ARGB32_Premultiplied);
                                    QElapsedTimer clock;
                                    clock.start();
                                    constexpr int frames = 120;
                                    for (int frame = 0; frame < frames;
                                         ++frame) {
                                        store.setCursorFrac(double(frame) /
                                                            (frames - 1));
                                        benchmark.fill(Qt::transparent);
                                        QPainter painter(&benchmark);
                                        overlay->paint(&painter);
                                    }
                                    qWarning()
                                        << "AUTOTEST hover overlay "
                                           "average_ms:"
                                        << (clock.nsecsElapsed() / 1.0e6) /
                                               frames;
                                }
                            }
                            if (autotestZoom &&
                                !engine.rootObjects().isEmpty()) {
                                auto* trace =
                                    engine.rootObjects()
                                        .first()
                                        ->findChild<TraceView*>(
                                            QStringLiteral("traceView"));
                                if (trace && trace->width() > 0 &&
                                    trace->height() > 0) {
                                    const double wheelSpanBefore =
                                        store.viewSpan();
                                    const QPointF wheelPosition(
                                        trace->width() * 0.5,
                                        trace->height() * 0.5);
                                    QWheelEvent wheel(
                                        wheelPosition, wheelPosition, QPoint(),
                                        QPoint(0, 120), Qt::NoButton,
                                        Qt::NoModifier, Qt::ScrollUpdate,
                                        false);
                                    QGuiApplication::sendEvent(trace, &wheel);
                                    const bool wheelZoomReady =
                                        store.viewSpan() < wheelSpanBefore;
                                    engine.rootObjects().first()->setProperty(
                                        "wheelZoomAutotestReady",
                                        wheelZoomReady);
                                    qWarning() << "AUTOTEST wheel zoom:"
                                               << wheelZoomReady;
                                    store.setViewStart(0.0);
                                    store.setViewEnd(1.0);
                                    QImage benchmark(
                                        QSize(int(trace->width()),
                                              int(trace->height())),
                                        QImage::Format_ARGB32_Premultiplied);
                                    benchmark.fill(Qt::transparent);
                                    {
                                        QPainter warmup(&benchmark);
                                        trace->paint(&warmup);
                                    }
                                    QElapsedTimer clock;
                                    clock.start();
                                    constexpr int frames = 80;
                                    for (int frame = 0; frame < frames;
                                         ++frame) {
                                        const double phase =
                                            double(frame) / double(frames - 1);
                                        const double span =
                                            0.20 +
                                            0.65 *
                                                (0.5 +
                                                 0.5 * std::sin(phase *
                                                                6.283185307));
                                        const double start =
                                            (1.0 - span) * phase;
                                        store.setViewStart(start);
                                        store.setViewEnd(start + span);
                                        benchmark.fill(Qt::transparent);
                                        QPainter painter(&benchmark);
                                        trace->paint(&painter);
                                    }
                                    qWarning()
                                        << "AUTOTEST zoom paint "
                                           "average_ms:"
                                        << (clock.nsecsElapsed() / 1.0e6) /
                                               frames;
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
                                    [keyTimer, &engine]() {
                                        if (engine.rootObjects().isEmpty())
                                            return;
                                        QObject* root =
                                            engine.rootObjects().first();
                                        auto* video =
                                            root->findChild<MpvVideoItem*>(
                                                QStringLiteral("videoPlayer"));
                                        auto* window =
                                            qobject_cast<QQuickWindow*>(root);
                                        if (!video || !video->loaded() ||
                                            !window)
                                            return;
                                        for (const Qt::Key key :
                                             {Qt::Key_Right, Qt::Key_Left,
                                              Qt::Key_Space}) {
                                            QKeyEvent press(QEvent::KeyPress,
                                                            key,
                                                            Qt::NoModifier);
                                            QCoreApplication::sendEvent(window,
                                                                        &press);
                                            QKeyEvent release(
                                                QEvent::KeyRelease, key,
                                                Qt::NoModifier);
                                            QCoreApplication::sendEvent(
                                                window, &release);
                                        }
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
                                        if (engine.rootObjects().isEmpty())
                                            return;
                                        QObject* root =
                                            engine.rootObjects().first();
                                        auto* primary =
                                            root->findChild<MpvVideoItem*>(
                                                QStringLiteral("videoPlayer"));
                                        auto* reference =
                                            root->findChild<MpvVideoItem*>(
                                                QStringLiteral(
                                                    "videoPlayerReferen"
                                                    "ce"));
                                        if (!primary || !reference ||
                                            !primary->loaded() ||
                                            !reference->loaded())
                                            return;
                                        root->setProperty("dualCursorBaseline",
                                                          store.cursorFrac());
                                        if (primary->paused())
                                            QMetaObject::invokeMethod(
                                                root, "videoTogglePaused");
                                        playbackTimer->stop();
                                        playbackTimer->deleteLater();
                                        QTimer::singleShot(
                                            500, &engine, [&engine]() {
                                                if (engine.rootObjects()
                                                        .isEmpty())
                                                    return;
                                                QObject* root =
                                                    engine.rootObjects()
                                                        .first();
                                                auto* reference =
                                                    root->findChild<
                                                        MpvVideoItem*>(
                                                        QStringLiteral(
                                                            "videoPlaye"
                                                            "rReferen"
                                                            "c"
                                                            "e"));
                                                if (reference)
                                                    root->setProperty(
                                                        "dualPauseSeekBasel"
                                                        "ine",
                                                        reference
                                                            ->exactSeekCount());
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
                                // shared GPS/speed station map, then playback
                                // resumes without periodic seeks.
                                QTimer::singleShot(5200, &engine, [&engine]() {
                                    if (engine.rootObjects().isEmpty()) return;
                                    QObject* root =
                                        engine.rootObjects().first();
                                    auto* reference =
                                        root->findChild<MpvVideoItem*>(
                                            QStringLiteral(
                                                "videoPlayerReference"));
                                    const int baseline =
                                        root->property("dualPauseSeekBaseline")
                                            .toInt();
                                    root->setProperty(
                                        "dualContinuousBeforePause",
                                        reference && baseline >= 0 &&
                                            reference->exactSeekCount() ==
                                                baseline);
                                    root->setProperty(
                                        "dualPauseSeekBaseline",
                                        reference ? reference->exactSeekCount()
                                                  : -1);
                                    QMetaObject::invokeMethod(
                                        root, "videoTogglePaused");
                                });
                                QTimer::singleShot(
                                    5700, &engine, [&engine, &store]() {
                                        if (engine.rootObjects().isEmpty())
                                            return;
                                        QObject* root =
                                            engine.rootObjects().first();
                                        auto* primary =
                                            root->findChild<MpvVideoItem*>(
                                                QStringLiteral("videoPlayer"));
                                        auto* reference =
                                            root->findChild<MpvVideoItem*>(
                                                QStringLiteral(
                                                    "videoPlayerReference"));
                                        const int pauseBaseline =
                                            root->property(
                                                    "dualPauseSeekBaseline")
                                                .toInt();
                                        const double error =
                                            reference
                                                ? std::abs(
                                                      store.compareVideoTime() -
                                                      reference->position())
                                                : std::numeric_limits<
                                                      double>::infinity();
                                        const bool pausedAligned =
                                            primary && reference &&
                                            primary->paused() &&
                                            reference->paused() &&
                                            pauseBaseline >= 0 &&
                                            reference->exactSeekCount() >
                                                pauseBaseline &&
                                            error <= 0.05 &&
                                            root->property(
                                                    "dualContinuousBeforePause")
                                                .toBool();
                                        root->setProperty(
                                            "dualPauseAlignmentReady",
                                            pausedAligned);
                                        qWarning()
                                            << "AUTOTEST dual pause alignment:"
                                            << pausedAligned << "error" << error
                                            << "exact seeks"
                                            << (reference
                                                    ? reference
                                                              ->exactSeekCount() -
                                                          pauseBaseline
                                                    : -1);
                                        QMetaObject::invokeMethod(
                                            root, "videoTogglePaused");
                                        root->setProperty(
                                            "dualReferenceSeekBaseline",
                                            reference
                                                ? reference->exactSeekCount()
                                                : -1);
                                    });
                            }
                            QTimer::singleShot(
                                autotestDualVideo ? 7000 : 2500, &engine,
                                [&store, &engine, shotPath, startupVideoPath,
                                 autotestWindows, autotestRename,
                                 autotestBrakeSync, autotestCornerEdit,
                                 autotestZoom, autotestDualVideo,
                                 autotestStandaloneVideo,
                                 sequentialVideoReady]() {
                                    QList<QQuickWindow*> windows;
                                    for (QObject* root : engine.rootObjects()) {
                                        if (auto* window =
                                                qobject_cast<QQuickWindow*>(
                                                    root))
                                            windows.append(window);
                                        if (autotestWindows) {
                                            for (QQuickWindow* child :
                                                 root->findChildren<
                                                     QQuickWindow*>())
                                                if (!windows.contains(child))
                                                    windows.append(child);
                                        }
                                    }
                                    bool videoReady =
                                        startupVideoPath.isEmpty();
                                    if (autotestStandaloneVideo) {
                                        QObject* root =
                                            engine.rootObjects().isEmpty()
                                                ? nullptr
                                                : engine.rootObjects().first();
                                        videoReady =
                                            root && root->property(
                                                            "standaloneVideoAut"
                                                            "otestReady")
                                                        .toBool();
                                    } else if (!videoReady) {
                                        for (QObject* root :
                                             engine.rootObjects()) {
                                            auto* video =
                                                root->findChild<MpvVideoItem*>(
                                                    QStringLiteral("videoPl"
                                                                   "ayer"));
                                            if (!video || !video->ready() ||
                                                !video->loaded() ||
                                                video->duration() <= 0.0)
                                                continue;
                                            videoReady =
                                                std::abs(video->volume() -
                                                         75.0) <= 0.01;
                                            if (!store.primaryVideoSource()
                                                     .isEmpty()) {
                                                const double target =
                                                    store.primaryVideoTime();
                                                const double error = std::abs(
                                                    video->position() - target);
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
                                                videoReady =
                                                    videoReady && error <= 0.1;
                                                const omatrack::UnifiedLap*
                                                    lap =
                                                        store.primaryUnified();
                                                if (autotestBrakeSync) {
                                                    const size_t sample =
                                                        lap && !lap->brake
                                                                    .empty()
                                                            ? std::min(
                                                                  size_t(std::llround(
                                                                      store
                                                                          .cursorFrac() *
                                                                      double(
                                                                          lap->brake
                                                                              .size() -
                                                                          1))),
                                                                  lap->brake
                                                                          .size() -
                                                                      1)
                                                            : 0;
                                                    const bool brakeReady =
                                                        lap &&
                                                        !lap->brake.empty() &&
                                                        video->paused() &&
                                                        lap->brake[sample] >=
                                                            10.0;
                                                    qWarning()
                                                        << "AUTOTEST brake "
                                                           "sync:"
                                                        << brakeReady
                                                        << "lap time"
                                                        << (lap && !lap->time
                                                                        .empty()
                                                                ? lap->time
                                                                      [sample]
                                                                : -1.0);
                                                    videoReady = videoReady &&
                                                                 brakeReady;
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
                                                        lap && !lap->time
                                                                    .empty()
                                                            ? baseline +
                                                                  (autotestDualVideo
                                                                       ? 2.0
                                                                       : 10.0) /
                                                                      lap->time
                                                                          .back()
                                                            : 1.0;
                                                    videoReady =
                                                        videoReady &&
                                                        !video->paused() &&
                                                        store.cursorFrac() >
                                                            playbackStart +
                                                                0.002;
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
                                    if (windows.isEmpty())
                                        qWarning() << "AUTOTEST "
                                                      "found no "
                                                      "QQuickWindo"
                                                      "w";

                                    bool renameReady = true;
                                    if (autotestRename) {
                                        QObject* root =
                                            engine.rootObjects().isEmpty()
                                                ? nullptr
                                                : engine.rootObjects().first();
                                        renameReady =
                                            root &&
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
                                        QObject* root =
                                            engine.rootObjects().isEmpty()
                                                ? nullptr
                                                : engine.rootObjects().first();
                                        auto* primary =
                                            root ? root->findChild<
                                                       MpvVideoItem*>(
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
                                            root ? root->findChild<
                                                       MpvVideoItem*>(
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
                                        const double target =
                                            store.compareVideoTime();
                                        const double error =
                                            reference
                                                ? std::abs(
                                                      reference->position() -
                                                      target)
                                                : -1.0;
                                        auto* controls =
                                            root ? root->findChild<QQuickItem*>(
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
                                        const int seekBaseline =
                                            root ? root->property(
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
                                            reference->exactSeekCount() ==
                                                seekBaseline;
                                        const bool pauseAlignmentReady =
                                            root &&
                                            root->property(
                                                    "dualPauseAlignmentReady")
                                                .toBool();
                                        dualVideoReady =
                                            root &&
                                            root->property(
                                                    "dualVid"
                                                    "eo")
                                                .toBool() &&
                                            primary && primary->ready() &&
                                            primary->loaded() &&
                                            primary->duration() > 0.0 &&
                                            reference && reference->ready() &&
                                            reference->loaded() &&
                                            reference->duration() > 0.0 &&
                                            primary->source() !=
                                                reference->source() &&
                                            reference->muted() &&
                                            !primary->paused() &&
                                            !reference->paused() &&
                                            error <= 0.15 &&
                                            pauseAlignmentReady &&
                                            continuousPlayback &&
                                            !store.comparisonAlignmentBasis()
                                                 .isEmpty() &&
                                            fullscreenRestored && chromeReady &&
                                            sequentialVideoReady();
                                        qWarning()
                                            << "AUTOTEST "
                                               "dual video:"
                                            << dualVideoReady << "primary"
                                            << (primary ? primary->source()
                                                              .fileName()
                                                        : QString())
                                            << (primary ? primary->position()
                                                        : -1.0)
                                            << "reference"
                                            << (reference ? reference->source()
                                                                .fileName()
                                                          : QString())
                                            << (reference
                                                    ? reference->position()
                                                    : -1.0)
                                            << "target" << target << "error"
                                            << error << "loaded"
                                            << (primary && primary->loaded())
                                            << (reference &&
                                                reference->loaded())
                                            << "paused"
                                            << (primary ? primary->paused()
                                                        : false)
                                            << (reference ? reference->paused()
                                                          : false)
                                            << "store"
                                            << store.primaryVideoSource()
                                                   .fileName()
                                            << store.compareVideoSource()
                                                   .fileName();
                                        qWarning()
                                            << "AUTOTEST "
                                               "dual sync "
                                               "model:"
                                            << store.comparisonAlignmentBasis()
                                            << "gps anchors"
                                            << store.comparisonGpsAnchors()
                                            << "rate"
                                            << (reference
                                                    ? reference->playbackRate()
                                                    : -1.0)
                                            << "seeks "
                                               "during "
                                               "playback"
                                            << (reference && seekBaseline >= 0
                                                    ? reference
                                                              ->exactSeekCount() -
                                                          seekBaseline
                                                    : -1);
                                    }
                                    bool cornerMutationReady = true;
                                    bool cornerEscapeReady = true;
                                    if (autotestCornerEdit) {
                                        QObject* root =
                                            engine.rootObjects().isEmpty()
                                                ? nullptr
                                                : engine.rootObjects().first();
                                        auto* cornerWindow =
                                            root ? root->findChild<
                                                       QQuickWindow*>(
                                                       QStringLiteral("c"
                                                                      "o"
                                                                      "r"
                                                                      "n"
                                                                      "e"
                                                                      "r"
                                                                      "W"
                                                                      "i"
                                                                      "n"
                                                                      "d"
                                                                      "o"
                                                                      "w"))
                                                 : nullptr;
                                        cornerMutationReady =
                                            root && root->property(
                                                            "cornerE"
                                                            "ditAuto"
                                                            "testRea"
                                                            "dy")
                                                        .toBool();
                                        cornerEscapeReady =
                                            cornerWindow &&
                                            !cornerWindow->isVisible();
                                        qWarning()
                                            << "AUTOTEST "
                                               "corner "
                                               "mutations:"
                                            << cornerMutationReady
                                            << "escape:" << cornerEscapeReady;
                                    }
                                    const bool zoomReady =
                                        !autotestZoom ||
                                        (!engine.rootObjects().isEmpty() &&
                                         engine.rootObjects()
                                             .first()
                                             ->property("wheelZoomA"
                                                        "utotestRea"
                                                        "dy")
                                             .toBool());
                                    const QFileInfo requested(shotPath);
                                    for (QQuickWindow* window : windows) {
                                        QString output = shotPath;
                                        if (autotestWindows) {
                                            QString suffix = requested.suffix();
                                            if (!suffix.isEmpty())
                                                suffix.prepend('.');
                                            output =
                                                requested.path() + "/" +
                                                requested.completeBaseName() +
                                                "_" +
                                                (window->objectName().isEmpty()
                                                     ? QStringLiteral("window")
                                                     : window->objectName()) +
                                                suffix;
                                        }
                                        const QImage image =
                                            window->grabWindow();
                                        if (image.save(output))
                                            qWarning()
                                                << "AUTOTEST"
                                                   " saved:"
                                                << output << image.size();
                                        else
                                            qWarning() << "AUTOTEST"
                                                          " save "
                                                          "failed:"
                                                       << output;
                                    }
                                    videoReady = videoReady && renameReady &&
                                                 cornerMutationReady &&
                                                 cornerEscapeReady &&
                                                 zoomReady && dualVideoReady;
                                    const int exitCode =
                                        videoReady             ? 0
                                        : !cornerMutationReady ? 3
                                        : !cornerEscapeReady   ? 4
                                                               : 2;
                                    qApp->exit(exitCode);
                                });
                            return;
                        }
                    }
                }
            }
            qWarning() << "AUTOTEST no session found";
            qApp->exit(1);
        });
    startTimer->start();
    return true;
}
