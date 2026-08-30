#include "app/IndexCache.h"
#include "core/TelemetryEngine.h"

#include <QFile>
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
