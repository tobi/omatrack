// Remote discovery and the shared normalized-telemetry cache.
//
// A connection contributes listing and authentication. Discovery keeps only
// zero-byte source stubs plus ETag metadata in the per-location cache. Native
// `.telemetry` files exist for remote recordings only and are keyed by the
// object's ETag under `.omatrack/c/{generation}/`, both on this machine and
// beside the recordings on the server, where they are published create-only
// so machines converge without overwriting. Local files are parsed directly.
#pragma once

#include "LibraryLocation.h"

#include <QByteArray>
#include <QMap>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVector>

#include <atomic>
#include <functional>
#include <memory>

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

/// Cooperative cancel for a network or scan job. Empty means "run to finish".
using IoCancel = std::shared_ptr<std::atomic<bool>>;

inline bool ioCancelled(const IoCancel& cancel) {
    return cancel && cancel->load(std::memory_order_relaxed);
}

/// A protocol, as a bag of callbacks rather than a class. Listing runs as
/// straight-line synchronous code on a worker thread, and a test can supply
/// lambdas to drive the engine with no network at all.
///
/// `sign` runs on every header-signed outgoing request, listing and download
/// alike, and owns authentication and any protocol-specific headers. The
/// `presign` callback produces a URL a credential-less client can fetch — a
/// SigV4 presigned URL for S3/GCS, credentials embedded in the URL for
/// WebDAV — so the player and the range reader never have to know how the
/// server authenticates. `head`, `get` and `putIfAbsent` are shared helpers
/// built on `sign` + sendFollowing(), so the engine never switches on the
/// protocol to build a request.
struct RemoteBackend {
    /// List every object under the connection. `scope` receives the protocol
    /// scope the sync should record (the S3 region) — empty when the protocol
    /// has none. `cancel` propagates through paging and directory recursion.
    std::function<bool(QVector<RemoteObject>* objects, QString* scope,
                       QString* error, const IoCancel& cancel)>
        list;
    std::function<void(QNetworkRequest&, const QByteArray& method,
                       const QByteArray& payload)>
        sign;
    /// Absolute URL of one object under this connection, or invalid when the
    /// backend cannot name objects itself (tests that only list).
    std::function<QUrl(const QString& relativePath)> objectUrl;
    /// A URL a player with no notion of these credentials can fetch.
    /// `expirySeconds` bounds signed URLs; WebDAV ignores it.
    std::function<QUrl(const QUrl& objectUrl, int expirySeconds)> presign;

    /// One header-signed HEAD. Used for region discovery and metadata.
    HttpResponse head(const QUrl& url, const IoCancel& cancel = {}) const;
    /// One header-signed GET returning the body.
    HttpResponse get(const QUrl& url, const IoCancel& cancel = {}) const;
    /// Create-only PUT: succeeds (empty string) on 200/201/204/412, else the
    /// reason. Existing objects win.
    QString putIfAbsent(const QUrl& url, const QByteArray& body,
                        const IoCancel& cancel = {}) const;
};

/// Base request carrying the policies every Omatrack request needs. Qt is told
/// not to follow redirects on its own — it would reuse headers signed for the
/// original URL, which both breaks the signature and can hand credentials to a
/// host that never earned them. sendFollowing() follows them deliberately,
/// rebuilding and re-signing each hop.
/// HTTP runs on a dedicated I/O thread. The caller blocks on the I/O thread's
/// QFuture; this must never be called on the GUI thread — run it through
/// QtConcurrent and observe the result with a QFutureWatcher instead.
QNetworkRequest makeRequest(const QUrl& url);
HttpResponse sendFollowing(const QUrl& url, const QByteArray& method,
                           const RequestFactory& build,
                           const QByteArray& body = {},
                           const IoCancel& cancel = {});

/// Stable identity for a library entry, connection or folder alike. The input
/// is unchanged from when WebDAV was the only protocol, so location metadata
/// remains stable across restarts.
QString locationId(const QString& target, const QString& username);

/// Per-location discovery cache containing source stubs and ETag metadata.
/// Empty for a plain folder.
QString cacheDirectory(const RemoteConnection& connection);

/// Root under which per-location discovery caches sit.
QString cacheRoot();
/// Local mirror of the normalized `.telemetry` files for remote recordings.
/// The cache is keyed by the remote object's ETag, never by location, and
/// lives under one directory per converter generation
/// (`.omatrack/c/{generation}/{key}.telemetry`) so a decoder fix upstream
/// regenerates every normalization instead of trusting an older one.
QString telemetryCacheRoot();
QString telemetryCacheDirectory();
/// The same generation directory relative to a remote root, for the shared
/// cache published beside the recordings (`.omatrack/c/{generation}`).
QString telemetryCacheRelativeDirectory();
QString telemetryCachePath(const QString& key);
/// Remove every generation directory other than this build's, plus any
/// pre-generation `{key}.telemetry` left directly in the root. Returns the
/// number of entries removed. Disk I/O: call from a worker.
int pruneStaleTelemetryCaches();
QString etagFileKey(const QString& etag);
/// True for cache-private `.omatrack/` artifacts and legacy/overlay names
/// that must not become library sources.
bool isSidecarPath(const QString& relativePath);
/// Wait until the I/O thread has finished tearing down the last reply.
void drainNetworkIo();

