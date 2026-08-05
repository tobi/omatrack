// racecraft-qt application entry.
// Qt Quick Controls 2 Material UI; telemetry canvas is a custom C++
// QQuickPaintedItem; parsing is the vendored Rust bridge.

#include <QFontDatabase>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlError>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QUrl>
#include <clocale>

#include "AutotestHarness.h"
#include "TelemetryStore.h"

int main(int argc, char** argv) {
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
    QGuiApplication app(argc, argv);
    std::setlocale(LC_NUMERIC, "C");
    const bool autotest = !qgetenv("RACECRAFT_AUTOTEST").isEmpty();
    QCoreApplication::setOrganizationName(autotest ? "racecraft-autotest"
                                                   : "racecraft");
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
    const bool autotestWindows =
        !qgetenv("RACECRAFT_AUTOTEST_WINDOWS").isEmpty();
    const QString startupVideoPath = qEnvironmentVariable("RACECRAFT_VIDEO");
    // Root-window inputs travel as required properties, not context
    // properties, so qmllint and qmlcachegen can see them.
    engine.setInitialProperties({
        {QStringLiteral("autotestWindows"), autotestWindows},
        {QStringLiteral("startupVideo"),
         startupVideoPath.isEmpty() ? QUrl()
                                    : QUrl::fromLocalFile(startupVideoPath)},
    });
    engine.loadFromModule("Racecraft", "Main");
    if (engine.rootObjects().isEmpty()) {
        return -1;
    }

    TelemetryStore* store =
        engine.singletonInstance<TelemetryStore*>("Racecraft", "Store");
    if (!store) {
        qWarning() << "Store singleton missing";
        return -1;
    }
    // Configured telemetry directories come from racecraft.yml; a positional
    // argument adds one scan root for this launch (and is remembered).
    if (argc > 1) store->addSessionDirectory(QString::fromLocal8Bit(argv[1]));
    if (!startupVideoPath.isEmpty()) store->openFile(startupVideoPath);

    racecraft::autotest::install(engine, *store);
    return app.exec();
}
