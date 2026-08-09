// Unit tests for YamlConfig — the omatrack.yml configuration store.
//
// Tests nested-map CRUD, slash-path convenience, round-trip save/load,
// fresh-install detection, and removal. Each test uses a temp XDG_CONFIG_HOME
// so the singleton's file is isolated from the user's real config.

#include "app/YamlConfig.h"
#include "app/TrackMetadata.h"

#include <QtTest>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QTemporaryDir>

using namespace omatrack;

class YamlConfigTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        // Isolate config to a temp dir so we never touch the user's file.
        tempDir_ = QDir::tempPath() + "/omatrack-test-" +
                   QString::number(QCoreApplication::applicationPid());
        QDir().mkpath(tempDir_);
        qputenv("XDG_CONFIG_HOME", tempDir_.toUtf8());
    }

    void cleanup() {
        // Remove both current and pre-rename files between tests.
        QFile::remove(YamlConfig::filePath());
        QDir(tempDir_ + QStringLiteral("/racecraft")).removeRecursively();
    }

    void cleanupTestCase() { QDir(tempDir_).removeRecursively(); }

    void freshInstallHasNoValues() {
        YamlConfig config;
        QVERIFY(config.isFresh());
        QVERIFY(!config.value({"telemetry", "dir1"}).isValid());
    }

    void setValueAndGetRoundTrip() {
        YamlConfig config;
        config.setValue({"telemetry", "dir1"}, QStringLiteral("/data/tele"));
        QCOMPARE(config.value({"telemetry", "dir1"}).toString(),
                 QStringLiteral("/data/tele"));
    }

    void nestedThreeLevelsDeep() {
        YamlConfig config;
        config.setValue({"channels", "speed", "color"},
                        QStringLiteral("#ff0000"));
        QCOMPARE(config.value({"channels", "speed", "color"}).toString(),
                 QStringLiteral("#ff0000"));
    }

    void slashPathConvenience() {
        YamlConfig config;
        config.setValue("driver/aliases/7", QStringLiteral("Alice"));
        QCOMPARE(config.value("driver/aliases/7").toString(),
                 QStringLiteral("Alice"));
    }

    void missingKeyReturnsFallback() {
        YamlConfig config;
        QCOMPARE(config.value({"nonexistent", "key"}, 42).toInt(), 42);
    }

    void setInvalidRemovesKey() {
        YamlConfig config;
        config.setValue({"temp", "val"}, QStringLiteral("data"));
        config.setValue({"temp", "val"}, QVariant());  // invalid → remove
        QVERIFY(!config.value({"temp", "val"}).isValid());
    }

    void removeErasesKey() {
        YamlConfig config;
        config.setValue({"a", "b"}, 99);
        config.remove({"a", "b"});
        QVERIFY(!config.value({"a", "b"}).isValid());
    }

    void mapReturnsNestedMap() {
        YamlConfig config;
        config.setValue({"tracks", "IMS", "name"}, QStringLiteral("IMS"));
        config.setValue({"tracks", "IMS", "length"}, 4.0);
        QVariantMap m = config.map({"tracks", "IMS"});
        QCOMPARE(m["name"].toString(), QStringLiteral("IMS"));
        QCOMPARE(m["length"].toDouble(), 4.0);
    }

    void setMapReplacesEntireMap() {
        YamlConfig config;
        QVariantMap m;
        m["x"] = 1;
        m["y"] = 2;
        config.setMap({"section"}, m);
        QVariantMap out = config.map({"section"});
        QCOMPARE(out.size(), 2);
        QCOMPARE(out["x"].toInt(), 1);
        QCOMPARE(out["y"].toInt(), 2);
    }

    void mapOnAbsentPathReturnsEmpty() {
        YamlConfig config;
        QVERIFY(config.map({"does", "not", "exist"}).isEmpty());
    }

    void readsExternalDocumentWithoutChangingConfigPath() {
        const QString path = tempDir_ + QStringLiteral("/TRACK.yml");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(
            "channels:\n  speed: Speed_Ref\ntrack:\n  name: Road America\n");
        file.close();

        QString error;
        const QVariantMap document = YamlConfig::readDocument(path, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(document.value("channels").toMap().value("speed").toString(),
                 QStringLiteral("Speed_Ref"));
        QCOMPARE(document.value("track").toMap().value("name").toString(),
                 QStringLiteral("Road America"));
        QVERIFY(!QFile::exists(YamlConfig::filePath()));
    }

    void writesArbitraryDocumentAtomically() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("TRACK.yml"));
        const QVariantMap document{
            {QStringLiteral("schema"), QStringLiteral("2")},
            {QStringLiteral("files"),
             QVariantMap{{QStringLiteral("onboard.mp4"),
                          QVariantMap{{QStringLiteral("offset"),
                                       QStringLiteral("1.25")}}}}}};

        QString error;
        QVERIFY2(YamlConfig::writeDocument(path, document, &error),
                 qPrintable(error));
        QCOMPARE(YamlConfig::readDocument(path), document);
        QVERIFY(!QFileInfo::exists(path + QStringLiteral(".XXXXXX")));
    }

    void trackMetadataMergesEveryParentRootToLeaf() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString parent = directory.filePath(QStringLiteral("event"));
        const QString child = QDir(parent).filePath(QStringLiteral("run"));
        QVERIFY(QDir().mkpath(child));
        QVERIFY(YamlConfig::writeDocument(
            QDir(parent).filePath(QStringLiteral("TRACK.yml")),
            QVariantMap{
                {QStringLiteral("car"),
                 QVariantMap{
                     {QStringLiteral("number"), QStringLiteral("7")},
                     {QStringLiteral("class"), QStringLiteral("LMP2")}}},
                {QStringLiteral("channels"),
                 QVariantMap{
                     {QStringLiteral("speed"), QStringLiteral("Speed")}}}}));
        QVERIFY(YamlConfig::writeDocument(
            QDir(child).filePath(QStringLiteral("TRACK.yml")),
            QVariantMap{
                {QStringLiteral("car"),
                 QVariantMap{{QStringLiteral("number"), QStringLiteral("8")}}},
                {QStringLiteral("channels"),
                 QVariantMap{{QStringLiteral("brake"),
                              QStringLiteral("Brake_Pressure")}}}}));

        QStringList paths;
        const QVariantMap merged =
            omatrack::track_metadata::readHierarchy(child, true, &paths);
        QCOMPARE(paths.constLast(),
                 QDir(child).filePath(QStringLiteral("TRACK.yml")));
        QCOMPARE(merged.value(QStringLiteral("car"))
                     .toMap()
                     .value(QStringLiteral("number"))
                     .toString(),
                 QStringLiteral("8"));
        QCOMPARE(merged.value(QStringLiteral("car"))
                     .toMap()
                     .value(QStringLiteral("class"))
                     .toString(),
                 QStringLiteral("LMP2"));
        QCOMPARE(merged.value(QStringLiteral("channels"))
                     .toMap()
                     .value(QStringLiteral("speed"))
                     .toString(),
                 QStringLiteral("Speed"));
        QCOMPARE(merged.value(QStringLiteral("channels"))
                     .toMap()
                     .value(QStringLiteral("brake"))
                     .toString(),
                 QStringLiteral("Brake_Pressure"));
    }

    void trackMetadataUpdatePreservesUnrelatedKeys() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("TRACK.yml"));
        const QVariantMap files{
            {QStringLiteral("onboard.mp4"),
             QVariantMap{{QStringLiteral("offset"), QStringLiteral("1.25")}}}};
        QVERIFY(YamlConfig::writeDocument(
            path,
            QVariantMap{
                {QStringLiteral("schema"), QStringLiteral("1")},
                {QStringLiteral("car"), QVariantMap{{QStringLiteral("number"),
                                                     QStringLiteral("old")}}},
                {QStringLiteral("files"), files},
                {QStringLiteral("custom"), QStringLiteral("keep")}}));

        QString error;
        QVERIFY2(
            omatrack::track_metadata::update(
                directory.path(),
                QVariantMap{
                    {QStringLiteral("schema"), QStringLiteral("2")},
                    {QStringLiteral("event"), QStringLiteral("Road America")}},
                &error),
            qPrintable(error));
        const QVariantMap updated = YamlConfig::readDocument(path);
        QCOMPARE(updated.value(QStringLiteral("files")).toMap(), files);
        QCOMPARE(updated.value(QStringLiteral("custom")).toString(),
                 QStringLiteral("keep"));
        QCOMPARE(updated.value(QStringLiteral("event")).toString(),
                 QStringLiteral("Road America"));
        QVERIFY(!updated.contains(QStringLiteral("car")));
    }

    void emptyTrackMetadataRemovesOnlyOwnedKeys() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("TRACK.yml"));
        QVERIFY(YamlConfig::writeDocument(
            path,
            QVariantMap{{QStringLiteral("schema"), QStringLiteral("2")},
                        {QStringLiteral("event"), QStringLiteral("Old event")},
                        {QStringLiteral("files"),
                         QVariantMap{{QStringLiteral("video.mp4"),
                                      QStringLiteral("keep")}}}}));

        QVERIFY(omatrack::track_metadata::update(directory.path(), {}));
        const QVariantMap updated = YamlConfig::readDocument(path);
        QVERIFY(!updated.contains(QStringLiteral("schema")));
        QVERIFY(!updated.contains(QStringLiteral("event")));
        QVERIFY(updated.contains(QStringLiteral("files")));
    }

    void trackMetadataUpdateCreatesFileInSelectedFolder() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString selected =
            directory.filePath(QStringLiteral("selected-folder"));
        QVERIFY(QDir().mkpath(selected));

        QVERIFY(omatrack::track_metadata::update(
            selected,
            QVariantMap{{QStringLiteral("schema"), QStringLiteral("2")},
                        {QStringLiteral("series"), QStringLiteral("IMSA")}}));
        const QString expected =
            QDir(selected).filePath(QStringLiteral("TRACK.yml"));
        QVERIFY(QFileInfo::exists(expected));
        QCOMPARE(YamlConfig::readDocument(expected)
                     .value(QStringLiteral("series"))
                     .toString(),
                 QStringLiteral("IMSA"));
    }

    void driverMappingKeysAcceptPositiveNumbersAndWildcard() {
        using omatrack::track_metadata::normalizedDriverMappingKey;
        QCOMPARE(normalizedDriverMappingKey(QStringLiteral("  *  ")),
                 QStringLiteral("*"));
        QCOMPARE(normalizedDriverMappingKey(QStringLiteral("02.500")),
                 QStringLiteral("2.5"));
        QCOMPARE(normalizedDriverMappingKey(3.25), QStringLiteral("3.25"));
        QVERIFY(normalizedDriverMappingKey(QStringLiteral("0")).isEmpty());
        QVERIFY(normalizedDriverMappingKey(QStringLiteral("all")).isEmpty());
    }

    void exactDriverMappingWinsOverWildcardFallback() {
        const QVariantMap metadata{
            {QStringLiteral("driver"),
             QVariantMap{
                 {QStringLiteral("mappings"),
                  QVariantMap{
                      {QStringLiteral("*"), QStringLiteral("Any Driver")},
                      {QStringLiteral("2.50"), QStringLiteral("Exact Driver")},
                      {QStringLiteral("invalid"),
                       QStringLiteral("Ignored")}}}}}};

        QCOMPARE(omatrack::track_metadata::driverNameForId(metadata, 2.5),
                 QStringLiteral("Exact Driver"));
        QCOMPARE(omatrack::track_metadata::driverNameForId(metadata, 7.25),
                 QStringLiteral("Any Driver"));
        QVERIFY(
            omatrack::track_metadata::driverNameForId(metadata, 0).isEmpty());
    }

    void wildcardDriverMappingRoundTripsThroughTrackYaml() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("TRACK.yml"));
        const QVariantMap mappings{
            {QStringLiteral("*"), QStringLiteral("Any Driver")}};
        QVERIFY(YamlConfig::writeDocument(
            path, QVariantMap{
                      {QStringLiteral("driver"),
                       QVariantMap{{QStringLiteral("mappings"), mappings}}}}));
        QCOMPARE(YamlConfig::readDocument(path)
                     .value(QStringLiteral("driver"))
                     .toMap()
                     .value(QStringLiteral("mappings"))
                     .toMap(),
                 mappings);
    }

    void savePersistsAcrossInstances() {
        // Write with one instance, save, destroy, reload with a new instance.
        {
            YamlConfig config;
            config.setValue({"persisted", "value"}, QStringLiteral("hello"));
            config.save();
        }
        {
            YamlConfig reloaded;
            QVERIFY(!reloaded.isFresh());
            QCOMPARE(reloaded.value({"persisted", "value"}).toString(),
                     QStringLiteral("hello"));
        }
    }

    void saveWritesYamlToDisk() {
        YamlConfig config;
        config.setValue({"file", "check"}, 123);
        config.save();

        QFile f(YamlConfig::filePath());
        QVERIFY(f.exists());
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QByteArray content = f.readAll();
        QVERIFY(content.contains("file"));
        QVERIFY(content.contains("check"));
        QVERIFY(content.contains("123"));
    }

    void overwriteExistingValue() {
        YamlConfig config;
        config.setValue(QStringList{"key"}, QStringLiteral("old"));
        config.setValue(QStringList{"key"}, QStringLiteral("new"));
        QCOMPARE(config.value(QStringList{"key"}).toString(),
                 QStringLiteral("new"));
    }

    void siblingKeysCoexist() {
        YamlConfig config;
        config.setValue({"parent", "a"}, 1);
        config.setValue({"parent", "b"}, 2);
        config.setValue({"parent", "c"}, 3);
        QCOMPARE(config.value({"parent", "a"}).toInt(), 1);
        QCOMPARE(config.value({"parent", "b"}).toInt(), 2);
        QCOMPARE(config.value({"parent", "c"}).toInt(), 3);
    }

    void isFreshFalseAfterFileExists() {
        // First instance: file doesn't exist → fresh
        {
            YamlConfig config;
            QVERIFY(config.isFresh());
            config.setValue(QStringList{"x"}, 1);
            config.save();
        }
        // Second instance: file exists → not fresh
        {
            YamlConfig config;
            QVERIFY(!config.isFresh());
        }
    }
    void emptySequencePersistsAsIntentionalValue() {
        {
            YamlConfig config;
            config.setValue(QStringLiteral("telemetry_dirs"), QStringList{});
            config.save();
        }
        YamlConfig reloaded;
        const QVariant directories =
            reloaded.value(QStringLiteral("telemetry_dirs"));
        QVERIFY(directories.isValid());
        QVERIFY(directories.toStringList().isEmpty());
    }

    void malformedFileIsNeverOverwritten() {
        QFile file(YamlConfig::filePath());
        QVERIFY(file.open(QIODevice::WriteOnly));
        const QByteArray malformed("channels: [\n");
        QCOMPARE(file.write(malformed), malformed.size());
        file.close();

        YamlConfig config;
        QVERIFY(!config.isWritable());
        config.setValue(QStringLiteral("telemetry_dirs"),
                        QStringList{QStringLiteral("/must-not-write")});
        config.save();

        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), malformed);
    }

    void migratesPreRenameYamlOnce() {
        const QString legacyDir = tempDir_ + QStringLiteral("/racecraft");
        QVERIFY(QDir().mkpath(legacyDir));
        QFile legacyFile(legacyDir + QStringLiteral("/racecraft.yml"));
        QVERIFY(legacyFile.open(QIODevice::WriteOnly));
        legacyFile.write(
            "telemetry_dirs:\n  - \"/legacy/telemetry\"\n"
            "video:\n  muted: \"true\"\n");
        legacyFile.close();

        YamlConfig migrated;
        QVERIFY(!migrated.isFresh());
        QCOMPARE(
            migrated.value(QStringLiteral("telemetry_dirs")).toStringList(),
            QStringList{QStringLiteral("/legacy/telemetry")});
        QCOMPARE(migrated.value(QStringLiteral("video/muted")).toString(),
                 QStringLiteral("true"));
        QVERIFY(QFile::exists(YamlConfig::filePath()));
        QVERIFY(QFile::exists(legacyFile.fileName()));
    }
    void currentYamlWinsOverPreRenameYaml() {
        const QString legacyDir = tempDir_ + QStringLiteral("/racecraft");
        QVERIFY(QDir().mkpath(legacyDir));
        QFile legacyFile(legacyDir + QStringLiteral("/racecraft.yml"));
        QVERIFY(legacyFile.open(QIODevice::WriteOnly));
        legacyFile.write("source: \"legacy\"\n");
        legacyFile.close();

        QFile currentFile(YamlConfig::filePath());
        QVERIFY(currentFile.open(QIODevice::WriteOnly));
        currentFile.write("source: \"current\"\n");
        currentFile.close();

        YamlConfig config;
        QCOMPARE(config.value(QStringLiteral("source")).toString(),
                 QStringLiteral("current"));
    }

private:
    QString tempDir_;
};

QTEST_MAIN(YamlConfigTest)
#include "YamlConfigTest.moc"