/// The ETag the last sync recorded for `localPath`, or empty.
QString cachedObjectEtag(const RemoteConnection& connection,
                         const QString& localPath);

/// Fetches an object that is not part of the remote listing, such as
/// `.omatrack/c/{key}.telemetry`, into `destination`. Returns an empty string
/// on success or on a 404 cache miss; other failures are returned to the
/// caller.
QString fetchRemoteObject(const RemoteConnection& connection,
                          const QString& relativePath,
                          const QString& destination,
                          const IoCancel& cancel = {});

/// Publishes a generated cache object to the remote root without mirroring it
/// into the per-location discovery cache. Existing objects win.
QString publishRemoteObject(const RemoteConnection& connection,
                            const QString& relativePath, const QByteArray& body,
                            const IoCancel& cancel = {});

/// What the cache holds, split by who decided to put it there.
struct CacheUsage {
    /// Everything the sync downloaded on its own, which the budget governs.
    qint64 bytes = 0;
    /// Video somebody asked for by name, which it does not. Reported apart
    /// because a single recording dwarfs a whole library of telemetry, and a
    /// limit that counted it would evict thousands of laps to make room for
    /// one file the user deliberately chose to keep.
    qint64 videoBytes = 0;
};

/// What is currently held in every protocol's cache.
///
/// Measured from the filesystem rather than summed from index entries, which
/// is the only way it can be honest: it also counts the caches of locations
/// that have since been removed and the temporary files a download that died
/// mid-write left behind. Both are exactly what someone checking this number
/// wants to know about.
CacheUsage cacheUsage();

/// Deletes every downloaded file, and returns how many bytes that freed.
/// Nothing here cannot be fetched again.
qint64 clearCache();

/// What the cache may hold before the least recently opened files are dropped.
/// Video streams rather than being cached, so this is a guardrail against a
/// bucket of half a million laps rather than a limit anyone normally meets.
constexpr qint64 kDefaultCacheLimitBytes = 20LL * 1024 * 1024 * 1024;

/// Reads a size written the way a person writes one — `20 GB`, `500MiB`, a
/// bare byte count. Returns `fallback` for anything it cannot make sense of,
/// because a typo in a config file should not silently disable the cache.
qint64 parseByteSize(const QString& text, qint64 fallback);

/// Deletes least-recently-opened cached files until the total fits inside
/// `limitBytes`, and returns how many bytes that freed.
///
/// Age comes from the local file's modification time, which downloading sets
/// and opening a lap refreshes. Recording it in index.json instead would lose
/// the race with a sync, which reads that file once and writes it back minutes
/// later.
///
/// `keepPaths` names files that survive whatever their age — the recordings
/// open right now. Unlinking one of those succeeds on Linux and leaves the
/// next read failing; on Windows the delete fails and the index quietly stops
/// describing the disk.
///
/// Downloaded video is outside this entirely, neither counted nor evicted:
/// it is only ever here because somebody chose it for a flight, and it is
/// removed the same way it arrived — by asking.
qint64 enforceCacheBudget(qint64 limitBytes, const QSet<QString>& keepPaths);

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
/// directly — including a video already downloaded for offline use, where the
/// bytes on disk are the better answer than any URL.
///
/// A fresh signature every call is what makes this safe to call again: a
/// laptop that slept past the expiry, or a connection that dropped, is
/// recovered by asking for the source a second time.
///
/// The result carries the credential — a presigned signature for S3 and GCS,
/// `user:pass` for WebDAV — so it is safe to hand to a player and to nothing
/// else. Never log one, and use QUrl::toDisplayString() anywhere one is shown.
QUrl streamSource(const RemoteConnection& connection, const QString& localPath);

/// True when `localPath` is a video this connection has been asked to keep on
/// disk rather than stream. The pin lives in the cache index next to the file
/// it names, so it survives both a restart and a re-sync.
bool offlineVideoPinned(const RemoteConnection& connection,
                        const QString& localPath);

/// The listed object URL for a cache path, or invalid when the last sync
/// did not record one. Used for range reads of streamed video.
QUrl objectUrlForPath(const RemoteConnection& connection,
                      const QString& localPath);

