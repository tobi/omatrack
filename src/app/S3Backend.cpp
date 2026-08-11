// S3: Amazon's object-storage API, and everything that reimplements it.
//
// One backend covers more than one service on purpose. MinIO, Cloudflare R2
// and Backblaze all speak this API, and so does Google Cloud Storage when
// addressed through its XML endpoint with an HMAC interoperability key. What
// actually varies between them is three things — endpoint, addressing style,
// and region — so those are parameters rather than separate backends.

#include "RemoteCache.h"
#include "SigV4.h"

#include <QDateTime>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QUrlQuery>
#include <QXmlStreamReader>

#include <utility>

namespace omatrack {
namespace {

/// Buckets can hold a great many objects; this bounds the paging loop so a
/// server that keeps handing back continuation tokens cannot spin forever.
constexpr int kMaximumPages = 1000;

/// A bucket name is not a hostname, so the target is split by hand. QUrl
/// lowercases an authority — it would quietly rewrite a mixed-case bucket
/// into one that does not exist and report no error at all.
struct BucketPath {
    QString bucket;
    /// Empty, or slash-terminated. A prefix names a folder, which is what a
    /// WebDAV collection URL means too.
    QString prefix;
};

/// Where requests go and how the bucket is addressed.
struct Endpoint {
    QUrl origin;
    bool pathStyle = false;
};

QString schemeFor(LocationType type) {
    switch (type) {
        case LocationType::S3: return QStringLiteral("s3");
        case LocationType::Gcs: return QStringLiteral("gs");
        case LocationType::WebDav:
        case LocationType::Folder: break;
    }
    return {};
}

BucketPath parseTarget(const QString& target) {
    QString rest = target.trimmed();
    const int scheme = rest.indexOf(QStringLiteral("://"));
    if (scheme >= 0) rest = rest.mid(scheme + 3);

    BucketPath parsed;
    const int slash = rest.indexOf(QLatin1Char('/'));
    if (slash < 0) {
        parsed.bucket = rest;
        return parsed;
    }
    parsed.bucket = rest.left(slash);
    // A run of slashes is a typo, not a key: S3 would treat `a//b` as a
    // distinct object, but nobody types one meaning that, and leaving them in
    // would give one folder two spellings and therefore two caches.
    for (const QString& segment :
         rest.mid(slash + 1).split(QLatin1Char('/'), Qt::SkipEmptyParts))
        parsed.prefix.append(segment).append(QLatin1Char('/'));
    return parsed;
}

/// Deliberately looser than the AWS naming rules: GCS allows underscores and
/// longer names, and a bucket this rejects is one the user cannot connect to
/// at all. Anything the address syntax can carry unambiguously is allowed
/// through, and the server gets the final say.
bool isUsableBucket(const QString& bucket) {
    if (bucket.size() < 3 || bucket.size() > 222) return false;
    if (bucket.startsWith(QLatin1Char('.')) ||
        bucket.endsWith(QLatin1Char('.')))
        return false;
    for (const QChar character : bucket) {
        const char16_t code = character.unicode();
        const bool allowed = (code >= u'a' && code <= u'z') ||
                             (code >= u'A' && code <= u'Z') ||
                             (code >= u'0' && code <= u'9') || code == u'.' ||
                             code == u'-' || code == u'_';
        if (!allowed) return false;
    }
    return true;
}

QString defaultHost(LocationType type, const QString& region) {
    // Google's S3-compatible XML endpoint. It is one host for every bucket
    // and every region, which is why GCS needs no region discovery.
    if (type == LocationType::Gcs)
        return QStringLiteral("storage.googleapis.com");
    return region.isEmpty() ? QStringLiteral("s3.amazonaws.com")
                            : QStringLiteral("s3.%1.amazonaws.com").arg(region);
}

Endpoint resolveEndpoint(const RemoteConnection& connection,
                         const QString& bucket, const QString& region) {
    const QString custom =
        connection.options.value(QStringLiteral("endpoint")).trimmed();
    if (!custom.isEmpty()) {
        QUrl origin(custom.contains(QStringLiteral("://"))
                        ? custom
                        : QStringLiteral("https://") + custom);
        origin.setPath({});
        origin.setQuery(QString());
        // A custom endpoint is a MinIO box, an R2 account, or a test server.
        // Virtual-host addressing there needs wildcard DNS and a wildcard
        // certificate, which such a deployment almost never has.
        return {origin, true};
    }
    const QUrl origin(QStringLiteral("https://") +
                      defaultHost(connection.type, region));
    // AWS prefers virtual-host addressing, but a dot in the bucket name makes
    // `bucket.s3.region.amazonaws.com` fail certificate validation: a
    // wildcard certificate only covers a single label.
    // Google publishes no per-bucket hostnames, so GCS is always path-style.
    return {origin, bucket.contains(QLatin1Char('.')) ||
                        connection.type == LocationType::Gcs};
}

/// Sets a path or query that is already in SigV4's encoding, without letting
/// Qt re-encode it.
///
/// This is not fussiness. Qt leaves `/` and `+` raw in a query string, and a
/// continuation token is base64 — full of both. A raw `+` is read as a space
/// by anything applying form-decoding rules, which loses the token outright.
/// Signing and sending the same bytes removes the question.
void setEncodedPath(QUrl* url, const QByteArray& encoded) {
    url->setPath(QString::fromLatin1(encoded), QUrl::StrictMode);
}

void setEncodedQuery(QUrl* url,
                     const QList<std::pair<QString, QString>>& parameters) {
    QByteArray query;
    for (const auto& [name, value] : parameters) {
        if (!query.isEmpty()) query.append('&');
        query.append(sigv4::uriEncode(name, false))
            .append('=')
            .append(sigv4::uriEncode(value, false));
    }
    url->setQuery(QString::fromLatin1(query), QUrl::StrictMode);
}

QUrl objectUrl(const Endpoint& endpoint, const QString& bucket,
               const QString& key) {
    QUrl url = endpoint.origin;
    if (endpoint.pathStyle) {
        setEncodedPath(&url, '/' + sigv4::uriEncode(bucket, false) + '/' +
                                 sigv4::uriEncode(key, true));
    } else {
        url.setHost(bucket + QLatin1Char('.') + endpoint.origin.host());
        setEncodedPath(&url, '/' + sigv4::uriEncode(key, true));
    }
    return url;
}

sigv4::Credentials credentialsOf(const RemoteConnection& connection) {
    return {connection.username.trimmed(), connection.password};
}

/// Signs in place. An empty key pair means a public bucket, which is read
/// with no Authorization header at all rather than a broken one.
void signRequest(QNetworkRequest& request, const QByteArray& method,
                 const RemoteConnection& connection, const QString& region) {
    request.setRawHeader("User-Agent", "Omatrack-S3/1");
    const sigv4::Credentials credentials = credentialsOf(connection);
    if (credentials.isEmpty()) return;

    const sigv4::HeaderMap headers = sigv4::signedHeaders(
        credentials, {region}, method, request.url(), sigv4::kEmptyPayload,
        QDateTime::currentDateTimeUtc());
    for (auto it = headers.cbegin(); it != headers.cend(); ++it) {
        // Qt derives Host from the URL. Setting it again would send it twice,
        // and the signature already commits to the same value.
        if (it.key() == "host") continue;
        request.setRawHeader(it.key(), it.value());
    }
}

RequestFactory factoryFor(const RemoteConnection& connection,
                          const QByteArray& method, const QString& region) {
    return [connection, method, region](const QUrl& url) {
        QNetworkRequest request = makeRequest(url);
        signRequest(request, method, connection, region);
        return request;
    };
}

/// AWS answers a bucket's home region in a header — even on the 403 that an
/// unsigned probe earns, which is why this needs no credentials. Asking is
/// far better than guessing: a signature scoped to the wrong region comes
/// back as AuthorizationHeaderMalformed, which names nothing useful.
QString discoverRegion(QNetworkAccessManager& manager, const QString& bucket) {
    const QUrl probe(QStringLiteral("https://s3.amazonaws.com/") + bucket);
    const HttpResponse response =
        sendFollowing(manager, probe, "HEAD",
                      [](const QUrl& url) { return makeRequest(url); });
    return QString::fromLatin1(response.headers.value("x-amz-bucket-region"));
}

QString percentDecoded(const QString& value, bool encoded) {
    return encoded ? QUrl::fromPercentEncoding(value.toUtf8()) : value;
}

/// Turns an <Error> document into something worth showing a driver, or an
/// empty string when the response carried no such document. S3 puts the
/// actionable part in the code, not the message.
QString describeError(const QByteArray& body) {
    QString code;
    QString message;
    QXmlStreamReader xml(body);
    while (!xml.atEnd()) {
        xml.readNext();
        if (!xml.isStartElement()) continue;
        if (xml.name() == QStringLiteral("Code"))
            code = xml.readElementText();
        else if (xml.name() == QStringLiteral("Message"))
            message = xml.readElementText();
    }

    if (code == QStringLiteral("RequestTimeTooSkewed"))
        return QStringLiteral(
            "This computer's clock is too far off for the server to accept a "
            "request. Check the system time.");
    if (code == QStringLiteral("SignatureDoesNotMatch") ||
        code == QStringLiteral("InvalidAccessKeyId"))
        return QStringLiteral("The access key or secret was not accepted.");
    if (code == QStringLiteral("AccessDenied"))
        return QStringLiteral(
            "Access denied: this key may not list the bucket.");
    if (code == QStringLiteral("NoSuchBucket"))
        return QStringLiteral("That bucket does not exist.");
    if (code == QStringLiteral("PermanentRedirect") ||
        code == QStringLiteral("AuthorizationHeaderMalformed"))
        return QStringLiteral(
            "The bucket is in a different region. Set the region on this "
            "connection.");
    if (code.isEmpty()) return {};
    return message.isEmpty() ? code
                             : QStringLiteral("%1: %2").arg(code, message);
}

/// One page of ListObjectsV2. Returns the continuation token to ask for next,
/// or an empty string when the listing is complete.
bool parsePage(const QByteArray& body, const QString& prefix,
               QVector<RemoteObject>* objects, const Endpoint& endpoint,
               const QString& bucket, QString* nextToken, QString* error) {
    QXmlStreamReader xml(body);
    bool encoded = false;
    bool truncated = false;
    QString token;
    QString key;
    QString etag;
    QString modified;
    qint64 size = -1;
    bool inContents = false;

    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement()) {
            const QStringView name = xml.name();
            if (name == QStringLiteral("Contents")) {
                inContents = true;
                key.clear();
                etag.clear();
                modified.clear();
                size = -1;
            } else if (name == QStringLiteral("EncodingType")) {
                encoded = xml.readElementText() == QStringLiteral("url");
            } else if (name == QStringLiteral("IsTruncated")) {
                truncated = xml.readElementText() == QStringLiteral("true");
            } else if (name == QStringLiteral("NextContinuationToken")) {
                token = xml.readElementText();
            } else if (inContents && name == QStringLiteral("Key")) {
                key = xml.readElementText();
            } else if (inContents && name == QStringLiteral("ETag")) {
                etag = xml.readElementText();
            } else if (inContents && name == QStringLiteral("LastModified")) {
                modified = xml.readElementText();
            } else if (inContents && name == QStringLiteral("Size")) {
                size = xml.readElementText().toLongLong();
            }
        } else if (xml.isEndElement() &&
                   xml.name() == QStringLiteral("Contents")) {
            inContents = false;
            const QString decoded = percentDecoded(key, encoded);
            // A key ending in `/` is the placeholder the AWS console writes
            // to make a bucket look like it has folders. There is no file.
            if (decoded.isEmpty() || decoded.endsWith(QLatin1Char('/')))
                continue;
            if (!decoded.startsWith(prefix)) continue;

            RemoteObject object;
            object.relativePath = decoded.mid(prefix.size());
            if (object.relativePath.isEmpty()) continue;
            object.url = objectUrl(endpoint, bucket, decoded);
            object.etag = etag;
            object.modified = modified;
            object.size = size;
            objects->append(object);
        }
    }

