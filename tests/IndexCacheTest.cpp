#include "app/IndexCache.h"
#include "core/TelemetryEngine.h"

#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>

class IndexCacheTest : public QObject {
    Q_OBJECT
private slots:
    void identityIncludesGeneration() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("session.ld"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write("not-a-real-session"), 18);
        file.close();

        const omatrack::FileIdentity identity = omatrack::fileIdentity(path);
        QVERIFY(identity.valid);
        QCOMPARE(identity.generation,
                 QString::fromStdString(omatrack::converterGeneration()));
        const QString cachePath = omatrack::indexCachePath(identity);
        QVERIFY(cachePath.contains(identity.generation));
        QVERIFY(!cachePath.contains(QStringLiteral(".telemetry")));
    }

    void doesNotLoadOldCompletenessSummaries() {
        QTemporaryDir directory;
        const QByteArray previous = qgetenv("XDG_CACHE_HOME");
        const auto restore = qScopeGuard([previous]() {
            if (previous.isNull())
                qunsetenv("XDG_CACHE_HOME");
            else
                qputenv("XDG_CACHE_HOME", previous);
        });
        qputenv("XDG_CACHE_HOME", directory.path().toUtf8());
        const QString source = directory.filePath(QStringLiteral("source.ld"));
        QFile input(source);
        QVERIFY(input.open(QIODevice::WriteOnly));
        input.write("synthetic");
        input.close();
        const auto identity = omatrack::fileIdentity(source);
        QString legacy = omatrack::indexCachePath(identity);
        QVERIFY(legacy.contains(QStringLiteral("/index/v2/")));
        legacy.replace(QStringLiteral("/index/v2/"), QStringLiteral("/index/"));
        QVERIFY(QDir().mkpath(QFileInfo(legacy).absolutePath()));
        QFile cache(legacy);
        QVERIFY(cache.open(QIODevice::WriteOnly));
        cache.write(
            QJsonDocument(
                QJsonObject{
                    {QStringLiteral("converterGeneration"),
                     identity.generation},
                    {QStringLiteral("metadata"),
                     QJsonObject{{QStringLiteral("oldComplete"), true}}},
                })
                .toJson());
        cache.close();
        QVERIFY(omatrack::loadIndexCache(source).isEmpty());
    }

    void doesNotCacheEmptyMetadata() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("session.ld"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("x");
        file.close();
        QVERIFY(!omatrack::storeIndexCache(path, {}));
        QVERIFY(omatrack::loadIndexCache(path).isEmpty());
    }
};

QTEST_GUILESS_MAIN(IndexCacheTest)
#include "IndexCacheTest.moc"
