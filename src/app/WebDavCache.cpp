#include "WebDavCache.h"

#include <QCryptographicHash>
#include <QDir>
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
#include <QXmlStreamReader>

namespace omatrack {
namespace {

constexpr int kRequestTimeoutMs = 30'000;
constexpr int kMaximumDepth = 64;

struct HttpResult {
    int status = 0;
    QByteArray body;
    QString error;
    qint64 bytes = 0;
};

struct Resource {
    QUrl url;
    QString relativePath;
    QString etag;
    QString modified;
    qint64 size = -1;
    bool collection = false;
};

struct DownloadResult {
    int status = 0;
    QString error;
    qint64 bytes = 0;
};

QUrl normalizedRoot(const WebDavConnection& connection) {
    QUrl root(connection.url.trimmed());
    QString path = root.path();
    if (!path.endsWith(QLatin1Char('/'))) path.append(QLatin1Char('/'));
    root.setPath(path);
    root.setFragment({});
    return root;
}

QByteArray authorizationHeader(const WebDavConnection& connection) {
    if (connection.username.isEmpty()) return {};
    return (connection.username + QLatin1Char(':') + connection.password)
        .toUtf8()
        .toBase64();
}

QNetworkRequest requestFor(const QUrl& url,
                           const WebDavConnection& connection) {
    QNetworkRequest request(url);
    request.setRawHeader("User-Agent", "Omatrack-WebDAV/1");
    const QByteArray auth = authorizationHeader(connection);
    if (!auth.isEmpty()) request.setRawHeader("Authorization", "Basic " + auth);
    return request;
}

HttpResult send(QNetworkAccessManager& manager, QNetworkRequest request,
                const QByteArray& method, const QByteArray& body = {}) {
    QNetworkReply* reply = manager.sendCustomRequest(request, method, body);
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    bool timedOut = false;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timeout, &QTimer::timeout, &loop, [&]() {
        timedOut = true;
        if (reply->isRunning()) reply->abort();
        loop.quit();
    });
    timeout.start(kRequestTimeoutMs);
    loop.exec();

    HttpResult result;
    result.status =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    result.body = reply->readAll();
    if (timedOut)
        result.error = QStringLiteral("WebDAV request timed out");
    else if (reply->error() != QNetworkReply::NoError)
        result.error = reply->errorString();
    reply->deleteLater();
    return result;
}

DownloadResult downloadToFile(QNetworkAccessManager& manager,
                              QNetworkRequest request, const QString& path) {
    QSaveFile output(path);
    if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return {0, QStringLiteral("Unable to open WebDAV cache file"), 0};

    QNetworkReply* reply = manager.get(request);
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
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(&timeout, &QTimer::timeout, &loop, [&]() {
        timedOut = true;
        if (reply->isRunning()) reply->abort();
        loop.quit();
    });
    timeout.start(kRequestTimeoutMs);
    loop.exec();
    const QByteArray finalChunk = reply->readAll();
    if (!finalChunk.isEmpty() && !writeFailed) {
        if (output.write(finalChunk) != finalChunk.size()) writeFailed = true;
        bytes += finalChunk.size();
    }

    DownloadResult result;
    result.status =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    result.bytes = bytes;
    if (timedOut)
        result.error = QStringLiteral("WebDAV download timed out");
    else if (writeFailed)
        result.error = QStringLiteral("Unable to write WebDAV cache file");
    else if (reply->error() != QNetworkReply::NoError)
        result.error = reply->errorString();
    reply->deleteLater();
    if (result.status != 200 || !result.error.isEmpty() || !output.commit()) {
        output.cancelWriting();
        if (result.error.isEmpty())
            result.error = QStringLiteral("Unable to commit WebDAV cache file");
        return result;
    }
    return result;
}

bool parseResources(const QByteArray& payload, const QUrl& root,
                    QVector<Resource>* resources, QString* error) {
    QXmlStreamReader xml(payload);
    Resource current;
    bool inResponse = false;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement()) {
            const QString name = xml.name().toString();
            if (name == QStringLiteral("response")) {
                current = Resource{};
                inResponse = true;
            } else if (inResponse && name == QStringLiteral("href")) {
                const QString href = xml.readElementText();
                QUrl url = QUrl::fromEncoded(href.toUtf8());
                if (url.isRelative()) url = root.resolved(url);
                current.url = url;
            } else if (inResponse && name == QStringLiteral("collection")) {
                current.collection = true;
            } else if (inResponse && name == QStringLiteral("getetag")) {
                current.etag = xml.readElementText();
            } else if (inResponse &&
                       name == QStringLiteral("getlastmodified")) {
                current.modified = xml.readElementText();
            } else if (inResponse &&
                       name == QStringLiteral("getcontentlength")) {
                current.size = xml.readElementText().toLongLong();
            }
        } else if (xml.isEndElement() &&
                   xml.name() == QStringLiteral("response")) {
            if (!current.url.isValid()) {
                if (error)
                    *error = QStringLiteral("WebDAV response has no URL");
                return false;
            }
            const QString rootPath = root.path();
            const QString resourcePath = current.url.path();
            if (resourcePath.startsWith(rootPath)) {
                const QByteArray encodedRelative =
                    resourcePath.mid(rootPath.size()).toUtf8();
                QString relative = QUrl::fromPercentEncoding(encodedRelative);
                while (relative.endsWith(QLatin1Char('/'))) relative.chop(1);
                const QString clean = QDir::cleanPath(relative);
                if (!clean.isEmpty() && clean != QStringLiteral(".") &&
                    clean != QStringLiteral("..") &&
                    !clean.startsWith(QStringLiteral("../")) &&
                    !clean.startsWith(QLatin1Char('/'))) {
                    current.relativePath = clean;
                    resources->append(current);
                }
            }
            inResponse = false;
        }
    }
    if (xml.hasError()) {
        if (error) *error = xml.errorString();
        return false;
    }
    return true;
}

