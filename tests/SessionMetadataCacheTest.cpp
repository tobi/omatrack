#include "app/SessionMetadataCache.h"

#include <QFile>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

class SessionMetadataCacheTest : public QObject {
    Q_OBJECT
private slots:
    void fingerprintTracksPathSizeAndPrefix() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString firstPath =
            directory.filePath(QStringLiteral("first.pds"));
        const QString secondPath =
            directory.filePath(QStringLiteral("second.pds"));

        QFile first(firstPath);
        QVERIFY(first.open(QIODevice::WriteOnly));
        QCOMPARE(first.write("telemetry"), qint64(9));
        first.close();
        QVERIFY(QFile::copy(firstPath, secondPath));

        const QString original = SessionMetadataCache::fingerprint(firstPath);
        QVERIFY(!original.isEmpty());
        QCOMPARE(original.size(), 64);
        QVERIFY(original != SessionMetadataCache::fingerprint(secondPath));

        QVERIFY(first.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(first.write("Telemetry"), qint64(9));
        first.close();
        QVERIFY(original != SessionMetadataCache::fingerprint(firstPath));

        QVERIFY(first.open(QIODevice::WriteOnly | QIODevice::Append));
        QCOMPARE(first.write("!"), qint64(1));
        first.close();
        QVERIFY(original != SessionMetadataCache::fingerprint(firstPath));
    }

    void fingerprintIntentionallyBoundsContentRead() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("large.pds"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QByteArray contents(1024 * 1024 + 16, 'a');
        QCOMPARE(file.write(contents), qint64(contents.size()));
        file.close();

        const QString original = SessionMetadataCache::fingerprint(path);
        QVERIFY(file.open(QIODevice::ReadWrite));
        QVERIFY(file.seek(1024 * 1024 + 4));
        QCOMPARE(file.write("b", 1), qint64(1));
        file.close();
        QCOMPARE(SessionMetadataCache::fingerprint(path), original);
    }

    void fingerprintTracksMotecSidecar() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString ldPath = directory.filePath(QStringLiteral("run.ld"));
        const QString ldxPath = directory.filePath(QStringLiteral("run.ldx"));
        QFile ld(ldPath);
        QVERIFY(ld.open(QIODevice::WriteOnly));
        QCOMPARE(ld.write("telemetry"), qint64(9));
        ld.close();

        const QString withoutSidecar =
            SessionMetadataCache::fingerprint(ldPath);
        QFile ldx(ldxPath);
        QVERIFY(ldx.open(QIODevice::WriteOnly));
        QCOMPARE(ldx.write("markers-1"), qint64(9));
        ldx.close();
        const QString firstSidecar = SessionMetadataCache::fingerprint(ldPath);
        QVERIFY(firstSidecar != withoutSidecar);

        QVERIFY(ldx.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(ldx.write("markers-2"), qint64(9));
        ldx.close();
        QVERIFY(SessionMetadataCache::fingerprint(ldPath) != firstSidecar);

        QVERIFY(QFile::remove(ldxPath));
        QCOMPARE(SessionMetadataCache::fingerprint(ldPath), withoutSidecar);
    }

    void roundTripsSupportedAndUnsupportedEntries() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("index.json"));
        {
            SessionMetadataCache cache(path);
            cache.store(QStringLiteral("supported"), QStringLiteral("/run.pds"),
                        true,
                        QJsonObject{{QStringLiteral("driver"),
                                     QStringLiteral("Driver 1")}});
            cache.store(QStringLiteral("media"), QStringLiteral("/video.mp4"),
                        false, {});
            QVERIFY(cache.save());
        }
        {
            SessionMetadataCache cache(path);
            const auto supported = cache.lookup(QStringLiteral("supported"));
            QVERIFY(supported.found);
            QVERIFY(supported.supported);
            QCOMPARE(
                supported.metadata.value(QStringLiteral("driver")).toString(),
                QStringLiteral("Driver 1"));

            const auto media = cache.lookup(QStringLiteral("media"));
            QVERIFY(media.found);
            QVERIFY(!media.supported);
            QVERIFY(cache.lookup(QStringLiteral("missing")).found == false);
        }
    }

    void concurrentSnapshotsMergeOnSave() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("index.json"));
        SessionMetadataCache first(path);
        SessionMetadataCache second(path);
        first.store(QStringLiteral("first"), QStringLiteral("/first.pds"), true,
                    QJsonObject{{QStringLiteral("driver"), "First"}});
        second.store(QStringLiteral("second"), QStringLiteral("/second.pds"),
                     true, QJsonObject{{QStringLiteral("driver"), "Second"}});
        QVERIFY(first.save());
        QVERIFY(second.save());

        SessionMetadataCache reloaded(path);
        QVERIFY(reloaded.lookup(QStringLiteral("first")).found);
        QVERIFY(reloaded.lookup(QStringLiteral("second")).found);
    }

    void missingLookupIsEmpty() {
        SessionMetadataCache cache(QStringLiteral("/tmp/does-not-exist.json"));
        QVERIFY(!cache.lookup(QStringLiteral("missing")).found);
        QVERIFY(!cache.lookup(QString()).found);
    }

    void storeOverwritesTheSameFingerprint() {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("index.json"));
        SessionMetadataCache cache(path);
        cache.store(QStringLiteral("run"), QStringLiteral("/run.pds"), true,
                    QJsonObject{{QStringLiteral("driver"),
                                 QStringLiteral("First")}});
        cache.store(QStringLiteral("run"), QStringLiteral("/run.pds"), true,
                    QJsonObject{{QStringLiteral("driver"),
                                 QStringLiteral("Second")}});
        QVERIFY(cache.save());
        QCOMPARE(cache.lookup(QStringLiteral("run"))
                     .metadata.value(QStringLiteral("driver"))
                     .toString(),
                 QStringLiteral("Second"));
    }

    void missingFileHasNoFingerprint() {
        QVERIFY(SessionMetadataCache::fingerprint(
                    QStringLiteral("/tmp/omatrack-missing-session.pds"))
                    .isEmpty());
    }
};

QTEST_MAIN(SessionMetadataCacheTest)
#include "SessionMetadataCacheTest.moc"
