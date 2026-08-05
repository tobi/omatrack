// racecraft-qt application entry.
// Qt Quick Controls 2 Material UI; telemetry canvas is a custom C++
// QQuickPaintedItem; parsing is the vendored Rust bridge.

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlError>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QDir>
#include <QElapsedTimer>
#include <QImage>
#include <QHoverEvent>
#include <QKeyEvent>
#include <QFileInfo>
#include <QPainter>
#include <QFontDatabase>
#include <QMouseEvent>
#include <QTimer>
#include <QFile>
#include <QProcess>
#include <QRegularExpression>
#include <QUrl>
#include <QVariantList>
#include <cmath>
#include <clocale>
#include <QVariantMap>

QVariantMap loadOmarchyColors() {
    QVariantMap colors;
    QProcess themeProcess;
    themeProcess.start(QStringLiteral("omarchy"),
                       {QStringLiteral("theme"), QStringLiteral("current")});
    if (!themeProcess.waitForFinished(500)) return colors;
    QString theme =
        QString::fromUtf8(themeProcess.readAllStandardOutput()).trimmed()
            .toLower();
    theme.replace(QRegularExpression(QStringLiteral("[^a-z0-9-]")),
                  QStringLiteral("-"));
    if (theme.isEmpty()) return colors;
    const QString omarchyPath =
        qEnvironmentVariable("OMARCHY_PATH",
                             QStringLiteral("/usr/share/omarchy"));
    QFile file(omarchyPath + QStringLiteral("/themes/") + theme +
               QStringLiteral("/colors.toml"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return colors;
    const QRegularExpression assignment(
        QStringLiteral("^\\s*([a-z_]+)\\s*=\\s*\"(#[0-9a-fA-F]{6,8})\""));
    const QString text = QString::fromUtf8(file.readAll());
    for (const QString& line : text.split('\n')) {
        const QRegularExpressionMatch match = assignment.match(line);
        if (match.hasMatch())
            colors.insert(match.captured(1), match.captured(2));
    }
    return colors;
}

#include "core/TelemetryEngine.h"
#include "app/MpvVideoItem.h"
#include "app/TelemetryStore.h"
#include "app/TraceView.h"

int main(int argc, char** argv) {
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
    QGuiApplication app(argc, argv);
    std::setlocale(LC_NUMERIC, "C");
    const bool autotest = !qgetenv("RACECRAFT_AUTOTEST").isEmpty();
    QCoreApplication::setOrganizationName(autotest ? "racecraft-autotest" : "racecraft");
    QCoreApplication::setApplicationName("racecraft-qt");
    QQuickStyle::setStyle("Material");

    const QStringList bundledFonts{
        QStringLiteral(":/fonts/Geist-Variable.ttf"),
        QStringLiteral(":/fonts/GeistMono-Variable.ttf"),
    };
    for (const QString& font : bundledFonts) {
        if (QFontDatabase::addApplicationFont(font) < 0)
            qWarning() << "Failed to load bundled font:" << font;
    }

    qmlRegisterType<TraceView>("Racecraft", 1, 0, "TraceView");
    qmlRegisterType<TraceCursorOverlay>(
        "Racecraft", 1, 0, "TraceCursorOverlay");
    qmlRegisterType<MpvVideoItem>("Racecraft", 1, 0, "MpvVideoItem");

    TelemetryStore store;

    // Configured telemetry directories come from racecraft.yml; a positional
    // argument adds one scan root for this launch (and is remembered).
    if (argc > 1) store.addSessionDirectory(QString::fromLocal8Bit(argv[1]));

    QQmlApplicationEngine engine;
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed,
                     [](const QUrl& url) {
                         qWarning() << "QML load FAILED:" << url;
                     });
    QObject::connect(&engine, &QQmlApplicationEngine::warnings,
                     [](const QList<QQmlError>& errs) {
                         for (const QQmlError& e : errs)
                             qWarning() << "QML:" << e.toString();
                     });
    engine.rootContext()->setContextProperty("store", &store);
    engine.rootContext()->setContextProperty(
        "omarchyColors", loadOmarchyColors());
    const bool autotestWindows = !qgetenv("RACECRAFT_AUTOTEST_WINDOWS").isEmpty();
    engine.rootContext()->setContextProperty("autotestWindows", autotestWindows);
    const QString startupVideoPath =
        qEnvironmentVariable("RACECRAFT_VIDEO");
    if (!startupVideoPath.isEmpty()) store.openFile(startupVideoPath);
    engine.rootContext()->setContextProperty(
        "startupVideo",
        startupVideoPath.isEmpty()
            ? QUrl()
            : QUrl::fromLocalFile(startupVideoPath));
    engine.load(QUrl(QStringLiteral("qrc:/Main.qml")));
    if (engine.rootObjects().isEmpty()) {
        return -1;
    }

    // RACECRAFT_AUTOTEST: headless self-check — open first session's fastest
    // lap, render one frame, screenshot to the given path, then exit.
    const QByteArray autotestShot = qgetenv("RACECRAFT_AUTOTEST");
    const bool autotestCompare = !qgetenv("RACECRAFT_AUTOTEST_COMPARE").isEmpty();
    const bool autotestHover = !qgetenv("RACECRAFT_AUTOTEST_HOVER").isEmpty();
    const bool autotestSelection =
        !qgetenv("RACECRAFT_AUTOTEST_SELECTION").isEmpty();
    const bool autotestAlignment =
        !qgetenv("RACECRAFT_AUTOTEST_ALIGNMENT").isEmpty();
    const bool autotestZoom =
        !qgetenv("RACECRAFT_AUTOTEST_ZOOM").isEmpty();
    const bool autotestCorner =
        !qgetenv("RACECRAFT_AUTOTEST_CORNER").isEmpty();
    const bool autotestRename =
        !qgetenv("RACECRAFT_AUTOTEST_RENAME").isEmpty();
    const bool autotestBrakeSync =
        !qgetenv("RACECRAFT_AUTOTEST_BRAKE_SYNC").isEmpty();
    const bool autotestCornerEdit =
        !qgetenv("RACECRAFT_AUTOTEST_CORNER_EDIT").isEmpty();
    if (!autotestShot.isEmpty()) {
        const QString shotPath = QString::fromUtf8(autotestShot);
        QTimer::singleShot(
            200, &engine,
            [&store, &engine, shotPath, startupVideoPath,
             autotestCompare, autotestWindows, autotestHover,
             autotestSelection, autotestAlignment, autotestZoom,
             autotestCorner, autotestRename, autotestBrakeSync,
             autotestCornerEdit]() {
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
                            if (!startupVideoPath.isEmpty() &&
                                !store.primaryVideoSource().isEmpty())
                                store.setCursorFrac(0.5);
                            if (autotestBrakeSync) {
                                const racecraft::UnifiedLap* lap =
                                    store.primaryUnified();
                                if (lap && lap->brake.size() > 1) {
                                    for (size_t sample = 1;
                                         sample < lap->brake.size();
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
                            if (autotestRename &&
                                !engine.rootObjects().isEmpty()) {
                                QObject* root = engine.rootObjects().first();
                                QMetaObject::invokeMethod(
                                    root, "openDriverRename",
                                    Q_ARG(QVariant,
                                          store.primaryDriverMappingKey()),
                                    Q_ARG(QVariant,
                                          store.primaryDriverName()));
                                auto* field = root->findChild<QObject*>(
                                    QStringLiteral("driverRenameField"));
                                auto* dialog = root->findChild<QObject*>(
                                    QStringLiteral("driverRenameDialog"));
                                if (field && dialog) {
                                    field->setProperty(
                                        "text",
                                        QStringLiteral("Autotest Driver"));
                                    QMetaObject::invokeMethod(dialog,
                                                              "accept");
                                }
                            }
                            bool comparisonSelected = false;
                            if (autotestCompare) {
                                for (const QVariant& otherGroupValue : groups) {
                                    if (comparisonSelected) break;
                                    const QVariantList otherDates =
                                        otherGroupValue.toMap().value("dates").toList();
                                    for (const QVariant& otherDateValue : otherDates) {
                                        if (comparisonSelected) break;
                                        const QVariantList otherSessions =
                                            otherDateValue.toMap().value("sessions").toList();
                                        for (const QVariant& otherSessionValue : otherSessions) {
                                            const QString otherKey =
                                                otherSessionValue.toMap().value("key").toString();
                                            if (otherKey == key) continue;
                                            const QVariantList otherLaps =
                                                store.lapsForSession(otherKey);
                                            for (const QVariant& otherLapValue : otherLaps) {
                                                const QVariantMap otherLap =
                                                    otherLapValue.toMap();
                                                if (!otherLap.value("isFastest").toBool())
                                                    continue;
                                                // First selected session is the fixed
                                                // reference; second is the active run.
                                                store.selectLap(
                                                    otherKey,
                                                    otherLap.value("lapId").toInt());
                                                store.compareLap(key, best);
                                                comparisonSelected = true;
                                                break;
                                            }
                                            if (comparisonSelected) break;
                                        }
                                    }
                                }
                            }
                            if (!comparisonSelected) store.clearCompare();
                            // Never clobber Track Atlas ranges or a saved
                            // racecraft.yml override with brake-zone guesses.
                            if (store.cornerList().isEmpty())
                                store.autoGenerateCorners();
                            if (autotestCornerEdit &&
                                !engine.rootObjects().isEmpty()) {
                                QObject* root = engine.rootObjects().first();
                                const int before = store.cornerList().size();
                                const int added =
                                    store.addCorner(0.94, 0.98);
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
                                store.setEditingCorners(true);
                                // A right-click must open the Material menu:
                                // a QWidget QMenu would abort this process.
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
                                        QCoreApplication::sendEvent(trace,
                                                                    &press);
                                        QObject* menu =
                                            root->findChild<QObject*>(
                                                QStringLiteral("cornerMenu"));
                                        menuReady =
                                            menu &&
                                            menu->property("visible").toBool();
                                        if (menu)
                                            QMetaObject::invokeMethod(menu,
                                                                      "close");
                                        // Same path with editing off must
                                        // open the channel menu instead.
                                        store.setEditingCorners(false);
                                        QCoreApplication::sendEvent(trace,
                                                                    &press);
                                        QObject* channels =
                                            root->findChild<QObject*>(
                                                QStringLiteral("channelMenu"));
                                        const bool channelReady =
                                            channels &&
                                            channels->property("visible")
                                                .toBool();
                                        if (channels)
                                            QMetaObject::invokeMethod(channels,
                                                                      "close");
                                        qWarning()
                                            << "AUTOTEST channel menu:"
                                            << channelReady;
                                        menuReady = menuReady && channelReady;
                                        store.setEditingCorners(true);
                                    }
                                }
                                qWarning() << "AUTOTEST corner menu:"
                                           << menuReady;
                                root->setProperty(
                                    "cornerEditAutotestReady",
                                    addedReady && renamedReady && menuReady &&
                                    store.cornerList().size() == before);
                                auto* window =
                                    root->findChild<QQuickWindow*>(
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
                                        qWarning()
                                            << "AUTOTEST zone editor:"
                                            << image.save(zonesShot)
                                            << zonesShot;
                                    });
                                    QTimer::singleShot(
                                        1000, &engine, [root]() {
                                        QMetaObject::invokeMethod(
                                            root,
                                            "dismissCornerPopover");
                                    });
                                }
                            }
                            if (autotestAlignment && comparisonSelected)
                                store.setReferenceAlignment(0.02);
                            if (autotestSelection &&
                                !engine.rootObjects().isEmpty()) {
                                auto* trace = engine.rootObjects()
                                                  .first()
                                                  ->findChild<TraceView*>(
                                                      QStringLiteral("traceView"));
                                if (trace && trace->width() > 0 &&
                                    trace->height() > 0) {
                                    const QPointF start(trace->width() * 0.28,
                                                        trace->height() * 0.35);
                                    const QPointF finish(trace->width() * 0.62,
                                                         trace->height() * 0.35);
                                    QMouseEvent begin(
                                        QEvent::MouseButtonDblClick,
                                        start, start, start,
                                        Qt::LeftButton, Qt::LeftButton,
                                        Qt::NoModifier);
                                    QCoreApplication::sendEvent(trace, &begin);
                                    QHoverEvent move(
                                        QEvent::HoverMove, finish, finish,
                                        start, Qt::NoModifier);
                                    QCoreApplication::sendEvent(trace, &move);
                                    QMouseEvent commit(
                                        QEvent::MouseButtonPress,
                                        finish, finish, finish,
                                        Qt::LeftButton, Qt::LeftButton,
                                        Qt::NoModifier);
                                    QCoreApplication::sendEvent(trace, &commit);
                                    QMouseEvent release(
                                        QEvent::MouseButtonRelease,
                                        finish, finish, finish,
                                        Qt::LeftButton, Qt::NoButton,
                                        Qt::NoModifier);
                                    QCoreApplication::sendEvent(trace, &release);
                                }
                            }
                            if (autotestCorner &&
                                !engine.rootObjects().isEmpty()) {
                                auto* trace = engine.rootObjects()
                                                  .first()
                                                  ->findChild<TraceView*>(
                                                      QStringLiteral("traceView"));
                                const QVariantList corners = store.cornerList();
                                if (trace && !corners.isEmpty()) {
                                    const QVariantMap corner =
                                        corners.first().toMap();
                                    const double middle =
                                        (corner.value("start").toDouble() +
                                         corner.value("end").toDouble()) * 0.5;
                                    const QPointF position(
                                        62.0 + middle *
                                                   std::max(
                                                       1.0,
                                                       trace->width() - 62.0),
                                        10.0);
                                    QMouseEvent click(
                                        QEvent::MouseButtonPress,
                                        position, position, position,
                                        Qt::LeftButton, Qt::LeftButton,
                                        Qt::NoModifier);
                                    QCoreApplication::sendEvent(trace, &click);
                                    QMouseEvent release(
                                        QEvent::MouseButtonRelease,
                                        position, position, position,
                                        Qt::LeftButton, Qt::NoButton,
                                        Qt::NoModifier);
                                    QCoreApplication::sendEvent(trace, &release);
                                }
                            }
                            if (autotestHover && !engine.rootObjects().isEmpty()) {
                                auto* overlay = engine.rootObjects()
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
                                    for (int frame = 0; frame < frames; ++frame) {
                                        store.setCursorFrac(
                                            double(frame) / (frames - 1));
                                        benchmark.fill(Qt::transparent);
                                        QPainter painter(&benchmark);
                                        overlay->paint(&painter);
                                    }
                                    qWarning()
                                        << "AUTOTEST hover overlay average_ms:"
                                        << (clock.nsecsElapsed() / 1.0e6) /
                                               frames;
                                }
                            }
                            if (autotestZoom &&
                                !engine.rootObjects().isEmpty()) {
                                auto* trace = engine.rootObjects()
                                                  .first()
                                                  ->findChild<TraceView*>(
                                                      QStringLiteral("traceView"));
                                if (trace && trace->width() > 0 &&
                                    trace->height() > 0) {
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
                                    for (int frame = 0; frame < frames; ++frame) {
                                        const double phase =
                                            double(frame) / double(frames - 1);
                                        const double span =
                                            0.20 + 0.65 *
                                                       (0.5 + 0.5 *
                                                                  std::sin(
                                                                      phase *
                                                                      6.283185307));
                                        const double start =
                                            (1.0 - span) * phase;
                                        store.setViewStart(start);
                                        store.setViewEnd(start + span);
                                        benchmark.fill(Qt::transparent);
                                        QPainter painter(&benchmark);
                                        trace->paint(&painter);
                                    }
                                    qWarning() << "AUTOTEST zoom paint average_ms:"
                                               << (clock.nsecsElapsed() / 1.0e6) /
                                                      frames;
                                    store.setViewStart(0.0);
                                    store.setViewEnd(1.0);
                                }
                            }
                            if (!startupVideoPath.isEmpty() &&
                                !store.primaryVideoSource().isEmpty() &&
                                !autotestBrakeSync) {
                                auto* keyTimer = new QTimer(&engine);
                                keyTimer->setInterval(50);
                                QObject::connect(
                                    keyTimer, &QTimer::timeout, &engine,
                                    [keyTimer, &engine]() {
                                    if (engine.rootObjects().isEmpty()) return;
                                    QObject* root = engine.rootObjects().first();
                                    auto* video =
                                        root->findChild<MpvVideoItem*>(
                                            QStringLiteral("videoPlayer"));
                                    auto* window =
                                        qobject_cast<QQuickWindow*>(root);
                                    if (!video || !video->loaded() || !window)
                                        return;
                                    for (const Qt::Key key :
                                         {Qt::Key_Right, Qt::Key_Left,
                                          Qt::Key_Space}) {
                                        QKeyEvent press(
                                            QEvent::KeyPress, key,
                                            Qt::NoModifier);
                                        QCoreApplication::sendEvent(window,
                                                                    &press);
                                        QKeyEvent release(
                                            QEvent::KeyRelease, key,
                                            Qt::NoModifier);
                                        QCoreApplication::sendEvent(window,
                                                                    &release);
                                    }
                                    keyTimer->stop();
                                    keyTimer->deleteLater();
                                });
                                keyTimer->start();
                            }
                            QTimer::singleShot(
                                2500, &engine,
                                [&store, &engine, shotPath, startupVideoPath,
                                 autotestWindows, autotestRename,
                                 autotestBrakeSync,
                                 autotestCornerEdit]() {
                                QList<QQuickWindow*> windows;
                                for (QObject* root : engine.rootObjects()) {
                                    if (auto* window = qobject_cast<QQuickWindow*>(root))
                                        windows.append(window);
                                    if (autotestWindows) {
                                        for (QQuickWindow* child : root->findChildren<QQuickWindow*>())
                                            if (!windows.contains(child)) windows.append(child);
                                    }
                                }
                                bool videoReady =
                                    startupVideoPath.isEmpty();
                                if (!videoReady) {
                                    for (QObject* root :
                                         engine.rootObjects()) {
                                        auto* video =
                                            root->findChild<MpvVideoItem*>(
                                                QStringLiteral(
                                                    "videoPlayer"));
                                        if (!video || !video->ready() ||
                                            !video->loaded() ||
                                            video->duration() <= 0.0)
                                            continue;
                                        videoReady =
                                            std::abs(video->volume() - 75.0) <=
                                            0.01;
                                        if (!store.primaryVideoSource().isEmpty()) {
                                            const double target =
                                                store.primaryVideoTime();
                                            const double error =
                                                std::abs(video->position() -
                                                         target);
                                            qWarning()
                                                << "AUTOTEST video sync:"
                                                << video->position()
                                                << "target" << target
                                                << "error" << error;
                                            videoReady =
                                                videoReady && error <= 0.1;
                                            const racecraft::UnifiedLap* lap =
                                                store.primaryUnified();
                                            if (autotestBrakeSync) {
                                                const size_t sample =
                                                    lap && !lap->brake.empty()
                                                        ? std::min(
                                                              size_t(std::llround(
                                                                  store.cursorFrac() *
                                                                  double(lap->brake.size() - 1))),
                                                              lap->brake.size() - 1)
                                                        : 0;
                                                const bool brakeReady =
                                                    lap && !lap->brake.empty() &&
                                                    video->paused() &&
                                                    lap->brake[sample] >= 10.0;
                                                qWarning()
                                                    << "AUTOTEST brake sync:"
                                                    << brakeReady
                                                    << "lap time"
                                                    << (lap && !lap->time.empty()
                                                            ? lap->time[sample]
                                                            : -1.0);
                                                videoReady =
                                                    videoReady && brakeReady;
                                            } else {
                                                const double playbackStart =
                                                    lap && !lap->time.empty()
                                                        ? 0.5 +
                                                              10.0 /
                                                                  lap->time.back()
                                                        : 1.0;
                                                videoReady =
                                                    videoReady &&
                                                    !video->paused() &&
                                                    store.cursorFrac() >
                                                        playbackStart + 0.002;
                                            }
                                        }
                                        qWarning()
                                            << "AUTOTEST video volume:"
                                            << video->volume();
                                        break;
                                    }
                                    if (!videoReady)
                                        qWarning()
                                            << "AUTOTEST video failed to load or sync";
                                }
                                if (windows.isEmpty())
                                    qWarning() << "AUTOTEST found no QQuickWindow";

                                bool renameReady = true;
                                if (autotestRename) {
                                    QObject* root =
                                        engine.rootObjects().isEmpty()
                                            ? nullptr
                                            : engine.rootObjects().first();
                                    renameReady =
                                        root &&
                                        root->findChild<QObject*>(
                                            QStringLiteral(
                                                "headerDriverEdit")) &&
                                        store.driverDisplayName(
                                            store.primarySessionKey()) ==
                                            QStringLiteral(
                                                "Autotest Driver");
                                    qWarning()
                                        << "AUTOTEST driver rename:"
                                        << renameReady;
                                }
                                qWarning()
                                    << "AUTOTEST gps available:"
                                    << store.hasGpsData();
                                bool cornerMutationReady = true;
                                bool cornerEscapeReady = true;
                                if (autotestCornerEdit) {
                                    QObject* root =
                                        engine.rootObjects().isEmpty()
                                            ? nullptr
                                            : engine.rootObjects().first();
                                    auto* cornerWindow =
                                        root
                                            ? root->findChild<QQuickWindow*>(
                                                  QStringLiteral(
                                                      "cornerWindow"))
                                            : nullptr;
                                    cornerMutationReady =
                                        root &&
                                        root->property(
                                                "cornerEditAutotestReady")
                                            .toBool();
                                    cornerEscapeReady =
                                        cornerWindow &&
                                        !cornerWindow->isVisible();
                                    qWarning()
                                        << "AUTOTEST corner mutations:"
                                        << cornerMutationReady
                                        << "escape:" << cornerEscapeReady;
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
                                        qWarning() << "AUTOTEST saved:" << output << image.size();
                                    else
                                        qWarning() << "AUTOTEST save failed:" << output;
                                }
                                videoReady =
                                    videoReady && renameReady &&
                                    cornerMutationReady &&
                                    cornerEscapeReady;
                                const int exitCode =
                                    videoReady
                                        ? 0
                                        : !cornerMutationReady
                                            ? 3
                                            : !cornerEscapeReady ? 4 : 2;
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
    }

    return app.exec();
}