QVector<Resource> parseCachedResources(const QJsonObject& entries,
                                       const QString& cachePath) {
    QVector<Resource> result;
    for (auto it = entries.begin(); it != entries.end(); ++it) {
        const QJsonObject row = it.value().toObject();
        const QString relative = it.key();
        if (relative.isEmpty()) continue;
        const QString local = QDir(cachePath).filePath(relative);
        if (!QFileInfo::exists(local)) continue;
        Resource resource;
        resource.relativePath = relative;
        resource.etag = row.value(QStringLiteral("etag")).toString();
        resource.modified = row.value(QStringLiteral("modified")).toString();
        resource.size =
            row.value(QStringLiteral("size")).toVariant().toLongLong();
        result.append(resource);
    }
    return result;
}

bool isValidConnection(const WebDavConnection& connection, QString* error) {
    const QUrl root = normalizedRoot(connection);
    if (!root.isValid() ||
        (root.scheme() != QStringLiteral("http") &&
         root.scheme() != QStringLiteral("https")) ||
        root.host().isEmpty()) {
        if (error) *error = QStringLiteral("Enter a valid http(s) WebDAV URL");
        return false;
    }
    return true;
}

bool collectDepthOne(QNetworkAccessManager& manager,
                     const WebDavConnection& connection, const QUrl& url,
                     const QUrl& root, int depth, QSet<QString>* visited,
                     QVector<Resource>* resources, QString* error) {
    if (depth > kMaximumDepth) {
        if (error)
            *error = QStringLiteral("WebDAV directory nesting is too deep");
        return false;
    }
    const QString key = url.toString(QUrl::FullyEncoded);
    if (visited->contains(key)) return true;
    visited->insert(key);

    QNetworkRequest request = requestFor(url, connection);
    request.setRawHeader("Depth", "1");
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/xml; charset=utf-8"));
    const QByteArray body(
        "<?xml version=\"1.0\" encoding=\"utf-8\" ?>"
        "<propfind xmlns=\"DAV:\"><prop>"
        "<resourcetype/><getetag/><getlastmodified/><getcontentlength/>"
        "</prop></propfind>");
    const HttpResult response = send(manager, request, "PROPFIND", body);
    if (response.status != 207) {
        if (error) {
            *error = response.error.isEmpty()
                         ? QStringLiteral("WebDAV PROPFIND returned HTTP %1")
                               .arg(response.status)
                         : response.error;
        }
        return false;
    }
    QVector<Resource> children;
    if (!parseResources(response.body, root, &children, error)) return false;
    for (const Resource& resource : children) resources->append(resource);
    for (const Resource& resource : children) {
        if (!resource.collection) continue;
        if (!collectDepthOne(manager, connection, resource.url, root, depth + 1,
                             visited, resources, error))
            return false;
    }
    return true;
}

bool collectResources(QNetworkAccessManager& manager,
                      const WebDavConnection& connection, const QUrl& root,
                      QVector<Resource>* resources, QString* error) {
    QNetworkRequest request = requestFor(root, connection);
    request.setRawHeader("Depth", "infinity");
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/xml; charset=utf-8"));
    const QByteArray body(
        "<?xml version=\"1.0\" encoding=\"utf-8\" ?>"
        "<propfind xmlns=\"DAV:\"><prop>"
        "<resourcetype/><getetag/><getlastmodified/><getcontentlength/>"
        "</prop></propfind>");
    const HttpResult response = send(manager, request, "PROPFIND", body);
    if (response.status == 207)
        return parseResources(response.body, root, resources, error);

    resources->clear();
    QSet<QString> visited;
    return collectDepthOne(manager, connection, root, root, 0, &visited,
                           resources, error);
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

}  // namespace

QString WebDavCache::connectionId(const QString& url, const QString& username) {
    const QByteArray key =
        (url.trimmed() + QLatin1Char('\n') + username).toUtf8();
    return QString::fromLatin1(
        QCryptographicHash::hash(key, QCryptographicHash::Sha256).toHex());
}

