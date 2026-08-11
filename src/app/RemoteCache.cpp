#include "RemoteCache.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QTimer>

#include <algorithm>

namespace omatrack {
namespace {

constexpr int kRequestTimeoutMs = 30'000;
/// Enough for the usual http→https or bucket-region hop, low enough that a
/// redirect loop ends as an error rather than a hang.
constexpr int kMaximumRedirects = 5;

struct DownloadResult {
    int status = 0;
    QString error;
    qint64 bytes = 0;
};

/// The redirect target, or an invalid URL when the response was not one.
QUrl redirectTarget(QNetworkReply* reply, const QUrl& from) {
    const int status =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status != 301 && status != 302 && status != 303 && status != 307 &&
        status != 308)
        return {};
    const QByteArray location = reply->rawHeader("Location");
    if (location.isEmpty()) return {};
    const QUrl target = from.resolved(QUrl::fromEncoded(location));
    return target.isValid() ? target : QUrl();
}

DownloadResult downloadToFile(QNetworkAccessManager& manager, const QUrl& url,
                              const RequestFactory& build,
                              const QString& path) {
    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return {0, QStringLiteral("Unable to open cache file"), 0};

    DownloadResult result;
    QUrl target = url;
    for (int hop = 0; hop <= kMaximumRedirects; ++hop) {
        // Each hop restarts the file: a redirect may arrive after the server
        // has already written a short error body.
        output.seek(0);
        result = DownloadResult{};

        QNetworkReply* reply = manager.get(build(target));
        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        bool timedOut = false;
        bool writeFailed = false;
        qint64 bytes = 0;
        QObject::connect(reply, &QNetworkReply::readyRead, &loop, [&]() {
            const QByteArray chunk = reply->readAll();
            if (output.write(chunk) != chunk.size()) {
                writeFailed = true;
                reply->abort();
                return;
            }
            bytes += chunk.size();
        });
        QObject::connect(reply, &QNetworkReply::finished, &loop,
                         &QEventLoop::quit);
        QObject::connect(&timeout, &QTimer::timeout, &loop, [&]() {
            timedOut = true;
            if (reply->isRunning()) reply->abort();
            loop.quit();
        });
        timeout.start(kRequestTimeoutMs);
        loop.exec();
        const QByteArray finalChunk = reply->readAll();
        if (!finalChunk.isEmpty() && !writeFailed) {
            if (output.write(finalChunk) != finalChunk.size())
                writeFailed = true;
            bytes += finalChunk.size();
        }

        const QUrl next = timedOut ? QUrl() : redirectTarget(reply, target);
        result.status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        result.bytes = bytes;
        if (timedOut)
            result.error = QStringLiteral("Download timed out");
        else if (writeFailed)
            result.error = QStringLiteral("Unable to write cache file");
        else if (next.isValid())
            result.error.clear();
        else if (reply->error() != QNetworkReply::NoError)
            result.error = reply->errorString();
        reply->deleteLater();

        if (!next.isValid()) break;
        if (hop == kMaximumRedirects) {
            result.error = QStringLiteral("Too many redirects");
            break;
        }
        target = next;
    }

    if (result.status != 200 || !result.error.isEmpty() || !output.commit()) {
        output.cancelWriting();
        if (result.error.isEmpty())
            result.error = QStringLiteral("Unable to commit cache file");
        return result;
    }
    return result;
}

QJsonObject readIndex(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    return document.isObject() ? document.object() : QJsonObject();
}

bool writeIndex(const QString& path, const QJsonObject& index) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    if (file.write(QJsonDocument(index).toJson(QJsonDocument::Compact)) < 0)
        return false;
    return file.commit();
}

