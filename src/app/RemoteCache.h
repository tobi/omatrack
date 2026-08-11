// Discovery and content cache for telemetry that lives on a remote server.
//
// One sync engine, one cache layout, one offline story — the parts that do not
// depend on the protocol. A protocol contributes two things: a way to list what
// the server holds, and a way to authenticate a request. Everything else
// (deciding what changed, streaming it to disk, pruning what the server
// dropped, falling back to the last good cache) is shared.
//
// Callers downstream of a sync only ever see a local directory, so nothing in
// the app has to learn which protocol produced a file.
#pragma once

#include "LibraryLocation.h"

#include <QByteArray>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVector>

#include <functional>

class QNetworkAccessManager;
class QNetworkRequest;

namespace omatrack {

/// One file on the far side of a connection.
struct RemoteObject {
    QUrl url;
    QString relativePath;
    /// Opaque server-side version. Empty means "always re-download".
    QString etag;
    /// Raw server-supplied timestamp, compared as a string only.
    QString modified;
    qint64 size = -1;
};

/// What reaching a server takes: the credential-bearing half of a
/// LibraryLocation, without the library's ordering or display concerns.
struct RemoteConnection {
    QString id;
    LocationType type = LocationType::WebDav;
    QString name;
    QString target;
    QString username;
    QString password;
    /// See LibraryLocation::options — protocol tuning kept out of the target.
    QMap<QString, QString> options;
};

struct RemoteSyncResult {
    QString id;
    QString cachePath;
    QString status;
    QString error;
    QStringList files;
    /// Objects the server offers that this filesystem cannot hold — an S3 key
    /// may legally contain `:` or `?`, which Windows will not create.
    QStringList skipped;
    qint64 downloadedBytes = 0;
    /// The server could not be reached and this is the previous cache.
    bool fromCache = false;
    bool success = false;
};

/// A protocol, as two callbacks rather than a class: sync() is straight-line
/// synchronous code, and a test can supply lambdas to drive the engine with no
/// network at all.
///
/// `sign` runs on every outgoing request, listing and download alike, and owns
/// both authentication and any protocol-specific headers.
struct RemoteBackend {
    std::function<bool(QNetworkAccessManager&, QVector<RemoteObject>*,
                       QString*)>
        list;
    std::function<void(QNetworkRequest&, const QByteArray& method)> sign;
    /// Whatever signing this protocol's requests obliged the backend to work
    /// out — for S3, the bucket's region. Meaningful only once list() has run.
    /// The sync records it so that presigning a stream URL later reproduces
    /// the same scope without another round trip.
    std::function<QString()> scope;
};

/// A blocking HTTP round trip. Backends share this with the engine so that
/// timeout and redirect handling stay in one place.
struct HttpResponse {
    int status = 0;
    QByteArray body;
    /// Lowercased header names. S3 answers a bucket's region here, including
    /// on the 403 an unsigned probe gets back.
    QMap<QByteArray, QByteArray> headers;
    QString error;
};

/// Builds a signed, fully-formed request for one URL. Called again per
/// redirect hop, which is why it takes the URL rather than being handed a
/// finished request: a request signature covers the URL it was made for.
using RequestFactory = std::function<QNetworkRequest(const QUrl&)>;

/// Base request carrying the policies every Omatrack request needs. Qt is told
/// not to follow redirects on its own — it would reuse headers signed for the
/// original URL, which both breaks the signature and can hand credentials to a
/// host that never earned them. sendFollowing() follows them deliberately,
/// rebuilding and re-signing each hop.
QNetworkRequest makeRequest(const QUrl& url);
HttpResponse sendFollowing(QNetworkAccessManager& manager, const QUrl& url,
                           const QByteArray& method,
                           const RequestFactory& build,
                           const QByteArray& body = {});

/// Stable identity for a library entry, connection or folder alike. The input
/// is unchanged from when WebDAV was the only protocol, so no configured
/// location loses the cache it already downloaded.
QString locationId(const QString& target, const QString& username);

/// Where a connection's downloads live. Empty for a plain folder.
QString cacheDirectory(const RemoteConnection& connection);

/// The root under which every protocol's caches sit.
QString cacheRoot();

/// Bytes currently held in every protocol's cache.
///
/// Measured from the filesystem rather than summed from index entries, which
/// is the only way it can be honest: it also counts the caches of locations
/// that have since been removed and the temporary files a download that died
/// mid-write left behind. Both are exactly what someone checking this number
/// wants to know about.
qint64 cacheUsageBytes();

/// Deletes every downloaded file, and returns how many bytes that freed.
/// Nothing here cannot be fetched again.
qint64 clearCache();

/// True when `path` names a video container this application plays.
///
/// The sync needs to agree with the library about this, because video is the
/// one thing a connection streams instead of downloading — two lists that
/// drifted apart would mean either a 30 GB download or a session that no
/// player will open.
bool isVideoFile(const QString& path);

/// How long a streaming URL stays valid. Long enough that a seek an hour into
/// a session, or a reconnect after a laptop wakes up, never lands on an
/// expired signature; far inside SigV4's seven-day ceiling.
constexpr int kStreamExpirySeconds = 12 * 60 * 60;

/// What a media player should open for a file inside a connection's cache, or
/// an invalid URL when `localPath` is an ordinary file the caller should open
/// directly.
///
/// The result carries the credential — a presigned signature for S3 and GCS,
/// `user:pass` for WebDAV — so it is safe to hand to a player and to nothing
/// else. Never log one, and use QUrl::toDisplayString() anywhere one is shown.
QUrl streamSource(const RemoteConnection& connection, const QString& localPath);

/// Empty when `target` is usable for `type`, else the reason it is not.
QString validateTarget(LocationType type, const QString& target);

/// The form of `target` to store. Normalizing per protocol matters because
/// the connection id hashes the target: two spellings of one bucket must
/// settle on the same string or they become two caches of the same files.
QString normalizeTarget(LocationType type, const QString& target);

RemoteSyncResult syncConnection(const RemoteConnection& connection);

// ── Protocols ───────────────────────────────────────────────────────
RemoteBackend makeWebDavBackend(const RemoteConnection& connection);
QString webDavTargetError(const QString& target);

RemoteBackend makeS3Backend(const RemoteConnection& connection);
QString s3TargetError(LocationType type, const QString& target);
QString s3NormalizedTarget(LocationType type, const QString& target);
/// `objectUrl` with SigV4 query-string authentication attached, so that a
/// player with no notion of AWS credentials can fetch it. `region` is the
/// scope the sync signed against; empty falls back to the connection's own
/// setting. Returns the URL unchanged when the connection has no key, which
/// is what a public bucket wants.
QUrl s3PresignedUrl(const RemoteConnection& connection, const QString& region,
                    const QUrl& objectUrl, int expirySeconds);

}  // namespace omatrack