    if (xml.hasError()) {
        if (error) *error = xml.errorString();
        return false;
    }
    // The token is encoded under the same rule as the keys.
    *nextToken = truncated ? percentDecoded(token, encoded) : QString();
    return true;
}

}  // namespace

QString s3TargetError(LocationType type, const QString& target) {
    const QString scheme = schemeFor(type);
    const QString trimmed = target.trimmed();
    const QString expected = scheme + QStringLiteral("://");
    if (!trimmed.startsWith(expected, Qt::CaseInsensitive))
        return QStringLiteral("Enter a %1 address").arg(expected);
    const BucketPath parsed = parseTarget(trimmed);
    if (!isUsableBucket(parsed.bucket))
        return QStringLiteral("%1%2 does not name a bucket")
            .arg(expected, parsed.bucket);
    return {};
}

QString s3NormalizedTarget(LocationType type, const QString& target) {
    const BucketPath parsed = parseTarget(target);
    // The scheme is lowercased and the prefix is slash-terminated so that the
    // handful of ways a driver might type one bucket all hash to one cache.
    // The bucket and prefix keep their case: object keys are case-sensitive,
    // and so are the buckets old enough to have a capital letter.
    return schemeFor(type) + QStringLiteral("://") + parsed.bucket +
           QLatin1Char('/') + parsed.prefix;
}

