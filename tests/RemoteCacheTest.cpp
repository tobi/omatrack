// The sync engine and its protocols, against a local HTTP server.
//
// One responder speaks both WebDAV and the S3 API, because the point of these
// tests is the shared engine: discovery, the ETag cache-hit test, the offline
// fallback, and the rules about which names may become files are the same code
// whichever protocol produced the listing.

#include "app/RemoteCache.h"

#include <QtTest>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTcpServer>
#include <QTcpSocket>

#include <memory>

using namespace omatrack;

class RemoteCacheTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        cacheDir_ = std::make_unique<QTemporaryDir>();
        QVERIFY(cacheDir_->isValid());
        qputenv("XDG_CACHE_HOME", cacheDir_->path().toUtf8());
        QVERIFY(server_.listen(QHostAddress::LocalHost));
        connect(&server_, &QTcpServer::newConnection, this,
                &RemoteCacheTest::acceptConnection);
    }

    void cleanupTestCase() {
        server_.close();
        qunsetenv("XDG_CACHE_HOME");
    }

    void downloadsAndReusesCachedFiles() {
        requests_ = 0;
        propfinds_ = 0;
        gets_ = 0;
        RemoteConnection connection;
        connection.name = QStringLiteral("Test server");
        connection.target = QStringLiteral("http://127.0.0.1:%1/dav/")
                                .arg(server_.serverPort());
        connection.id = locationId(connection.target, {});

        const RemoteSyncResult first = syncConnection(connection);
        QVERIFY2(first.success, qPrintable(first.error));
        QCOMPARE(first.files, QStringList{QStringLiteral("session.vbo")});
        QCOMPARE(propfinds_, 1);
        QCOMPARE(gets_, 1);
        QFile cached(QDir(first.cachePath).filePath("session.vbo"));
        QVERIFY(cached.open(QIODevice::ReadOnly));
        QCOMPARE(cached.readAll(), QByteArrayLiteral("telemetry"));

        const RemoteSyncResult second = syncConnection(connection);
        QVERIFY2(second.success, qPrintable(second.error));
        QVERIFY(!second.fromCache);
        QCOMPARE(propfinds_, 2);
        QCOMPARE(gets_, 1);
    }

    void servesCacheWhenServerIsOffline() {
        RemoteConnection connection;
        connection.target = QStringLiteral("http://127.0.0.1:%1/dav/")
                                .arg(server_.serverPort());
        connection.id = locationId(connection.target, {});
        const RemoteSyncResult online = syncConnection(connection);
        QVERIFY2(online.success, qPrintable(online.error));

        server_.close();
        const RemoteSyncResult offline = syncConnection(connection);
        QVERIFY(offline.success);
        QVERIFY(offline.fromCache);
        QCOMPARE(offline.files, QStringList{QStringLiteral("session.vbo")});

        QVERIFY(server_.listen(QHostAddress::LocalHost));
    }

    // ── S3 ──────────────────────────────────────────────────────────

    /// A bucket behaves exactly as a WebDAV collection does from the library's
    /// point of view: files land in a cache directory, and a second sync that
    /// finds nothing changed downloads nothing.
    void listsAndDownloadsAnS3Bucket() {
        gets_ = 0;
        listings_ = 0;
        lastAuthorization_.clear();
        scenario_ = QStringLiteral("plain");

        const RemoteConnection connection = s3Connection(
            QStringLiteral("s3://team-telemetry/season-2026/"), scenario_);

        const RemoteSyncResult first = syncConnection(connection);
        QVERIFY2(first.success, qPrintable(first.error));
        QCOMPARE(first.files, (QStringList{QStringLiteral("brands-hatch.vbo"),
                                           QStringLiteral("spa/lap 2.vbo")}));
        QCOMPARE(listings_, 1);
        QCOMPARE(gets_, 2);

        QFile cached(QDir(first.cachePath).filePath("spa/lap 2.vbo"));
        QVERIFY(cached.open(QIODevice::ReadOnly));
        QCOMPARE(cached.readAll(), QByteArrayLiteral("telemetry"));

        // Signed, and signed the way GCS also accepts: nothing beyond host,
        // the payload hash, and the date is committed to.
        QVERIFY(lastAuthorization_.startsWith("AWS4-HMAC-SHA256 Credential="));
        QVERIFY(lastAuthorization_.contains(
            "SignedHeaders=host;x-amz-content-sha256;x-amz-date,"));
        QVERIFY(lastAuthorization_.contains("/eu-west-2/s3/aws4_request"));

        const RemoteSyncResult second = syncConnection(connection);
        QVERIFY2(second.success, qPrintable(second.error));
        QCOMPARE(listings_, 2);
        QCOMPARE(gets_, 2);
        QCOMPARE(second.downloadedBytes, 0);
    }

    /// A bucket bigger than one page. The continuation token is base64, so it
    /// carries `+` and `/` — the characters Qt would otherwise leave raw in a
    /// query, where a `+` reads back as a space and the token is lost.
    void pagesThroughATruncatedListing() {
        gets_ = 0;
        listings_ = 0;
        lastQuery_.clear();
        scenario_ = QStringLiteral("paged");

        const RemoteConnection connection =
            s3Connection(QStringLiteral("s3://team-telemetry/"), scenario_);
        const RemoteSyncResult result = syncConnection(connection);

        QVERIFY2(result.success, qPrintable(result.error));
        QCOMPARE(listings_, 2);
        QCOMPARE(result.files, (QStringList{QStringLiteral("page-one.vbo"),
                                            QStringLiteral("page-two.vbo")}));
        QVERIFY2(lastQuery_.contains("continuation-token=tok%2Ben%2F1%3D%3D"),
                 lastQuery_.constData());
    }

    /// S3 keys are byte strings: they may hold characters no Windows
    /// filesystem accepts, and a console-created bucket is full of zero-byte
    /// keys that only exist to make it look like it has folders.
    void skipsKeysThatCannotBecomeFiles() {
        gets_ = 0;
        listings_ = 0;
        scenario_ = QStringLiteral("awkward");

        const RemoteConnection connection =
            s3Connection(QStringLiteral("s3://team-telemetry/"), scenario_);
        const RemoteSyncResult result = syncConnection(connection);

        QVERIFY2(result.success, qPrintable(result.error));
        QCOMPARE(result.files, QStringList{QStringLiteral("clean.vbo")});
        // The folder placeholder is not a file and is not worth mentioning;
        // the unusable name is, or it would go missing without explanation.
        QCOMPARE(result.skipped, QStringList{QStringLiteral("notes:draft.vbo")});
        QVERIFY(result.status.contains(QStringLiteral("1 unusable name")));
        QCOMPARE(gets_, 1);
        QVERIFY(!QFileInfo::exists(QDir(result.cachePath).filePath("season")));
    }

    /// The XML error code is the actionable part of an S3 refusal; the raw
    /// document is not something to put in front of a driver.
    void explainsWhatTheBucketRefused() {
        scenario_ = QStringLiteral("denied");
        const RemoteConnection connection =
            s3Connection(QStringLiteral("s3://team-telemetry/"), scenario_);

        const RemoteSyncResult result = syncConnection(connection);
        QVERIFY(!result.success);
        QCOMPARE(result.error,
                 QStringLiteral("The access key or secret was not accepted."));
        QVERIFY(!result.error.contains(QStringLiteral("<")));
    }

    /// GCS reaches the same backend, so what is worth proving is that the
    /// type carries the two things that differ: the `gs://` scheme, and a
    /// signature scoped to the literal region "auto" with nothing discovered.
    void readsAGoogleCloudStorageBucket() {
        gets_ = 0;
        listings_ = 0;
        lastAuthorization_.clear();
        scenario_ = QStringLiteral("plain");

        RemoteConnection connection = s3Connection(
            QStringLiteral("gs://team-telemetry/season-2026/"),
            QStringLiteral("gcs"));
        connection.type = LocationType::Gcs;
        connection.options.remove(QStringLiteral("region"));

        const RemoteSyncResult result = syncConnection(connection);
        QVERIFY2(result.success, qPrintable(result.error));
        QCOMPARE(result.files, (QStringList{QStringLiteral("brands-hatch.vbo"),
                                            QStringLiteral("spa/lap 2.vbo")}));
        QVERIFY2(lastAuthorization_.contains("/auto/s3/aws4_request"),
                 lastAuthorization_.constData());
        // Google strips headers in transit, so signing any more than these
        // makes a perfectly good bucket answer SignatureDoesNotMatch.
        QVERIFY(lastAuthorization_.contains(
            "SignedHeaders=host;x-amz-content-sha256;x-amz-date,"));

        // The cache is keyed by type, so one bucket reached two ways stays
        // two libraries rather than one that fights with itself.
        QVERIFY(result.cachePath.contains(QStringLiteral("/gcs/")));
    }

    void rejectsATargetThatDisagreesWithItsType() {
        QVERIFY(validateTarget(LocationType::Gcs,
                               QStringLiteral("gs://bucket/prefix"))
                    .isEmpty());
        // Picking Google in the dialog and pasting an S3 address is a real
        // slip, and it would otherwise be signed against the wrong host.
        QVERIFY(!validateTarget(LocationType::Gcs,
                                QStringLiteral("s3://bucket/prefix"))
                     .isEmpty());
        QVERIFY(!validateTarget(LocationType::S3,
                                QStringLiteral("gs://bucket/prefix"))
                     .isEmpty());
        QCOMPARE(normalizeTarget(LocationType::Gcs, QStringLiteral("gs://b/x")),
                 QStringLiteral("gs://b/x/"));
    }

    void validatesAndNormalizesS3Targets() {
        QVERIFY(validateTarget(LocationType::S3,
                               QStringLiteral("s3://bucket/prefix"))
                    .isEmpty());
        QVERIFY(!validateTarget(LocationType::S3,
                                QStringLiteral("https://bucket/prefix"))
                     .isEmpty());
        QVERIFY(!validateTarget(LocationType::S3, QStringLiteral("s3://ab"))
                     .isEmpty());

        // Every spelling of one folder has to settle on one string, because
        // the connection id is a hash of it and two spellings would otherwise
        // download the same bucket twice.
        const QString canonical =
            normalizeTarget(LocationType::S3, QStringLiteral("s3://b/season/"));
        QCOMPARE(normalizeTarget(LocationType::S3, QStringLiteral("s3://b/season")),
                 canonical);
        QCOMPARE(normalizeTarget(LocationType::S3,
                                 QStringLiteral("  s3://b//season//  ")),
                 canonical);

        // A bucket is not a hostname. Lowercasing one — which is exactly what
        // routing this through QUrl would do — names a bucket that is not there.
        QCOMPARE(normalizeTarget(LocationType::S3, QStringLiteral("s3://MyBucket/A/")),
                 QStringLiteral("s3://MyBucket/A/"));
    }

    // ── streaming ───────────────────────────────────────────────────

    /// Onboard video is 5–30 GB against telemetry's kilobytes, so it is never
    /// downloaded. What the cache holds is a zero-byte stand-in, and what the
    /// player gets is a presigned URL it can fetch with no credentials.
    void streamsVideoRatherThanDownloadingIt() {
        gets_ = 0;
        listings_ = 0;
        scenario_ = QStringLiteral("video");

        const RemoteConnection connection =
            s3Connection(QStringLiteral("s3://team-telemetry/"), scenario_);
        const RemoteSyncResult result = syncConnection(connection);

        QVERIFY2(result.success, qPrintable(result.error));
        // The video is in the library exactly as a downloaded file would be:
        // everything downstream is keyed on a local path.
        QCOMPARE(result.files, (QStringList{QStringLiteral("data.vbo"),
                                            QStringLiteral("onboard.mp4")}));
        QCOMPARE(gets_, 1);

        const QString stub = QDir(result.cachePath).filePath("onboard.mp4");
        QVERIFY(QFileInfo::exists(stub));
        QCOMPARE(QFileInfo(stub).size(), 0);

        const QUrl source = streamSource(connection, stub);
        QVERIFY2(source.isValid(), qPrintable(source.toString()));
        QCOMPARE(source.path(), QStringLiteral("/team-telemetry/onboard.mp4"));
        QCOMPARE(source.port(), int(server_.serverPort()));
        const QString query = source.query(QUrl::FullyEncoded);
        QVERIFY2(query.contains(QStringLiteral("X-Amz-Signature=")),
                 qPrintable(query));
        // Query-string auth, so nothing has to teach mpv about AWS.
        QVERIFY(query.contains(QStringLiteral("X-Amz-Expires=43200")));
        QVERIFY(query.contains(QStringLiteral("eu-west-2%2Fs3%2Faws4_request")));

        // Telemetry beside it is still a real file.
        QVERIFY(!streamSource(connection,
                              QDir(result.cachePath).filePath("data.vbo"))
                     .isValid());
    }

    /// A cache filled by a build that downloaded video is still holding those
    /// gigabytes. The sync is where they are handed back.
    void reclaimsAVideoAnOlderBuildDownloaded() {
        gets_ = 0;
        scenario_ = QStringLiteral("video");
        // Its own cache, so what it proves is the upgrade and not the ETag
        // reuse the test above already left behind.
        const RemoteConnection connection = s3Connection(
            QStringLiteral("s3://team-telemetry/"), QStringLiteral("reclaim"));
        const QString stub =
            QDir(cacheDirectory(connection)).filePath("onboard.mp4");
        QVERIFY(QDir().mkpath(QFileInfo(stub).absolutePath()));
        QFile downloaded(stub);
        QVERIFY(downloaded.open(QIODevice::WriteOnly));
        QCOMPARE(downloaded.write(QByteArray(64 * 1024, 'v')), 64 * 1024);
        downloaded.close();

        const RemoteSyncResult result = syncConnection(connection);
        QVERIFY2(result.success, qPrintable(result.error));
        QCOMPARE(QFileInfo(stub).size(), 0);
        QCOMPARE(gets_, 1);
    }

    /// WebDAV has no presigning, so the credential rides in the URL — which
    /// ffmpeg reads and the player therefore never has to know about.
    void putsWebDavCredentialsInTheStreamUrl() {
        scenario_ = QStringLiteral("video");
        RemoteConnection connection;
        connection.target = QStringLiteral("http://127.0.0.1:%1/dav/")
                                .arg(server_.serverPort());
        connection.username = QStringLiteral("driver");
        connection.password = QStringLiteral("p@ss/word");
        connection.id = locationId(connection.target + scenario_, {});

        const RemoteSyncResult result = syncConnection(connection);
        QVERIFY2(result.success, qPrintable(result.error));
        const QString stub = QDir(result.cachePath).filePath("onboard.mp4");
        QCOMPARE(QFileInfo(stub).size(), 0);

        const QUrl source = streamSource(connection, stub);
        QCOMPARE(source.userName(), QStringLiteral("driver"));
        QCOMPARE(source.password(), QStringLiteral("p@ss/word"));
        // Encoded on the way out, so a password full of URL punctuation
        // survives the trip to the player intact.
        QVERIFY(source.toString(QUrl::FullyEncoded)
                    .contains(QStringLiteral("driver:p%40ss%2Fword@")));
        // And absent from anything that might be shown or logged.
        QVERIFY(!source.toDisplayString().contains(QStringLiteral("word")));
    }

    // ── offline downloads ───────────────────────────────────────────

    /// Streaming is no use on a plane. A recording can be asked for by name,
    /// and once it is here it plays from disk, survives a re-sync, and is
    /// beyond the reach of the budget until it is given back.
    void keepsAPinnedVideoForTheFlight() {
        gets_ = 0;
        scenario_ = QStringLiteral("video");
        const RemoteConnection connection = s3Connection(
            QStringLiteral("s3://team-telemetry/"), QStringLiteral("offline"));

        const RemoteSyncResult first = syncConnection(connection);
        QVERIFY2(first.success, qPrintable(first.error));
        const QString stub = QDir(first.cachePath).filePath("onboard.mp4");
        QCOMPARE(QFileInfo(stub).size(), 0);
        QVERIFY(!offlineVideoPinned(connection, stub));

        // Pinning is a wish, not a transfer: a library scan must never be
        // where thirty gigabytes start moving.
        QCOMPARE(pinOfflineVideo(connection, stub, true), QString());
        QVERIFY(offlineVideoPinned(connection, stub));
        QCOMPARE(QFileInfo(stub).size(), 0);

        qint64 announced = -2;
        QCOMPARE(fetchObject(connection, stub,
                             [&announced](qint64, qint64 total) {
                                 announced = total;
                                 return true;
                             }),
                 QString());
        QCOMPARE(QFileInfo(stub).size(), 9);
        QCOMPARE(announced, 9);

        // Here, so the player is handed the file rather than a signature.
        QVERIFY(!streamSource(connection, stub).isValid());

        // Neither counted against the budget nor a candidate for it: one
        // recording would otherwise evict a whole season of telemetry.
        const CacheUsage usage = cacheUsage();
        QVERIFY2(usage.videoBytes >= 9,
                 qPrintable(QString::number(usage.videoBytes)));
        QCOMPARE(enforceCacheBudget(usage.bytes, {}), 0);
        QCOMPARE(QFileInfo(stub).size(), 9);

        // A sync neither hands the bytes back nor fetches them again.
        gets_ = 0;
        const RemoteSyncResult again = syncConnection(connection);
        QVERIFY2(again.success, qPrintable(again.error));
        QCOMPARE(gets_, 0);
        QCOMPARE(QFileInfo(stub).size(), 9);
        QVERIFY(offlineVideoPinned(connection, stub));

        // And withdrawing the wish returns the space and the stream.
        QCOMPARE(pinOfflineVideo(connection, stub, false), QString());
        QVERIFY(!offlineVideoPinned(connection, stub));
        QCOMPARE(QFileInfo(stub).size(), 0);
        QVERIFY(streamSource(connection, stub).isValid());
    }

    /// A download nobody can use is worse than none: if the server replaced
    /// the recording, what is on disk is the wrong one, and the sync says so
    /// by handing the space back rather than quietly keeping it.
    void reclaimsAPinnedVideoTheServerReplaced() {
        scenario_ = QStringLiteral("video");
        const RemoteConnection connection = s3Connection(
            QStringLiteral("s3://team-telemetry/"), QStringLiteral("replaced"));
        QVERIFY(syncConnection(connection).success);
        const QString stub =
            QDir(cacheDirectory(connection)).filePath("onboard.mp4");
        QCOMPARE(pinOfflineVideo(connection, stub, true), QString());
        QCOMPARE(fetchObject(connection, stub, {}), QString());
        QCOMPARE(QFileInfo(stub).size(), 9);

        scenario_ = QStringLiteral("video-recut");
        QVERIFY(syncConnection(connection).success);
        QCOMPARE(QFileInfo(stub).size(), 0);
        // The wish stands — it is the file that went stale, not the intent.
        QVERIFY(offlineVideoPinned(connection, stub));
        scenario_ = QStringLiteral("video");
    }

    // ── the budget ──────────────────────────────────────────────────

    void readsASizeTheWayItIsWritten() {
        QCOMPARE(parseByteSize(QStringLiteral("20 GB"), 0), 20LL << 30);
        QCOMPARE(parseByteSize(QStringLiteral("20GiB"), 0), 20LL << 30);
        QCOMPARE(parseByteSize(QStringLiteral("  500 mb "), 0), 500LL << 20);
        QCOMPARE(parseByteSize(QStringLiteral("1.5g"), 0), 1610612736LL);
        QCOMPARE(parseByteSize(QStringLiteral("4096"), 0), 4096LL);
        // A typo turns the cache off if it is taken at face value, so it is
        // not: the default stands and the library keeps working.
        QCOMPARE(parseByteSize(QStringLiteral("plenty"), 99), 99LL);
        QCOMPARE(parseByteSize(QStringLiteral("20 parsecs"), 99), 99LL);
        QCOMPARE(parseByteSize(QString(), 99), 99LL);
        QCOMPARE(parseByteSize(QStringLiteral("0"), 99), 99LL);
    }

    /// Oldest out first, with the exceptions that would each be a bug: the
    /// file being played, the stub a session is discovered by, and the index
    /// that makes everything beside it reusable.
    void evictsTheLeastRecentlyOpenedFirst() {
        const QString directory =
            cacheRoot() + QStringLiteral("/s3/budget-fixture");
        QVERIFY(QDir().mkpath(directory));
        const QDateTime now = QDateTime::currentDateTime();
        const QString oldest = write(directory, "oldest.vbo", 40'000,
                                     now.addSecs(-3000));
        const QString older =
            write(directory, "older.vbo", 40'000, now.addSecs(-2000));
        const QString openNow =
            write(directory, "playing.vbo", 40'000, now.addSecs(-4000));
        const QString newest =
            write(directory, "newest.vbo", 40'000, now.addSecs(-10));
        const QString stub = write(directory, "onboard.mp4", 0, now.addSecs(-5000));
        const QString index = write(directory, "index.json", 40'000,
                                    now.addSecs(-6000));

        const qint64 before = cacheUsage().bytes;
        const qint64 freed =
            enforceCacheBudget(before - 50'000, QSet<QString>{openNow});

        QCOMPARE(freed, 80'000);
        QVERIFY(!QFileInfo::exists(oldest));
        QVERIFY(!QFileInfo::exists(older));
        QVERIFY(QFileInfo::exists(newest));
        // Older than everything deleted, and kept anyway.
        QVERIFY(QFileInfo::exists(openNow));
        QVERIFY(QFileInfo::exists(stub));
        QVERIFY(QFileInfo::exists(index));

        // Under the limit is a no-op, not a trim to some watermark.
        QCOMPARE(enforceCacheBudget(cacheUsage().bytes + 1, {}), 0);
        // And a limit of zero means "unset", not "keep nothing".
        QCOMPARE(enforceCacheBudget(0, {}), 0);

        QVERIFY(QDir(directory).removeRecursively());
    }

    /// The index still lists what eviction took, which is what makes the next
    /// sync fetch it again instead of reading the gap as a server-side delete.
    void refetchesWhatTheBudgetEvicted() {
        gets_ = 0;
        scenario_ = QStringLiteral("plain");
        const RemoteConnection connection =
            s3Connection(QStringLiteral("s3://team-telemetry/season-2026/"),
                         QStringLiteral("budget"));

        QVERIFY(syncConnection(connection).success);
        QCOMPARE(gets_, 2);
        const QString cache = cacheDirectory(connection);
        QVERIFY(QFile::remove(QDir(cache).filePath("brands-hatch.vbo")));

        const RemoteSyncResult again = syncConnection(connection);
        QVERIFY2(again.success, qPrintable(again.error));
        QCOMPARE(gets_, 3);
        QVERIFY(QFileInfo::exists(QDir(cache).filePath("brands-hatch.vbo")));
        // The file that survived was not fetched a second time.
        QCOMPARE(again.files.size(), 2);
    }

    /// Declared last on purpose: it deletes what every test above downloaded.
    void measuresAndClearsTheWholeCache() {
        // Accounting has to come off the filesystem. An orphan like this one —
        // a cache whose location was removed, or a download that died
        // mid-write — is invisible to any index and still occupies the disk.
        const QString orphan =
            cacheRoot() + QStringLiteral("/s3/gone-with-a-location");
        QVERIFY(QDir().mkpath(orphan));
        QFile stray(QDir(orphan).filePath("leftover.vbo.tmp"));
        QVERIFY(stray.open(QIODevice::WriteOnly));
        QCOMPARE(stray.write(QByteArray(4096, 'x')), 4096);
        stray.close();

        const CacheUsage before = cacheUsage();
        QVERIFY2(before.bytes > 4096, qPrintable(QString::number(before.bytes)));

        QCOMPARE(clearCache(), before.bytes + before.videoBytes);
        QCOMPARE(cacheUsage().bytes, 0);
        QCOMPARE(cacheUsage().videoBytes, 0);
        QVERIFY(!QFileInfo::exists(orphan));
    }

private:
    /// A cached file of a given size, last opened at a given moment.
    QString write(const QString& directory, const QString& name, int size,
                  const QDateTime& used) {
        const QString path = QDir(directory).filePath(name);
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly)) return {};
        if (size > 0 && file.write(QByteArray(size, 'x')) != size) return {};
        file.setFileTime(used, QFileDevice::FileModificationTime);
        return path;
    }

    RemoteConnection s3Connection(const QString& target,
                                  const QString& scenario) const {
        RemoteConnection connection;
        connection.type = LocationType::S3;
        connection.target = target;
        connection.username = QStringLiteral("AKIAIOSFODNN7EXAMPLE");
        connection.password = QStringLiteral("secret");
        // A custom endpoint is what makes this testable at all: it points the
        // backend at the local server and switches it to path-style
        // addressing, which needs no wildcard DNS.
        connection.options.insert(
            QStringLiteral("endpoint"),
            QStringLiteral("http://127.0.0.1:%1").arg(server_.serverPort()));
        connection.options.insert(QStringLiteral("region"),
                                  QStringLiteral("eu-west-2"));
        connection.id = locationId(target + scenario, {});
        return connection;
    }

    void acceptConnection() {
        while (server_.hasPendingConnections()) {
            QTcpSocket* socket = server_.nextPendingConnection();
            connect(socket, &QTcpSocket::readyRead, this,
                    [this, socket]() { serve(socket); });
            connect(socket, &QTcpSocket::disconnected, socket,
                    &QObject::deleteLater);
        }
    }

    /// One <Contents> row, with the key percent-encoded as encoding-type=url
    /// promises.
    static QByteArray contents(const QString& key, const QString& etag) {
        return QStringLiteral(
                   "<Contents><Key>%1</Key>"
                   "<LastModified>2026-08-01T10:00:00.000Z</LastModified>"
                   "<ETag>&quot;%2&quot;</ETag><Size>9</Size></Contents>")
            .arg(QString::fromLatin1(QUrl::toPercentEncoding(key)), etag)
            .toUtf8();
    }

    QByteArray listingFor(const QByteArray& target) {
        QByteArray body =
            "<?xml version=\"1.0\"?><ListBucketResult>"
            "<EncodingType>url</EncodingType>";
        if (scenario_ == QStringLiteral("paged")) {
            const bool second = target.contains("continuation-token=");
            body += second ? "<IsTruncated>false</IsTruncated>"
                           : "<IsTruncated>true</IsTruncated>"
                             "<NextContinuationToken>tok%2Ben%2F1%3D%3D"
                             "</NextContinuationToken>";
            body += contents(second ? QStringLiteral("page-two.vbo")
                                    : QStringLiteral("page-one.vbo"),
                             QStringLiteral("e1"));
        } else if (scenario_ == QStringLiteral("video") ||
                   scenario_ == QStringLiteral("video-recut")) {
            body += "<IsTruncated>false</IsTruncated>";
            body += contents(QStringLiteral("data.vbo"), QStringLiteral("e1"));
            body += contents(QStringLiteral("onboard.mp4"),
                             scenario_.endsWith(QStringLiteral("recut"))
                                 ? QStringLiteral("e9")
                                 : QStringLiteral("e2"));
        } else if (scenario_ == QStringLiteral("awkward")) {
            body += "<IsTruncated>false</IsTruncated>";
            body += contents(QStringLiteral("clean.vbo"), QStringLiteral("e1"));
            body += contents(QStringLiteral("notes:draft.vbo"),
                             QStringLiteral("e2"));
            body += contents(QStringLiteral("season/"), QStringLiteral("e3"));
        } else {
            body += "<IsTruncated>false</IsTruncated>";
            body += contents(QStringLiteral("season-2026/brands-hatch.vbo"),
                             QStringLiteral("e1"));
            body += contents(QStringLiteral("season-2026/spa/lap 2.vbo"),
                             QStringLiteral("e2"));
            // Outside the prefix: the listing is filtered by the backend, not
            // trusted to have been filtered by the server.
            body += contents(QStringLiteral("season-2025/old.vbo"),
                             QStringLiteral("e3"));
        }
        return body + "</ListBucketResult>";
    }

    void serve(QTcpSocket* socket) {
        const QByteArray request = socket->readAll();
        if (!request.contains("\r\n\r\n")) return;
        ++requests_;
        const QByteArray target =
            request.mid(request.indexOf(' ') + 1,
                        request.indexOf(" HTTP/") - request.indexOf(' ') - 1);

        QByteArray payload;
        QByteArray status = "404 Not Found";
        if (request.startsWith("PROPFIND")) {
            ++propfinds_;
            const QString root = QStringLiteral("http://127.0.0.1:%1/dav/")
                                     .arg(server_.serverPort());
            const QString member = QStringLiteral(
                "<response><href>%1%2</href>"
                "<propstat><prop><getetag>\"%3\"</getetag>"
                "<getlastmodified>now</getlastmodified>"
                "<getcontentlength>9</getcontentlength>"
                "</prop></propstat></response>");
            QString body =
                QStringLiteral(
                    "<?xml version=\"1.0\"?><multistatus "
                    "xmlns=\"DAV:\"><response><href>%1</href>"
                    "<propstat><prop><resourcetype><collection/>"
                    "</resourcetype></prop></propstat></response>")
                    .arg(root) +
                member.arg(root, QStringLiteral("session.vbo"),
                           QStringLiteral("one"));
            if (scenario_ == QStringLiteral("video"))
                body += member.arg(root, QStringLiteral("onboard.mp4"),
                                   QStringLiteral("two"));
            payload = (body + QStringLiteral("</multistatus>")).toUtf8();
            status = "207 Multi-Status";
        } else if (request.startsWith("GET /dav/session.vbo")) {
            ++gets_;
            payload = QByteArrayLiteral("telemetry");
            status = "200 OK";
        } else if (target.contains("list-type=2")) {
            ++listings_;
            lastQuery_ = target;
            lastAuthorization_ = headerOf(request, "Authorization");
            if (scenario_ == QStringLiteral("denied")) {
                payload =
                    "<?xml version=\"1.0\"?><Error>"
                    "<Code>SignatureDoesNotMatch</Code>"
                    "<Message>The request signature we calculated does not "
                    "match.</Message></Error>";
                status = "403 Forbidden";
            } else {
                payload = listingFor(target);
                status = "200 OK";
            }
        } else if (target.startsWith("/team-telemetry/")) {
            ++gets_;
            lastAuthorization_ = headerOf(request, "Authorization");
            payload = QByteArrayLiteral("telemetry");
            status = "200 OK";
        }

        QByteArray response = "HTTP/1.1 ";
        response += status;
        response += "\r\nConnection: close\r\n";
        response += "Content-Type: application/xml\r\nContent-Length: ";
        response += QByteArray::number(payload.size());
        response += "\r\n\r\n";
        response += payload;
        socket->write(response);
        socket->disconnectFromHost();
    }

    static QByteArray headerOf(const QByteArray& request,
                               const QByteArray& name) {
        const int at = request.indexOf("\r\n" + name + ": ");
        if (at < 0) return {};
        const int from = at + name.size() + 4;
        return request.mid(from, request.indexOf("\r\n", from) - from);
    }

    std::unique_ptr<QTemporaryDir> cacheDir_;
    QTcpServer server_;
    QString scenario_;
    QByteArray lastAuthorization_;
    QByteArray lastQuery_;
    int requests_ = 0;
    int propfinds_ = 0;
    int gets_ = 0;
    int listings_ = 0;
};

QTEST_MAIN(RemoteCacheTest)
#include "RemoteCacheTest.moc"
