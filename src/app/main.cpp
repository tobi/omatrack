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
#include <cstdio>
#include <cstring>

#ifdef OMATRACK_ENABLE_AUTOTEST_HARNESS
#include "AutotestHarness.h"
#endif
#include "TelemetryStore.h"
#include "VerboseLog.h"
#ifdef Q_OS_WIN
#include "WindowsAssociations.h"
#include "WindowsIntegration.h"
#endif

namespace {

bool takeFlag(int& argc, char** argv, const char* longName,
              const char* shortName) {
    bool found = false;
    int out = 1;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], longName) == 0 ||
            (shortName && std::strcmp(argv[i], shortName) == 0)) {
            found = true;
            continue;
        }
        argv[out++] = argv[i];
    }
    argv[out] = nullptr;
    argc = out;
    return found;
}

void printHelp(const char* executable) {
    std::printf(
        "Omatrack telemetry workstation\n\n"
        "Usage:\n"
        "  %s [options] [telemetry-directory|telemetry-or-video-file]\n"
        "  %s --help\n\n"
        "Options:\n"
        "  -h, --help     Show this help and exit.\n"
        "  -V, --version  Print the version and exit.\n"
        "  -v, --verbose  Log file opens, cache hits and misses, writes,\n"
        "                 video/cursor seeks, and AiM vs .telemetry channel\n"
        "                 compare. Same as OMATRACK_VERBOSE=1.\n"
        "                 On Arch/Omarchy also set QT_FORCE_STDERR_LOGGING=1\n"
        "                 if you launch from a desktop entry.\n\n"
        "Headless inspection, CSV export, and corner analysis are currently "
        "provided by omatrack-cli:\n"
        "  omatrack-cli parse <file>   (.pds .ld .vbo .mp4 .telemetry)\n"
        "  omatrack-cli unify <file> --output <csv>\n"
        "  omatrack-cli corners <file> [--reference <file>] "
        "--zone <start:end>\n"
        "  omatrack-cli compare <aimd.mp4> <file.telemetry>\n\n"
        "Run omatrack-cli without arguments for its complete usage.\n",
        executable, executable);
}

}  // namespace

int main(int argc, char** argv) {
#ifdef Q_OS_WIN
    if (omatrack::consumeWindowsSetupHook(argc, argv)) return 0;
#endif
    const bool helpRequested = takeFlag(argc, argv, "--help", "-h");
    const bool versionRequested = takeFlag(argc, argv, "--version", "-V");
    const bool verbose = takeFlag(argc, argv, "--verbose", "-v") ||
                         qEnvironmentVariableIntValue("OMATRACK_VERBOSE") != 0;
    if (helpRequested) {
        printHelp(argc > 0 ? argv[0] : "omatrack");
        return 0;
    }
    if (versionRequested) {
        std::printf("omatrack %s\n", OMATRACK_VERSION);
        return 0;
    }
    // Must precede QGuiApplication: Arch Qt logs qInfo to journald otherwise.
    if (verbose) qputenv("QT_FORCE_STDERR_LOGGING", "1");
#ifdef OMATRACK_ENABLE_AUTOTEST_HARNESS
    const bool autotest = !qgetenv("OMATRACK_AUTOTEST").isEmpty();
    // The harness renames drivers, edits corners, pins files and appends the
    // positional directory as a scan root — all of which are written to
    // omatrack.yml. That must never be the developer's own configuration, so
    // an acceptance run gets a scratch XDG_CONFIG_HOME unless the caller
    // already pointed one somewhere deliberate.
    if (autotest && qEnvironmentVariableIsEmpty("XDG_CONFIG_HOME")) {
        const QString scratch =
            QStandardPaths::writableLocation(QStandardPaths::TempLocation) +
            QStringLiteral("/omatrack-autotest/config");
        qputenv("XDG_CONFIG_HOME", scratch.toUtf8());
    }
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
    if (verbose) omatrack::setVerbose(true);
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