struct ObjectRange {
    qint64 offset = 0;
    qint64 length = 0;
};

/// Signed range GETs with bounded concurrency and connection reuse.
bool getObjectRanges(const RemoteConnection& connection, const QUrl& url,
                     const QVector<ObjectRange>& ranges,
                     QVector<QByteArray>* bodies, QString* error = nullptr,
                     const IoCancel& cancel = {});

/// One signed GET of `length` bytes at `offset`. Empty on failure.
QByteArray getObjectRange(const RemoteConnection& connection, const QUrl& url,
                          qint64 offset, qint64 length,
                          QString* error = nullptr,
                          const IoCancel& cancel = {});

/// Records — or withdraws — the wish to hold `localPath` locally, and returns
/// the reason it could not be. Withdrawing also hands the bytes back
/// immediately; a pin on its own downloads nothing, because a recording takes
/// long enough that it belongs in a job with a progress bar rather than in
/// whatever happened to be running.
QString pinOfflineVideo(const RemoteConnection& connection,
                        const QString& localPath, bool pinned);

/// Reports bytes received against the total the server declared, which is -1
/// until it says. Returning false abandons the download.
using DownloadProgress = std::function<bool(qint64 received, qint64 total)>;

/// Outcome of a streamed GET written straight to disk. Used by the AppImage
/// updater so a 100+ MiB payload never materialises as a QByteArray.
struct FileDownload {
    int status = 0;
    QString error;
    qint64 bytes = 0;
};

/// Streams `url` into `path` on the I/O thread. The caller waits on the
/// returned result; the GUI loop must not be the waiter.
FileDownload downloadFile(const QUrl& url, const RequestFactory& build,
                          const QString& path,
                          const DownloadProgress& progress = {},
                          const IoCancel& cancel = {});

/// Downloads one already-listed object into the place in the cache it belongs,
/// blocking until it is there. Returns the reason it failed, or an empty
/// string. Meant for the one file a person asked for by name — a whole
/// connection goes through syncConnection().
QString fetchObject(const RemoteConnection& connection,
                    const QString& localPath, const DownloadProgress& progress,
                    const IoCancel& cancel = {});

/// One pasted address, split into the fields a connection stores.
struct ConnectionAddress {
    /// The address with everything else taken out of it.
    QString target;
    QString username;
    QString password;
    /// See LibraryLocation::options — `region`, `endpoint`, `scheme`.
    QMap<QString, QString> options;
    /// Empty when the address parsed, else why it did not.
    QString error;
};

/// Takes an address in the full form a console hands out —
/// `s3://KEY:SECRET@bucket/prefix?region=eu-west-2&endpoint_override=host` —
/// and separates the parts that are credentials or settings from the part
/// that names the data.
///
/// Splitting at the point of entry rather than storing the string whole is
/// what keeps the rest of the application honest: locationId() hashes the
/// target, so a secret left in there would name the cache directory on disk,
/// appear in the library row on screen, and orphan every downloaded byte the
/// day the key is rotated. Credentials belong in the same two fields WebDAV
/// has always used, and tuning knobs in `options`.
///
/// A plain `s3://bucket/prefix`, or a WebDAV URL with no `user:pass@`, comes
/// back unchanged apart from normalization, so this is safe to run over every
/// address including ones already stored.
ConnectionAddress splitAddress(LocationType type, const QString& address);

/// Empty when `target` is usable for `type`, else the reason it is not.
QString validateTarget(LocationType type, const QString& target);

/// The form of `target` to store. Normalizing per protocol matters because
/// the connection id hashes the target: two spellings of one bucket must
/// settle on the same string or they become two caches of the same files.
QString normalizeTarget(LocationType type, const QString& target);

RemoteSyncResult syncConnection(const RemoteConnection& connection,
                                const IoCancel& cancel = {});

// ── Protocols ───────────────────────────────────────────────────────
RemoteBackend makeWebDavBackend(const RemoteConnection& connection);
QString webDavTargetError(const QString& target);

RemoteBackend makeS3Backend(const RemoteConnection& connection);
QString s3TargetError(LocationType type, const QString& target);
ConnectionAddress s3SplitAddress(LocationType type, const QString& address);
QString s3NormalizedTarget(LocationType type, const QString& target);
/// `objectUrl` with SigV4 query-string authentication attached, so that a
/// player with no notion of AWS credentials can fetch it. `region` is the
/// scope the sync signed against; empty falls back to the connection's own
/// setting. Returns the URL unchanged when the connection has no key, which
/// is what a public bucket wants.
QUrl s3PresignedUrl(const RemoteConnection& connection, const QString& region,
                    const QUrl& objectUrl, int expirySeconds);

}  // namespace omatrack
