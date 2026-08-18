// WebDAV: PROPFIND discovery over http(s) with optional Basic credentials.

#include "RemoteCache.h"

#include <QDir>
#include <QNetworkRequest>
#include <QSet>
#include <QUrl>
#include <QXmlStreamReader>

#include <utility>

namespace omatrack {
namespace {

/// A directory tree can nest arbitrarily; this bounds the Depth: 1 walk so a
/// server that keeps answering never keeps us recursing.
constexpr int kMaximumDepth = 64;

const QByteArray kPropfindBody(
    "<?xml version=\"1.0\" encoding=\"utf-8\" ?>"
    "<propfind xmlns=\"DAV:\"><prop>"
    "<resourcetype/><getetag/><getlastmodified/><getcontentlength/>"
    "</prop></propfind>");

/// A PROPFIND response row, which unlike a RemoteObject may be a directory.
struct Resource {
    RemoteObject object;
    bool collection = false;
};

QUrl normalizedRoot(const QString& target) {
    QUrl root(target.trimmed());
    QString path = root.path();
    if (!path.endsWith(QLatin1Char('/'))) path.append(QLatin1Char('/'));
    root.setPath(path);
    root.setFragment({});
    return root;
}

QByteArray authorizationHeader(const RemoteConnection& connection) {
    if (connection.username.isEmpty()) return {};
    return (connection.username + QLatin1Char(':') + connection.password)
        .toUtf8()
        .toBase64();
}

void signRequest(QNetworkRequest& request, const RemoteConnection& connection) {
    request.setRawHeader("User-Agent", "Omatrack-WebDAV/1");
    const QByteArray auth = authorizationHeader(connection);
    if (!auth.isEmpty()) request.setRawHeader("Authorization", "Basic " + auth);
}

/// Builds a signed PROPFIND for one URL, ready to hand to sendFollowing().
RequestFactory propfindFactory(const RemoteConnection& connection,
                               const QByteArray& depth) {
    return [connection, depth](const QUrl& url) {
        QNetworkRequest request = makeRequest(url);
        request.setRawHeader("Depth", depth);
        request.setHeader(QNetworkRequest::ContentTypeHeader,
                          QStringLiteral("application/xml; charset=utf-8"));
        signRequest(request, connection);
        return request;
    };
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
                current.object.url = url;
            } else if (inResponse && name == QStringLiteral("collection")) {
                current.collection = true;
            } else if (inResponse && name == QStringLiteral("getetag")) {
                current.object.etag = xml.readElementText();
            } else if (inResponse &&
                       name == QStringLiteral("getlastmodified")) {
                current.object.modified = xml.readElementText();
            } else if (inResponse &&
                       name == QStringLiteral("getcontentlength")) {
                current.object.size = xml.readElementText().toLongLong();
            }
        } else if (xml.isEndElement() &&
                   xml.name() == QStringLiteral("response")) {
            if (!current.object.url.isValid()) {
                if (error)
                    *error = QStringLiteral("WebDAV response has no URL");
                return false;
            }
            const QString rootPath = root.path();
            const QString resourcePath = current.object.url.path();
            if (resourcePath.startsWith(rootPath)) {
                const QByteArray encodedRelative =
                    resourcePath.mid(rootPath.size()).toUtf8();
                QString relative = QUrl::fromPercentEncoding(encodedRelative);
                while (relative.endsWith(QLatin1Char('/'))) relative.chop(1);
                const QString clean = QDir::cleanPath(relative);
                // A server is free to answer with any href it likes, so treat
                // the path as untrusted: never let one escape the cache root.
                if (!clean.isEmpty() && clean != QStringLiteral(".") &&
                    clean != QStringLiteral("..") &&
                    !clean.startsWith(QStringLiteral("../")) &&
                    !clean.startsWith(QLatin1Char('/'))) {
                    current.object.relativePath = clean;
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

bool collectDepthOne(const RemoteConnection& connection, const QUrl& url,
                     const QUrl& root, int depth, QSet<QString>* visited,
                     QVector<Resource>* resources, QString* error,
                     const IoCancel& cancel) {
    if (ioCancelled(cancel)) {
        if (error) *error = QStringLiteral("Cancelled");
        return false;
    }
    if (depth > kMaximumDepth) {
        if (error)
            *error = QStringLiteral("WebDAV directory nesting is too deep");
        return false;
    }
    const QString key = url.toString(QUrl::FullyEncoded);
    if (visited->contains(key)) return true;
    visited->insert(key);

    const HttpResponse response =
        sendFollowing(url, "PROPFIND", propfindFactory(connection, "1"),
                      kPropfindBody, cancel);
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
        if (!collectDepthOne(connection, resource.object.url, root, depth + 1,
                             visited, resources, error, cancel))
            return false;
    }
    return true;
}

}  // namespace

QString webDavTargetError(const QString& target) {
    const QUrl root = normalizedRoot(target);
    if (!root.isValid() ||
        (root.scheme() != QStringLiteral("http") &&
         root.scheme() != QStringLiteral("https")) ||
        root.host().isEmpty())
        return QStringLiteral("Enter a valid http(s) WebDAV URL");
    return {};
}

RemoteBackend makeWebDavBackend(const RemoteConnection& connection) {
    RemoteBackend backend;

    backend.sign = [connection](QNetworkRequest& request, const QByteArray&,
                                const QByteArray&) {
        signRequest(request, connection);
    };

    backend.objectUrl = [connection](const QString& relative) {
        return normalizedRoot(connection.target).resolved(QUrl(relative));
    };

    // ffmpeg reads the credential straight out of the URL, so the player
    // needs to know nothing about how this server authenticates.
    backend.presign = [connection](const QUrl& objectUrl, int) {
        if (connection.username.isEmpty()) return objectUrl;
        QUrl authenticated = objectUrl;
        authenticated.setUserName(connection.username);
        authenticated.setPassword(connection.password);
        return authenticated;
    };

    backend.list = [connection](QVector<RemoteObject>* objects, QString* scope,
                                QString* error, const IoCancel& cancel) {
        if (scope) scope->clear();  // WebDAV has no SigV4 region.
        const QUrl root = normalizedRoot(connection.target);
        QVector<Resource> resources;

        // One Depth: infinity call is a single round trip for the whole tree.
        // Plenty of servers refuse it, so fall back to walking a directory at
        // a time rather than treating the refusal as an outage.
        const HttpResponse response = sendFollowing(
            root, "PROPFIND", propfindFactory(connection, "infinity"),
            kPropfindBody, cancel);
        if (response.status == 207) {
            if (!parseResources(response.body, root, &resources, error))
                return false;
        } else {
            resources.clear();
            QSet<QString> visited;
            if (!collectDepthOne(connection, root, root, 0, &visited,
                                 &resources, error, cancel))
                return false;
        }

        for (const Resource& resource : std::as_const(resources))
            if (!resource.collection) objects->append(resource.object);
        return true;
    };

    return backend;
}

}  // namespace omatrack
