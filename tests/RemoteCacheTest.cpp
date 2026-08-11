// The sync engine and its protocols, against a local HTTP server.
//
// One responder speaks both WebDAV and the S3 API, because the point of these
// tests is the shared engine: discovery, the ETag cache-hit test, the offline
// fallback, and the rules about which names may become files are the same code
// whichever protocol produced the listing.

#include "app/RemoteCache.h"

#include <QtTest>

#include <QFile>
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

private:
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
            payload = QStringLiteral(
                          "<?xml version=\"1.0\"?><multistatus "
                          "xmlns=\"DAV:\"><response><href>%1</href>"
                          "<propstat><prop><resourcetype><collection/>"
                          "</resourcetype></prop></propstat></response>"
                          "<response><href>%1session.vbo</href>"
                          "<propstat><prop><getetag>\"one\"</getetag>"
                          "<getlastmodified>now</getlastmodified>"
                          "<getcontentlength>9</getcontentlength>"
                          "</prop></propstat></response></multistatus>")
                          .arg(root)
                          .toUtf8();
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
