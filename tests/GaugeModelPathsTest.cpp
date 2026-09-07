#include "app/GaugeModelPaths.h"
#include <QtTest>
#include <QTemporaryDir>
#include <filesystem>

class GaugeModelPathsTest : public QObject {
    Q_OBJECT
    static void file(const QString& path) {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile value(path);
        if (!value.open(QIODevice::WriteOnly) || value.write("fixture") != 7)
            qFatal("Cannot create path fixture");
    }
    static bool linkDirectory(const QString& target, const QString& link) {
        std::error_code error;
        std::filesystem::create_directory_symlink(
            std::filesystem::u8path(target.toStdString()),
            std::filesystem::u8path(link.toStdString()), error);
        return !error;
    }
private slots:
    void platformLayouts() {
        const QString executable = "/package/Omatrack.app/Contents/MacOS";
        QCOMPARE(omatrack::gaugeModelRootForDirectory(executable, true),
                 QString("/package/Omatrack.app/Contents/Resources/models"));
        QCOMPARE(omatrack::gaugeModelRootForDirectory("/package/bin", false),
                 QString("/package/bin/models"));
    }
    void actualContentsRequired() {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const auto executable = temporary.filePath("App.app/Contents/MacOS");
        QVERIFY(QDir().mkpath(executable));
        const auto model =
            temporary.filePath("App.app/Contents/Resources/models/model.onnx");
        const auto runtime = temporary.filePath(
            "App.app/Contents/Frameworks/libonnxruntime.dylib");
        file(model);
        file(runtime);
        QVERIFY(
            omatrack::gaugeMacPackageContains(model, executable, "Resources"));
        QVERIFY(omatrack::gaugeMacPackageContains(runtime, executable,
                                                  "Frameworks"));
        QVERIFY(!omatrack::gaugeMacPackageContains(runtime, executable,
                                                   "Resources"));
    }
    void externalSubdirectorySymlinksRejected() {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const auto executable = temporary.filePath("App.app/Contents/MacOS");
        QVERIFY(QDir().mkpath(executable));
        for (const auto* section : {"Resources", "Frameworks"}) {
            const auto external =
                temporary.filePath(QString("external-") + section);
            const auto target = external + "/models/fixture";
            file(target);
            const auto linked =
                temporary.filePath(QString("App.app/Contents/") + section);
            if (!linkDirectory(external, linked))
                QSKIP("Directory symlink fixture unavailable on this host");
            QVERIFY(!omatrack::gaugeMacPackageContains(
                linked + "/models/fixture", executable, section));
            QVERIFY(!omatrack::gaugeMacPackageContains(target, executable,
                                                       section));
        }
    }
    void internalAliasesRemainUsable() {
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const auto executable = temporary.filePath("App.app/Contents/MacOS");
        QVERIFY(QDir().mkpath(executable));
        const auto internal = temporary.filePath("App.app/Contents/ModelData");
        file(internal + "/models/fixture");
        const auto linked = temporary.filePath("App.app/Contents/Resources");
        if (!linkDirectory(internal, linked))
            QSKIP("Directory symlink fixture unavailable on this host");
        QVERIFY(omatrack::gaugeMacPackageContains(linked + "/models/fixture",
                                                  executable, "Resources"));
    }
};
QTEST_GUILESS_MAIN(GaugeModelPathsTest)
#include "GaugeModelPathsTest.moc"