/// What the last sync left behind and is still on disk — the answer when the
/// server cannot be reached.
QVector<RemoteObject> cachedObjects(const QJsonObject& entries,
                                    const QString& cachePath) {
    QVector<RemoteObject> result;
    for (auto it = entries.begin(); it != entries.end(); ++it) {
        const QJsonObject row = it.value().toObject();
        const QString relative = it.key();
        if (relative.isEmpty()) continue;
        if (!QFileInfo::exists(QDir(cachePath).filePath(relative))) continue;
        RemoteObject object;
        object.relativePath = relative;
        object.etag = row.value(QStringLiteral("etag")).toString();
        object.modified = row.value(QStringLiteral("modified")).toString();
        object.size =
            row.value(QStringLiteral("size")).toVariant().toLongLong();
        result.append(object);
    }
    return result;
}

/// Empty when `relative` names a file this machine can actually hold under
/// the cache root, else why it cannot.
///
/// A server chooses these names, so they are untrusted input in both
/// directions. An href or an object key can be built to climb out of the
/// cache directory, and S3 permits characters — `:`, `?`, `*`, `|` — that
/// Windows simply will not create. The rule is applied on every platform so
/// that a library looks the same everywhere rather than quietly holding files
/// that vanish when the same bucket is opened on another machine.
QString localPathError(const QString& relative) {
    if (relative.isEmpty()) return QStringLiteral("empty name");
    if (relative != QDir::cleanPath(relative) ||
        relative.startsWith(QLatin1Char('/')) ||
        relative == QStringLiteral("..") ||
        relative.startsWith(QStringLiteral("../")) ||
        relative.contains(QStringLiteral("/../")))
        return QStringLiteral("path escapes the cache folder");
    for (const QChar character : relative) {
        const char16_t code = character.unicode();
        if (code < 0x20 || code == u'<' || code == u'>' || code == u':' ||
            code == u'"' || code == u'|' || code == u'?' || code == u'*' ||
            code == u'\\')
            return QStringLiteral("name contains %1, which cannot be a file")
                .arg(character);
    }
    return {};
}

RemoteSyncResult offlineResult(RemoteSyncResult result,
                               const QVector<RemoteObject>& cached,
                               const QString& reason) {
    result.fromCache = !cached.isEmpty();
    for (const RemoteObject& object : cached)
        result.files.append(QDir().cleanPath(object.relativePath));
    result.success = result.fromCache;
    result.status = result.fromCache
                        ? QStringLiteral("Offline cache (%1 files)")
                              .arg(result.files.size())
                        : QStringLiteral("Server unavailable: %1").arg(reason);
    return result;
}

}  // namespace

QNetworkRequest makeRequest(const QUrl& url) {
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    return request;
}

HttpResponse sendFollowing(QNetworkAccessManager& manager, const QUrl& url,
                           const QByteArray& method,
                           const RequestFactory& build,
                           const QByteArray& body) {
    HttpResponse result;
    QUrl target = url;
    for (int hop = 0; hop <= kMaximumRedirects; ++hop) {
        QNetworkReply* reply =
            manager.sendCustomRequest(build(target), method, body);
        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        bool timedOut = false;
        QObject::connect(reply, &QNetworkReply::finished, &loop,
                         &QEventLoop::quit);
        QObject::connect(&timeout, &QTimer::timeout, &loop, [&]() {
            timedOut = true;
            if (reply->isRunning()) reply->abort();
            loop.quit();
        });
        timeout.start(kRequestTimeoutMs);
        loop.exec();

        const QUrl next = timedOut ? QUrl() : redirectTarget(reply, target);
        result = HttpResponse{};
        result.status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        result.body = reply->readAll();
        for (const auto& [name, value] : reply->rawHeaderPairs())
            result.headers.insert(name.toLower(), value);
        if (timedOut)
            result.error = QStringLiteral("Request timed out");
        else if (!next.isValid() && reply->error() != QNetworkReply::NoError)
            result.error = reply->errorString();
        reply->deleteLater();

        if (!next.isValid()) return result;
        if (hop == kMaximumRedirects) {
            result.error = QStringLiteral("Too many redirects");
            return result;
        }
        target = next;
    }
    return result;
}