RemoteBackend makeS3Backend(const RemoteConnection& connection) {
    RemoteBackend backend;
    const BucketPath parsed = parseTarget(connection.target);

    // The region is resolved once per sync and captured, so the listing and
    // every download that follows sign against the same scope. It cannot live
    // in index.json: that file is a rebuildable content index, and a region
    // is connection state.
    auto region = std::make_shared<QString>(
        connection.options.value(QStringLiteral("region")).trimmed());
    // GCS accepts SigV4 scoped to the literal region "auto" against any
    // bucket, so there is nothing to discover and nothing to configure.
    if (region->isEmpty() && connection.type == LocationType::Gcs)
        *region = QStringLiteral("auto");

    backend.sign = [connection, region](QNetworkRequest& request,
                                        const QByteArray& method) {
        signRequest(request, method, connection, *region);
    };

    backend.list = [connection, parsed, region](QNetworkAccessManager& manager,
                                                QVector<RemoteObject>* objects,
                                                QString* error) {
        const bool custom =
            !connection.options.value(QStringLiteral("endpoint")).isEmpty();
        if (region->isEmpty() && !custom)
            *region = discoverRegion(manager, parsed.bucket);
        if (region->isEmpty()) *region = QStringLiteral("us-east-1");

        const Endpoint endpoint =
            resolveEndpoint(connection, parsed.bucket, *region);
        const RequestFactory build = factoryFor(connection, "GET", *region);

        QString token;
        for (int page = 0; page < kMaximumPages; ++page) {
            QUrl listing = endpoint.origin;
            setEncodedPath(&listing,
                           endpoint.pathStyle
                               ? '/' + sigv4::uriEncode(parsed.bucket, false)
                               : QByteArray("/"));
            if (!endpoint.pathStyle)
                listing.setHost(parsed.bucket + QLatin1Char('.') +
                                endpoint.origin.host());

            QList<std::pair<QString, QString>> parameters{
                {QStringLiteral("list-type"), QStringLiteral("2")},
                // Without this, a key holding a `&`, a `+`, or anything
                // non-ASCII comes back mangled inside the XML.
                {QStringLiteral("encoding-type"), QStringLiteral("url")}};
            if (!parsed.prefix.isEmpty())
                parameters.append({QStringLiteral("prefix"), parsed.prefix});
            if (!token.isEmpty())
                parameters.append(
                    {QStringLiteral("continuation-token"), token});
            setEncodedQuery(&listing, parameters);

            const HttpResponse response =
                sendFollowing(manager, listing, "GET", build);
            if (response.status != 200) {
                // The XML document is the better explanation whenever there
                // is one: Qt reports every 4xx as "server replied:
                // Forbidden", which names neither the cause nor the cure.
                const QString explained = describeError(response.body);
                if (error)
                    *error = !explained.isEmpty() ? explained
                             : !response.error.isEmpty()
                                 ? response.error
                                 : QStringLiteral(
                                       "The bucket listing returned "
                                       "HTTP %1")
                                       .arg(response.status);
                return false;
            }
            if (!parsePage(response.body, parsed.prefix, objects, endpoint,
                           parsed.bucket, &token, error))
                return false;
            if (token.isEmpty()) return true;
        }
        if (error) *error = QStringLiteral("The bucket listing did not finish");
        return false;
    };

    return backend;
}

}  // namespace omatrack
