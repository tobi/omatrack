// WebDAV discovery and content cache for remote telemetry sources.
#pragma once

#include <QString>
#include <QStringList>

#include <QUrl>

#include <cstdint>

namespace omatrack {

struct WebDavConnection {
    QString id;
    QString name;
    QString url;
    QString username;
    QString password;
    bool enabled = true;
};

struct WebDavSyncResult {
    QString id;
    QString cachePath;
    QString status;
    QString error;
    QStringList files;
    qint64 downloadedBytes = 0;
    bool fromCache = false;
    bool success = false;
};

class WebDavCache {
public:
    static QString connectionId(const QString& url, const QString& username);
    static QString cachePath(const WebDavConnection& connection);
    static WebDavSyncResult sync(const WebDavConnection& connection);
};

}  // namespace omatrack