QString locationId(const QString& target, const QString& username) {
    const QByteArray key =
        (target.trimmed() + QLatin1Char('\n') + username).toUtf8();
    return QString::fromLatin1(
        QCryptographicHash::hash(key, QCryptographicHash::Sha256).toHex());
}

QString cacheRoot() {
    return QStandardPaths::writableLocation(
               QStandardPaths::GenericCacheLocation) +
           QStringLiteral("/omatrack");
}

/// Every directory a protocol keeps caches in.
///
/// Enumerated from the protocols rather than from the configured locations,
/// so a cache belonging to a location that was removed while this build was
/// not running is still found, counted, and cleared.
QStringList cacheDirectories() {
    QStringList directories;
    for (const LocationType type :
         {LocationType::WebDav, LocationType::S3, LocationType::Gcs}) {
        const QString path =
            cacheRoot() + QLatin1Char('/') + locationTypeKey(type);
        if (QFileInfo::exists(path)) directories.append(path);
    }
    return directories;
}

qint64 cacheUsageBytes() {
    qint64 total = 0;
    for (const QString& directory : cacheDirectories()) {
        QDirIterator files(directory, QDir::Files | QDir::Hidden,
                           QDirIterator::Subdirectories);
        while (files.hasNext()) {
            files.next();
            total += files.fileInfo().size();
        }
    }
    return total;
}

qint64 clearCache() {
    const qint64 freed = cacheUsageBytes();
    for (const QString& directory : cacheDirectories())
        QDir(directory).removeRecursively();
    return freed;
}

QString cacheDirectory(const RemoteConnection& connection) {
    if (connection.type == LocationType::Folder) return {};
    const QString id = connection.id.isEmpty()
                           ? locationId(connection.target, connection.username)
                           : connection.id;
    return cacheRoot() + QLatin1Char('/') + locationTypeKey(connection.type) +
           QLatin1Char('/') + id;
}

QString validateTarget(LocationType type, const QString& target) {
    switch (type) {
        case LocationType::WebDav: return webDavTargetError(target);
        case LocationType::S3:
        case LocationType::Gcs: return s3TargetError(type, target);
        case LocationType::Folder: break;
    }
    return QStringLiteral("Unsupported connection type.");
}

QString normalizeTarget(LocationType type, const QString& target) {
    switch (type) {
        case LocationType::WebDav:
            // Unchanged from when WebDAV stood alone, so no configured server
            // gets a new id and loses the cache it already downloaded.
            return QUrl(target.trimmed()).toString(QUrl::FullyEncoded);
        case LocationType::S3:
        case LocationType::Gcs: return s3NormalizedTarget(type, target);
        case LocationType::Folder: break;
    }
    return target.trimmed();
}

