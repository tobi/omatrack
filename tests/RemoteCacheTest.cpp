// The sync engine and its protocols, against a local HTTP server.
//
// One responder speaks both WebDAV and the S3 API, because the point of these
// tests is the shared engine: discovery, the ETag cache-hit test, the offline
// fallback, and the rules about which names may become files are the same code
// whichever protocol produced the listing.

#include "app/AimRemoteIndex.h"
#include "app/RemoteCache.h"

#include <QtTest>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QTemporaryDir>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>

#include <QCryptographicHash>

#include <atomic>
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
        QCOMPARE(result.skipped,
                 QStringList{QStringLiteral("notes:draft.vbo")});
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

        RemoteConnection connection =
            s3Connection(QStringLiteral("gs://team-telemetry/season-2026/"),
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
        QCOMPARE(
            normalizeTarget(LocationType::S3, QStringLiteral("s3://b/season")),
            canonical);
        QCOMPARE(normalizeTarget(LocationType::S3,
                                 QStringLiteral("  s3://b//season//  ")),
                 canonical);

        // A bucket is not a hostname. Lowercasing one — which is exactly what
        // routing this through QUrl would do — names a bucket that is not
        // there.
        QCOMPARE(normalizeTarget(LocationType::S3,
                                 QStringLiteral("s3://MyBucket/A/")),
                 QStringLiteral("s3://MyBucket/A/"));
    }

    /// One pasteable string carries the key, the secret, the region and a
    /// non-Amazon endpoint — and none of them stay in the target, because the
    /// connection id is a hash of it.
    void splitsAWholeAddressIntoItsParts() {
        const ConnectionAddress split = splitAddress(
            LocationType::S3,
            QStringLiteral("s3://AKIAIOSFODNN7EXAMPLE:wJalrXUtnFEMI%2FK7MDENG"
                           "%2FbPxRfiCY@MyBucket/season-2026"
                           "?region=eu-west-2&scheme=http"
                           "&endpoint_override=minio.example:9000"));
        QVERIFY2(split.error.isEmpty(), qPrintable(split.error));
        QCOMPARE(split.target, QStringLiteral("s3://MyBucket/season-2026/"));
        QCOMPARE(split.username, QStringLiteral("AKIAIOSFODNN7EXAMPLE"));
        // A secret access key routinely contains a slash, which only survives
        // the authority as %2F.
        QCOMPARE(split.password,
                 QStringLiteral("wJalrXUtnFEMI/K7MDENG/bPxRfiCY"));
        QCOMPARE(split.options.value(QStringLiteral("region")),
                 QStringLiteral("eu-west-2"));
        QCOMPARE(split.options.value(QStringLiteral("endpoint")),
                 QStringLiteral("http://minio.example:9000"));

        // gs:// says the same things, and the address the target keeps is the
        // one a bare bucket would have produced.
        const ConnectionAddress google = splitAddress(
            LocationType::Gcs,
            QStringLiteral("gs://GOOG1EKEY:s3cret@lap_data/2026/"));
        QVERIFY(google.error.isEmpty());
        QCOMPARE(google.target, QStringLiteral("gs://lap_data/2026/"));
        QCOMPARE(google.username, QStringLiteral("GOOG1EKEY"));
        QCOMPARE(google.password, QStringLiteral("s3cret"));
        QVERIFY(google.options.isEmpty());

        // An address with nothing extra in it is left alone, so nothing
        // already configured changes identity and loses its cache.
        const ConnectionAddress plain = splitAddress(
            LocationType::S3, QStringLiteral("s3://team-telemetry/season/"));
        QCOMPARE(plain.target, QStringLiteral("s3://team-telemetry/season/"));
        QVERIFY(plain.username.isEmpty());
        QVERIFY(plain.password.isEmpty());

        // WebDAV carries a credential the same way, and QUrl parses that
        // authority correctly because a host really is a host.
        const ConnectionAddress dav = splitAddress(
            LocationType::WebDav,
            QStringLiteral("https://ayrton:m0naco@server.example/dav/"));
        QCOMPARE(dav.target, QStringLiteral("https://server.example/dav/"));
        QCOMPARE(dav.username, QStringLiteral("ayrton"));
        QCOMPARE(dav.password, QStringLiteral("m0naco"));

        // A mistyped parameter is reported rather than ignored: silently
        // dropping `reigon` means signing against the wrong region and an
        // error nobody can trace back to the typo.
        const ConnectionAddress typo = splitAddress(
            LocationType::S3, QStringLiteral("s3://bucket/x?reigon=eu-west-2"));
        QVERIFY(typo.error.contains(QStringLiteral("reigon")));
        QVERIFY(!validateTarget(LocationType::S3,
                                QStringLiteral("s3://bucket/x?reigon=eu"))
                     .isEmpty());
        QVERIFY(!splitAddress(LocationType::S3,
                              QStringLiteral("s3://bucket/x?scheme=ftp"))
                     .error.isEmpty());
    }

    /// The whole point of the syntax: paste one string and the sync works.
    void syncsFromAWholeAddress() {
        scenario_ = QStringLiteral("plain");
        const ConnectionAddress split = splitAddress(
            LocationType::S3,
            QStringLiteral("s3://AKIAIOSFODNN7EXAMPLE:secret@team-telemetry/"
                           "?region=eu-west-2&scheme=http&endpoint_override="
                           "127.0.0.1:%1")
                .arg(server_.serverPort()));
        QVERIFY2(split.error.isEmpty(), qPrintable(split.error));

        RemoteConnection connection;
        connection.type = LocationType::S3;
        connection.target = split.target;
        connection.username = split.username;
        connection.password = split.password;
        connection.options = split.options;
        connection.id =
            locationId(split.target + QStringLiteral("address"), {});

        const RemoteSyncResult result = syncConnection(connection);
        QVERIFY2(result.success, qPrintable(result.error));
        QVERIFY(result.files.contains(
            QStringLiteral("season-2026/brands-hatch.vbo")));
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
        QVERIFY(
            query.contains(QStringLiteral("eu-west-2%2Fs3%2Faws4_request")));

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

    void keepsSignedUrlsOutOfDownloadErrors() {
        scenario_ = QStringLiteral("video");
        const RemoteConnection connection =
            s3Connection(QStringLiteral("s3://team-telemetry/"),
                         QStringLiteral("missing-video"));
        const RemoteSyncResult sync = syncConnection(connection);
        QVERIFY2(sync.success, qPrintable(sync.error));
        const QString stub =
            QDir(sync.cachePath).filePath(QStringLiteral("onboard.mp4"));
        QCOMPARE(pinOfflineVideo(connection, stub, true), QString());

        scenario_ = QStringLiteral("missing-object");
        const QString error = fetchObject(connection, stub, {});
        QVERIFY(!error.isEmpty());
        QVERIFY(!error.contains(QStringLiteral("X-Amz-")));
        QVERIFY(!error.contains(QStringLiteral("Secret")));
        QVERIFY(error.contains(QStringLiteral("404")));
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
        const QString oldest =
            write(directory, "oldest.vbo", 40'000, now.addSecs(-3000));
        const QString older =
            write(directory, "older.vbo", 40'000, now.addSecs(-2000));
        const QString openNow =
            write(directory, "playing.vbo", 40'000, now.addSecs(-4000));
        const QString newest =
            write(directory, "newest.vbo", 40'000, now.addSecs(-10));
        const QString stub =
            write(directory, "onboard.mp4", 0, now.addSecs(-5000));
        const QString index =
            write(directory, "index.json", 40'000, now.addSecs(-6000));

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
        QCOMPARE(again.files.size(), 2);
    }

    void decodesKeysWhenEncodingTypeFollowsContents() {
        gets_ = 0;
        listings_ = 0;
        scenario_ = QStringLiteral("late-encoding");
        const RemoteConnection connection =
            s3Connection(QStringLiteral("s3://team-telemetry/season-2026/"),
                         QStringLiteral("late-encoding"));
        const RemoteSyncResult result = syncConnection(connection);
        QVERIFY2(result.success, qPrintable(result.error));
        QCOMPARE(result.files, QStringList{QStringLiteral("brands-hatch.vbo")});
        QCOMPARE(listings_, 1);
        QCOMPARE(gets_, 1);
    }

    void downloadsRecordingSidecarsWithoutListingThemAsSources() {
        QVERIFY(isSidecarPath(QStringLiteral(".session.mp4.telemetry")));
        QVERIFY(isSidecarPath(QStringLiteral("event/.session.mp4.telemetry")));
        QVERIFY(isSidecarPath(QStringLiteral(".session.mp4.json")));
        QVERIFY(isSidecarPath(QStringLiteral("event/.session.mp4.json")));
        QVERIFY(isSidecarPath(QStringLiteral(".session.mp4.ld")));
        QVERIFY(isSidecarPath(QStringLiteral("event/.session.mp4.ldx")));
        QVERIFY(isSidecarPath(QStringLiteral(".omatrack/aim-e1.mp4")));
        QVERIFY(!isSidecarPath(QStringLiteral("session.vbo")));
        QVERIFY(!isSidecarPath(QStringLiteral("session.ld")));

        gets_ = 0;
        listings_ = 0;
        lastPayloadHash_.clear();
        scenario_ = QStringLiteral("recording-sidecar");
        const RemoteConnection connection =
            s3Connection(QStringLiteral("s3://team-telemetry/"),
                         QStringLiteral("recording-sidecar"));

        const RemoteSyncResult result = syncConnection(connection);
        QVERIFY2(result.success, qPrintable(result.error));
        QCOMPARE(result.files, QStringList{QStringLiteral("session.mp4")});
        QVERIFY(isPortableTelemetryCompanion(
            QStringLiteral(".session.mp4.telemetry")));
        QVERIFY(
            !isPortableTelemetryCompanion(QStringLiteral(".session.mp4.json")));
        const QString sidecar =
            QDir(result.cachePath)
                .filePath(QStringLiteral(".session.mp4.telemetry"));
        QVERIFY(QFileInfo::exists(sidecar));
        QFile file(sidecar);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArrayLiteral("native-companion"));

        const QByteArray body = QByteArrayLiteral("updated-companion");
        QCOMPARE(putObject(connection, QStringLiteral(".session.mp4.telemetry"),
                           body),
                 QString());
        QVERIFY(!lastPayloadHash_.isEmpty());
        QCOMPARE(
            lastPayloadHash_,
            QCryptographicHash::hash(body, QCryptographicHash::Sha256).toHex());
    }

    void createOnlyPutKeepsTheExistingObject() {
        lastIfNoneMatch_.clear();
        scenario_ = QStringLiteral("create-only");
        const RemoteConnection connection =
            s3Connection(QStringLiteral("s3://team-telemetry/"),
                         QStringLiteral("create-only"));
        const QByteArray ours = QByteArrayLiteral("new-companion");
        QCOMPARE(putObject(connection, QStringLiteral(".session.mp4.ld"), ours),
                 QString());
        QCOMPARE(lastIfNoneMatch_, QByteArrayLiteral("*"));
        const QString local = QDir(cacheDirectory(connection))
                                  .filePath(QStringLiteral(".session.mp4.ld"));
        QFile file(local);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), QByteArrayLiteral("existing-companion"));
    }

    void ignoresLegacyEtagSidecarsOnTheServer() {
        gets_ = 0;
        listings_ = 0;
        scenario_ = QStringLiteral("sidecar");
        const RemoteConnection connection = s3Connection(
            QStringLiteral("s3://team-telemetry/"), QStringLiteral("sidecar"));

        const RemoteSyncResult result = syncConnection(connection);
        QVERIFY2(result.success, qPrintable(result.error));
        QCOMPARE(result.files, QStringList{QStringLiteral("session.vbo")});
        QVERIFY(!QFileInfo::exists(
            QDir(result.cachePath)
                .filePath(QStringLiteral(".omatrack/e1.json"))));
        QCOMPARE(gets_, 1);
    }

    void aRaisedCancelDoesNotTouchTheNetwork() {
        scenario_ = QStringLiteral("video");
        RemoteConnection connection = s3Connection(
            QStringLiteral("s3://team-telemetry/"), QStringLiteral("cancel"));
        auto cancel = std::make_shared<std::atomic<bool>>(true);
        const RemoteSyncResult result = syncConnection(connection, cancel);
        QCOMPARE(result.error, QStringLiteral("Cancelled"));
        QVERIFY(!result.success);
    }

    void classifiesVideoContainersAndLocationTypes() {
        QVERIFY(isVideoFile(QStringLiteral("onboard.mp4")));
        QVERIFY(isVideoFile(QStringLiteral("Onboard.MOV")));
        QVERIFY(isVideoFile(QStringLiteral("/laps/race.mkv")));
        QVERIFY(isVideoFile(QStringLiteral("clip.webm")));
        QVERIFY(isVideoFile(QStringLiteral("tape.avi")));
        QVERIFY(isVideoFile(QStringLiteral("phone.m4v")));
        QVERIFY(!isVideoFile(QStringLiteral("session.vbo")));
        QVERIFY(!isVideoFile(QStringLiteral("session.mp4.bak")));
        QVERIFY(!isVideoFile(QStringLiteral("mp4")));

        QCOMPARE(locationTypeKey(LocationType::Folder),
                 QStringLiteral("folder"));
        QCOMPARE(locationTypeKey(LocationType::WebDav),
                 QStringLiteral("webdav"));
        QCOMPARE(locationTypeKey(LocationType::S3), QStringLiteral("s3"));
        QCOMPARE(locationTypeKey(LocationType::Gcs), QStringLiteral("gcs"));
        bool ok = false;
        QCOMPARE(locationTypeFromKey(QStringLiteral("s3"), &ok),
                 LocationType::S3);
        QVERIFY(ok);
        QCOMPARE(locationTypeFromKey(QStringLiteral("nope"), &ok),
                 LocationType::Folder);
        QVERIFY(!ok);
        QCOMPARE(parseByteSize(QStringLiteral("2 tib"), 0), 2LL << 40);
        QCOMPARE(parseByteSize(QStringLiteral("4KiB"), 0), 4LL << 10);
        QCOMPARE(etagFileKey(QString()), QString());
        QCOMPARE(etagFileKey(QStringLiteral("*")), QString());
        QCOMPARE(etagFileKey(QStringLiteral("\"abc/1\"")),
                 QStringLiteral("abc_1"));
        QVERIFY(isSidecarPath(QStringLiteral(".omatrack/aim-abc_1.mp4")));
        QVERIFY(!isSidecarPath(QStringLiteral("omatrack/anything")));
    }

    void followsARedirectAndStopsALoop() {
        QNetworkAccessManager unused;
        const auto build = [](const QUrl& url) { return makeRequest(url); };
        const QUrl plain(QStringLiteral("http://127.0.0.1:%1/plain")
                             .arg(server_.serverPort()));
        const HttpResponse direct = sendFollowing(unused, plain, "GET", build);
        QCOMPARE(direct.status, 200);
        QCOMPARE(direct.body, QByteArrayLiteral("plain-body"));

        const QUrl bounce(QStringLiteral("http://127.0.0.1:%1/redirect")
                              .arg(server_.serverPort()));
        const HttpResponse followed =
            sendFollowing(unused, bounce, "GET", build);
        QCOMPARE(followed.status, 200);
        QCOMPARE(followed.body, QByteArrayLiteral("plain-body"));

        const QUrl loop(QStringLiteral("http://127.0.0.1:%1/loop")
                            .arg(server_.serverPort()));
        const HttpResponse looping = sendFollowing(unused, loop, "GET", build);
        QCOMPARE(looping.error, QStringLiteral("Too many redirects"));
    }

    void cancelAbandonsAHangingGet() {
        QNetworkAccessManager unused;
        const auto build = [](const QUrl& url) { return makeRequest(url); };
        auto cancel = std::make_shared<std::atomic<bool>>(false);
        QTimer::singleShot(80, this, [cancel]() { cancel->store(true); });
        const HttpResponse response =
            sendFollowing(unused,
                          QUrl(QStringLiteral("http://127.0.0.1:%1/hang")
                                   .arg(server_.serverPort())),
                          "GET", build, {}, cancel);
        QCOMPARE(response.error, QStringLiteral("Cancelled"));
    }

    void rangeGetReturnsTheRequestedSlice() {
        const RemoteConnection connection = s3Connection(
            QStringLiteral("s3://team-telemetry/"), QStringLiteral("range"));
        const QUrl url(QStringLiteral("http://127.0.0.1:%1/blob")
                           .arg(server_.serverPort()));
        QString error;
        const QByteArray slice = getObjectRange(connection, url, 10, 8, &error);
        QVERIFY2(!slice.isEmpty(), qPrintable(error));
        QCOMPARE(slice.size(), 8);
        QCOMPARE(uchar(slice[0]), uchar(10));
        QCOMPARE(uchar(slice[7]), uchar(17));
        QVERIFY(lastRange_.startsWith("bytes=10-17"));

        QVector<QByteArray> bodies;
        QVERIFY(getObjectRanges(connection, url, {{0, 4}, {200, 6}}, &bodies,
                                &error));
        QCOMPARE(bodies.size(), 2);
        QCOMPARE(bodies[0].size(), 4);
        QCOMPARE(uchar(bodies[0][0]), uchar(0));
        QCOMPARE(bodies[1].size(), 6);
        QCOMPARE(uchar(bodies[1][0]), uchar(200));
    }

    void rangeGetRejectsAFolderAndARaisedCancel() {
        RemoteConnection folder;
        folder.type = LocationType::Folder;
        QString error;
        QVERIFY(!getObjectRanges(folder,
                                 QUrl(QStringLiteral("http://127.0.0.1/blob")),
                                 {{0, 4}}, nullptr, &error));
        QCOMPARE(error, QStringLiteral("Invalid range request"));

        QVector<QByteArray> bodies;
        QVERIFY(!getObjectRanges(folder,
                                 QUrl(QStringLiteral("http://127.0.0.1/blob")),
                                 {{0, 4}}, &bodies, &error));
        QCOMPARE(error, QStringLiteral("No protocol backend"));

        auto cancel = std::make_shared<std::atomic<bool>>(true);
        const RemoteConnection connection = s3Connection(
            QStringLiteral("s3://team-telemetry/"), QStringLiteral("range-x"));
        QVERIFY(!getObjectRanges(connection,
                                 QUrl(QStringLiteral("http://127.0.0.1:%1/blob")
                                          .arg(server_.serverPort())),
                                 {{0, 4}}, &bodies, &error, cancel));
        QCOMPARE(error, QStringLiteral("Cancelled"));
    }

    void materializesAnAimExtractFromRangeGets() {
        const QByteArray mp4 = packedAimMp4();
        QVERIFY(mp4.size() > 80);
        aimMp4_ = mp4;

        const RemoteConnection connection = s3Connection(
            QStringLiteral("s3://team-telemetry/"), QStringLiteral("aim"));
        const QString cache = cacheDirectory(connection);
        QVERIFY(QDir().mkpath(cache));
        const QString stub =
            QDir(cache).filePath(QStringLiteral("onboard.mp4"));
        QVERIFY(QFile(stub).open(QIODevice::WriteOnly));
        QJsonObject entries;
        entries.insert(
            QStringLiteral("onboard.mp4"),
            QJsonObject{{QStringLiteral("etag"), QStringLiteral("e-aim")},
                        {QStringLiteral("size"), mp4.size()},
                        {QStringLiteral("stream"), true},
                        {QStringLiteral("url"),
                         QStringLiteral("http://127.0.0.1:%1/aim.mp4")
                             .arg(server_.serverPort())}});
        QFile index(QDir(cache).filePath(QStringLiteral("index.json")));
        QVERIFY(index.open(QIODevice::WriteOnly | QIODevice::Truncate));
        const QByteArray json =
            QJsonDocument(QJsonObject{{QStringLiteral("entries"), entries}})
                .toJson();
        QCOMPARE(index.write(json), qint64(json.size()));
        index.close();

        auto already = std::make_shared<std::atomic<bool>>(true);
        QCOMPARE(materializeAimExtract(connection, stub,
                                       QStringLiteral("e-aim"), already),
                 QStringLiteral("Cancelled"));

        QString error =
            materializeAimExtract(connection, stub, QStringLiteral("e-aim"));
        QVERIFY2(error.isEmpty(), qPrintable(error));
        const QString extract =
            aimExtractPath(connection, QStringLiteral("e-aim"));
        QVERIFY(QFileInfo(extract).size() > 0);
        QCOMPARE(telemetryOpenPath(&connection, stub), extract);

        // A second call is a cache hit.
        QCOMPARE(
            materializeAimExtract(connection, stub, QStringLiteral("e-aim")),
            QString());
        QCOMPARE(materializeAimExtract(connection, stub, {}),
                 QStringLiteral("Remote video has no ETag"));
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
        QVERIFY2(before.bytes > 4096,
                 qPrintable(QString::number(before.bytes)));

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

    static void appendBe32(QByteArray* data, quint32 value) {
        const qsizetype offset = data->size();
        data->resize(offset + 4);
        (*data)[offset] = char((value >> 24U) & 0xffU);
        (*data)[offset + 1] = char((value >> 16U) & 0xffU);
        (*data)[offset + 2] = char((value >> 8U) & 0xffU);
        (*data)[offset + 3] = char(value & 0xffU);
    }

    static QByteArray box(const char* type, const QByteArray& payload) {
        QByteArray out;
        appendBe32(&out, quint32(8 + payload.size()));
        out.append(type, 4);
        out.append(payload);
        return out;
    }

    static QByteArray fullBox(const char* type, const QByteArray& payload) {
        QByteArray body;
        appendBe32(&body, 0);
        body.append(payload);
        return box(type, body);
    }

    static QByteArray packedAimMp4() {
        QByteArray ftypPayload("isom");
        appendBe32(&ftypPayload, 0);
        ftypPayload.append("isom", 4);
        const QByteArray ftyp = box("ftyp", ftypPayload);

        QByteArray aimd(8, '\0');
        aimd[7] = 1;
        QByteArray stsdPayload;
        appendBe32(&stsdPayload, 1);
        stsdPayload.append(box("aimd", aimd));
        const QByteArray stsd = fullBox("stsd", stsdPayload);

        QByteArray stszPayload;
        appendBe32(&stszPayload, 0);
        appendBe32(&stszPayload, 2);
        appendBe32(&stszPayload, 4);
        appendBe32(&stszPayload, 4);
        const QByteArray stsz = fullBox("stsz", stszPayload);

        QByteArray stscPayload;
        appendBe32(&stscPayload, 1);
        appendBe32(&stscPayload, 1);
        appendBe32(&stscPayload, 2);
        appendBe32(&stscPayload, 1);
        const QByteArray stsc = fullBox("stsc", stscPayload);

        const qint64 sampleOffset = ftyp.size() + 8;
        QByteArray stcoPayload;
        appendBe32(&stcoPayload, 1);
        appendBe32(&stcoPayload, quint32(sampleOffset));
        const QByteArray stco = fullBox("stco", stcoPayload);

        const QByteArray moov = box(
            "moov",
            box("trak", box("mdia", box("minf", box("stbl", stsd + stsz + stsc +
                                                                stco)))));
        QByteArray mdat;
        appendBe32(&mdat, 16);
        mdat.append("mdat", 4);
        mdat.append("AIM0");
        mdat.append("AIM1");
        return ftyp + mdat + moov;
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
        } else if (scenario_ == QStringLiteral("sidecar")) {
            body += "<IsTruncated>false</IsTruncated>";
            body +=
                contents(QStringLiteral("session.vbo"), QStringLiteral("e1"));
            body += contents(QStringLiteral(".omatrack/e1.json"),
                             QStringLiteral("meta1"));
        } else if (scenario_ == QStringLiteral("recording-sidecar")) {
            body += "<IsTruncated>false</IsTruncated>";
            body +=
                contents(QStringLiteral("session.mp4"), QStringLiteral("e1"));
            body += contents(QStringLiteral(".session.mp4.json"),
                             QStringLiteral("meta1"));
            body += contents(QStringLiteral(".session.mp4.telemetry"),
                             QStringLiteral("tele1"));
        } else if (scenario_ == QStringLiteral("late-encoding")) {
            body =
                "<?xml version=\"1.0\"?><ListBucketResult>"
                "<IsTruncated>false</IsTruncated>";
            body += contents(QStringLiteral("season-2026/brands-hatch.vbo"),
                             QStringLiteral("e1"));
            body += "<EncodingType>url</EncodingType>";
        } else {
            body += "<IsTruncated>false</IsTruncated>";
            body += contents(QStringLiteral("season-2026/brands-hatch.vbo"),
                             QStringLiteral("e1"));
            body += contents(QStringLiteral("season-2026/spa/lap 2.vbo"),
                             QStringLiteral("e2"));
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
        QByteArray extraHeaders;
        const int queryAt = target.indexOf('?');
        const QByteArray pathOnly = queryAt < 0 ? target : target.left(queryAt);
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
            QString body = QStringLiteral(
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
        } else if (request.startsWith("PUT ")) {
            lastPayloadHash_ = headerOf(request, "x-amz-content-sha256");
            lastIfNoneMatch_ = headerOf(request, "If-None-Match");
            payload.clear();
            status = scenario_ == QStringLiteral("create-only")
                         ? "412 Precondition Failed"
                         : "200 OK";
        } else if (pathOnly == "/plain") {
            ++gets_;
            payload = QByteArrayLiteral("plain-body");
            status = "200 OK";
        } else if (pathOnly == "/redirect") {
            ++gets_;
            extraHeaders = "Location: http://127.0.0.1:" +
                           QByteArray::number(server_.serverPort()) +
                           "/plain\r\n";
            status = "302 Found";
        } else if (pathOnly == "/loop") {
            extraHeaders = "Location: http://127.0.0.1:" +
                           QByteArray::number(server_.serverPort()) +
                           "/loop\r\n";
            status = "302 Found";
        } else if (pathOnly == "/hang") {
            ++gets_;
            return;
        } else if (pathOnly == "/blob" || pathOnly == "/aim.mp4") {
            ++gets_;
            const QByteArray data =
                pathOnly == "/aim.mp4" ? aimMp4_ : blobPayload();
            QByteArray range = headerOf(request, "Range");
            lastRange_ = range;
            qint64 start = 0;
            qint64 end = data.size() - 1;
            if (range.startsWith("bytes=")) {
                const QByteArray spec = range.mid(6);
                const int dash = spec.indexOf('-');
                start = spec.left(dash).toLongLong();
                const QByteArray endPart = spec.mid(dash + 1);
                if (!endPart.isEmpty()) end = endPart.toLongLong();
                end = qMin(end, qint64(data.size()) - 1);
                payload = data.mid(int(start), int(end - start + 1));
                extraHeaders = "Content-Range: bytes " +
                               QByteArray::number(start) + "-" +
                               QByteArray::number(end) + "/" +
                               QByteArray::number(data.size()) + "\r\n";
                status = "206 Partial Content";
            } else {
                payload = data;
                status = "200 OK";
            }
        } else if (target.startsWith("/team-telemetry/.session.mp4.ld")) {
            ++gets_;
            payload = QByteArrayLiteral("existing-companion");
            status = "200 OK";
        } else if (target.startsWith("/team-telemetry/.session.mp4.json")) {
            ++gets_;
            payload = QByteArrayLiteral("{\"supported\":true}");
            status = "200 OK";
        } else if (target.startsWith(
                       "/team-telemetry/.session.mp4.telemetry")) {
            ++gets_;
            payload = QByteArrayLiteral("native-companion");
            status = "200 OK";
        } else if (target.startsWith("/team-telemetry/")) {
            ++gets_;
            lastAuthorization_ = headerOf(request, "Authorization");
            if (scenario_ == QStringLiteral("missing-object")) {
                payload = QByteArrayLiteral("missing");
                status = "404 Not Found";
            } else {
                payload = QByteArrayLiteral("telemetry");
                status = "200 OK";
            }
        }

        QByteArray response = "HTTP/1.1 ";
        response += status;
        response += "\r\nConnection: close\r\n";
        response += extraHeaders;
        response += "Content-Type: application/xml\r\nContent-Length: ";
        response += QByteArray::number(payload.size());
        response += "\r\n\r\n";
        response += payload;
        socket->write(response);
        socket->disconnectFromHost();
    }

    static QByteArray blobPayload() {
        QByteArray data(256, '\0');
        for (int i = 0; i < data.size(); ++i) data[i] = char(i);
        return data;
    }

    static QByteArray headerOf(const QByteArray& request,
                               const QByteArray& name) {
        const QByteArray lowered = request.toLower();
        const QByteArray needle = "\r\n" + name.toLower() + ": ";
        const int at = lowered.indexOf(needle);
        if (at < 0) return {};
        const int from = at + needle.size();
        return request.mid(from, request.indexOf("\r\n", from) - from);
    }

    std::unique_ptr<QTemporaryDir> cacheDir_;
    QTcpServer server_;
    QString scenario_;
    QByteArray lastPayloadHash_;
    QByteArray lastIfNoneMatch_;
    QByteArray lastAuthorization_;
    QByteArray lastQuery_;
    QByteArray lastRange_;
    QByteArray aimMp4_;
    int requests_ = 0;
    int propfinds_ = 0;
    int gets_ = 0;
    int listings_ = 0;
};

QTEST_MAIN(RemoteCacheTest)
#include "RemoteCacheTest.moc"