QString WebDavCache::cachePath(const WebDavConnection& connection) {
    const QString id = connection.id.isEmpty()
                           ? connectionId(connection.url, connection.username)
                           : connection.id;
    return QStandardPaths::writableLocation(
               QStandardPaths::GenericCacheLocation) +
           QStringLiteral("/omatrack/webdav/") + id;
}

WebDavSyncResult WebDavCache::sync(const WebDavConnection& connection) {
    WebDavSyncResult result;
    result.id = connection.id.isEmpty()
                    ? connectionId(connection.url, connection.username)
                    : connection.id;
    result.cachePath = cachePath(connection);
    const QString indexPath = QDir(result.cachePath).filePath("index.json");
    const QJsonObject oldIndex = readIndex(indexPath);
    const QJsonObject oldEntries =
        oldIndex.value(QStringLiteral("entries")).toObject();
    const QVector<Resource> cached =
        parseCachedResources(oldEntries, result.cachePath);

    QString validationError;
    if (!isValidConnection(connection, &validationError)) {
        result.error = validationError;
        result.status = validationError;
        result.fromCache = !cached.isEmpty();
        result.files = [&cached]() {
            QStringList paths;
            for (const Resource& resource : cached)
                paths.append(QDir().cleanPath(resource.relativePath));
            return paths;
        }();
        result.success = result.fromCache;
        if (result.fromCache)
            result.status = QStringLiteral("Offline cache (%1 files)")
                                .arg(result.files.size());
        return result;
    }

    if (!QDir().mkpath(result.cachePath)) {
        result.error = QStringLiteral("Unable to create WebDAV cache");
        result.status = result.error;
        return result;
    }

    QNetworkAccessManager manager;
    const QUrl root = normalizedRoot(connection);
    QVector<Resource> resources;
    QString listingError;
    if (!collectResources(manager, connection, root, &resources,
                          &listingError)) {
        result.error = listingError;
        result.fromCache = !cached.isEmpty();
        for (const Resource& resource : cached)
            result.files.append(QDir().cleanPath(resource.relativePath));
        result.success = result.fromCache;
        result.status =
            result.fromCache
                ? QStringLiteral("Offline cache (%1 files)")
                      .arg(result.files.size())
                : QStringLiteral("WebDAV unavailable: %1").arg(listingError);
        return result;
    }

    QJsonObject newEntries;
    std::sort(resources.begin(), resources.end(),
              [](const Resource& left, const Resource& right) {
                  return left.relativePath < right.relativePath;
              });
    QSet<QString> seen;
    for (const Resource& resource : resources) {
        if (resource.collection || seen.contains(resource.relativePath))
            continue;
        seen.insert(resource.relativePath);
        const QJsonObject old =
            oldEntries.value(resource.relativePath).toObject();
        const QString localPath =
            QDir(result.cachePath).filePath(resource.relativePath);
        const bool unchanged =
            QFileInfo::exists(localPath) && !resource.etag.isEmpty() &&
            resource.etag == old.value(QStringLiteral("etag")).toString() &&
            resource.modified ==
                old.value(QStringLiteral("modified")).toString();
        if (!unchanged) {
            if (!QDir().mkpath(QFileInfo(localPath).absolutePath())) {
                result.error =
                    QStringLiteral("Unable to create WebDAV cache folder");
                result.status = result.error;
                return result;
            }
            QNetworkRequest request = requestFor(resource.url, connection);
            const DownloadResult download =
                downloadToFile(manager, request, localPath);
            if (download.status != 200 || !download.error.isEmpty()) {
                result.error =
                    download.error.isEmpty()
                        ? QStringLiteral("WebDAV download returned HTTP %1")
                              .arg(download.status)
                        : download.error;
                result.status = result.error;
                return result;
            }
            result.downloadedBytes += download.bytes;
        }

        newEntries.insert(
            resource.relativePath,
            QJsonObject{{QStringLiteral("etag"), resource.etag},
                        {QStringLiteral("modified"), resource.modified},
                        {QStringLiteral("size"), resource.size},
                        {QStringLiteral("url"),
                         resource.url.toString(QUrl::FullyEncoded)}});
        result.files.append(resource.relativePath);
    }
    for (auto it = oldEntries.begin(); it != oldEntries.end(); ++it) {
        if (!newEntries.contains(it.key())) {
            QFile::remove(QDir(result.cachePath).filePath(it.key()));
        }
    }

    const QJsonObject index{
        {QStringLiteral("version"), 1},
        {QStringLiteral("url"), root.toString(QUrl::FullyEncoded)},
        {QStringLiteral("entries"), newEntries},
        {QStringLiteral("syncedAt"),
         QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}};
    if (!writeIndex(indexPath, index)) {
        result.error = QStringLiteral("Unable to write WebDAV cache index");
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
    return result;
}

}  // namespace omatrack