RemoteSyncResult syncConnection(const RemoteConnection& connection) {
    RemoteSyncResult result;
    result.id = connection.id.isEmpty()
                    ? locationId(connection.target, connection.username)
                    : connection.id;
    result.cachePath = cacheDirectory(connection);
    const QString indexPath = QDir(result.cachePath).filePath("index.json");
    const QJsonObject oldEntries =
        readIndex(indexPath).value(QStringLiteral("entries")).toObject();
    const QVector<RemoteObject> cached =
        cachedObjects(oldEntries, result.cachePath);

    const QString invalid = validateTarget(connection.type, connection.target);
    if (!invalid.isEmpty()) {
        result.error = invalid;
        result = offlineResult(std::move(result), cached, invalid);
        if (!result.fromCache) result.status = invalid;
        return result;
    }

    if (!QDir().mkpath(result.cachePath)) {
        result.error = QStringLiteral("Unable to create the cache folder");
        result.status = result.error;
        return result;
    }

    RemoteBackend backend;
    switch (connection.type) {
        case LocationType::WebDav:
            backend = makeWebDavBackend(connection);
            break;
        case LocationType::S3:
        case LocationType::Gcs:
            // Google Cloud Storage speaks the S3 API — SigV4 with an HMAC
            // interoperability key, and ListObjectsV2 paging. One backend.
            backend = makeS3Backend(connection);
            break;
        case LocationType::Folder: break;
    }

    QNetworkAccessManager manager;
    QVector<RemoteObject> objects;
    QString listingError;
    if (!backend.list(manager, &objects, &listingError)) {
        result.error = listingError;
        return offlineResult(std::move(result), cached, listingError);
    }

    std::sort(objects.begin(), objects.end(),
              [](const RemoteObject& left, const RemoteObject& right) {
                  return left.relativePath < right.relativePath;
              });

    QJsonObject newEntries;
    QSet<QString> seen;
    for (const RemoteObject& object : objects) {
        if (seen.contains(object.relativePath)) continue;
        seen.insert(object.relativePath);
        // Left out of newEntries as well as of the download, so the prune
        // below cannot mistake a name this machine never stored for a file
        // the server deleted.
        if (!localPathError(object.relativePath).isEmpty()) {
            result.skipped.append(object.relativePath);
            continue;
        }
        const QJsonObject old =
            oldEntries.value(object.relativePath).toObject();
        const QString localPath =
            QDir(result.cachePath).filePath(object.relativePath);
        const bool unchanged =
            QFileInfo::exists(localPath) && !object.etag.isEmpty() &&
            object.etag == old.value(QStringLiteral("etag")).toString() &&
            object.modified == old.value(QStringLiteral("modified")).toString();
        if (!unchanged) {
            if (!QDir().mkpath(QFileInfo(localPath).absolutePath())) {
                result.error =
                    QStringLiteral("Unable to create a cache folder");
                result.status = result.error;
                return result;
            }
            const RequestFactory build = [&backend](const QUrl& url) {
                QNetworkRequest request = makeRequest(url);
                backend.sign(request, "GET");
                return request;
            };
            const DownloadResult download =
                downloadToFile(manager, object.url, build, localPath);
            if (download.status != 200 || !download.error.isEmpty()) {
                result.error = download.error.isEmpty()
                                   ? QStringLiteral("Download returned HTTP %1")
                                         .arg(download.status)
                                   : download.error;
                result.status = result.error;
                return result;
            }
            result.downloadedBytes += download.bytes;
        }

        newEntries.insert(
            object.relativePath,
            QJsonObject{{QStringLiteral("etag"), object.etag},
                        {QStringLiteral("modified"), object.modified},
                        {QStringLiteral("size"), object.size},
                        {QStringLiteral("url"),
                         object.url.toString(QUrl::FullyEncoded)}});
        result.files.append(object.relativePath);
    }

    // Anything the server no longer lists is no longer reachable, so the local
    // copy is dead weight rather than an offline fallback.
    for (auto it = oldEntries.begin(); it != oldEntries.end(); ++it)
        if (!newEntries.contains(it.key()))
            QFile::remove(QDir(result.cachePath).filePath(it.key()));

    const QJsonObject index{
        {QStringLiteral("version"), 1},
        {QStringLiteral("url"), connection.target},
        {QStringLiteral("entries"), newEntries},
        {QStringLiteral("syncedAt"),
         QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}};
    if (!writeIndex(indexPath, index)) {
        result.error = QStringLiteral("Unable to write the cache index");
        result.status = result.error;
        return result;
    }
    result.success = true;
    result.status =
        result.downloadedBytes > 0
            ? QStringLiteral("Connected · %1 files · %2 MB")
                  .arg(result.files.size())
                  .arg(double(result.downloadedBytes) / (1024.0 * 1024.0), 0,
                       'f', 1)
            : QStringLiteral("Connected · %1 files (cached)")
                  .arg(result.files.size());
    // Say so rather than letting files quietly go missing: a driver comparing
    // the bucket to the library would otherwise have no way to tell why.
    if (!result.skipped.isEmpty())
        result.status.append(QStringLiteral(" · %1 unusable name%2")
                                 .arg(result.skipped.size())
                                 .arg(result.skipped.size() == 1 ? "" : "s"));
    return result;
}

}  // namespace omatrack
