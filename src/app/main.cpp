// Omatrack application entry.
// Qt Quick Controls 2 Material UI; the telemetry surfaces are custom C++
// scene-graph items; parsing is delegated through the upstream Rust bridge.

#include <QFileInfo>
#include <QFontDatabase>
#include <QIcon>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlError>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QStandardPaths>
#include <QUrl>
#include <clocale>

#ifdef OMATRACK_ENABLE_AUTOTEST_HARNESS
#include "AutotestHarness.h"
#endif
#include "TelemetryStore.h"
#ifdef Q_OS_WIN
#include "WindowsIntegration.h"
#endif

int main(int argc, char** argv) {
#ifdef OMATRACK_ENABLE_AUTOTEST_HARNESS
    const bool autotest = !qgetenv("OMATRACK_AUTOTEST").isEmpty();
#else
    constexpr bool autotest = false;
#endif
    QCoreApplication::setOrganizationName(autotest ? "omatrack-autotest"
                                                   : "omatrack");
    QCoreApplication::setOrganizationDomain("tobi.github.io");
    QCoreApplication::setApplicationName("omatrack");
    QCoreApplication::setApplicationVersion(OMATRACK_VERSION);
    QGuiApplication::setApplicationDisplayName("Omatrack");
    const QString desktopFileName = QStringLiteral("io.github.tobi.omatrack");
    const QString desktopEntry = QStandardPaths::locate(
        QStandardPaths::ApplicationsLocation, desktopFileName + ".desktop");
    if (!desktopEntry.isEmpty())
        QGuiApplication::setDesktopFileName(desktopFileName);

    // The trace surfaces are scene-graph geometry now, so their edges are
    // antialiased by the framebuffer rather than by QPainter. libmpv renders
    // into its own FBO and is unaffected by the sample count.
    QSurfaceFormat surfaceFormat = QSurfaceFormat::defaultFormat();
    surfaceFormat.setSamples(4);
    QSurfaceFormat::setDefaultFormat(surfaceFormat);
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
    QGuiApplication app(argc, argv);
    std::setlocale(LC_NUMERIC, "C");
#ifdef Q_OS_WIN
    omatrack::initializeWindowsIntegration(app);
    QGuiApplication::setWindowIcon(
        QIcon(QStringLiteral(":/assets/omatrack.ico")));
#else
    QGuiApplication::setWindowIcon(
        QIcon(QStringLiteral(":/assets/omatrack.svg")));
#endif
    QQuickStyle::setStyle("Material");

    const QStringList bundledFonts{
        QStringLiteral(":/fonts/Geist-Variable.ttf"),
        QStringLiteral(":/fonts/GeistMono-Variable.ttf"),
    };
    for (const QString& font : bundledFonts) {
        if (QFontDatabase::addApplicationFont(font) < 0)
            qWarning() << "Failed to load bundled font:" << font;
    }

    // The store is the `Store` QML singleton; the engine owns the one
    // instance and creates it while loading Main.qml.

    QQmlApplicationEngine engine;
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed,
        [](const QUrl& url) { qWarning() << "QML load FAILED:" << url; });
    QObject::connect(&engine, &QQmlApplicationEngine::warnings,
                     [](const QList<QQmlError>& errs) {
                         for (const QQmlError& e : errs)
                             qWarning() << "QML:" << e.toString();
                     });
#ifdef OMATRACK_ENABLE_AUTOTEST_HARNESS
    const bool autotestWindows =
        !qgetenv("OMATRACK_AUTOTEST_WINDOWS").isEmpty();
#else
    constexpr bool autotestWindows = false;
#endif
    const QString startupVideoPath = qEnvironmentVariable("OMATRACK_VIDEO");
    // Root-window inputs travel as required properties, not context
    // properties, so qmllint and qmlcachegen can see them.
    engine.setInitialProperties({
        {QStringLiteral("autotestWindows"), autotestWindows},
        {QStringLiteral("startupVideo"),
         startupVideoPath.isEmpty() ? QUrl()
                                    : QUrl::fromLocalFile(startupVideoPath)},
    });
    engine.loadFromModule("Omatrack", "Main");
    if (engine.rootObjects().isEmpty()) {
        return -1;
    }

    TelemetryStore* store =
        engine.singletonInstance<TelemetryStore*>("Omatrack", "Store");
    if (!store) {
        qWarning() << "Store singleton missing";
        return -1;
    }
    // Configuration supplies persistent scan roots. A positional path opens a
    // directory or one supported telemetry/video file.
    const QStringList arguments = QCoreApplication::arguments();
    if (arguments.size() > 1) {
        const QString path = arguments.at(1);
        if (QFileInfo(path).isDir())
            store->addSessionDirectory(path);
        else
            store->openFile(path);
    }
    if (!startupVideoPath.isEmpty()) store->openFile(startupVideoPath);

#ifdef OMATRACK_ENABLE_AUTOTEST_HARNESS
    omatrack::autotest::install(engine, *store);
#endif
    return app.exec();
}
