// Sidebar openIndex cache under XDG_CACHE_HOME.
//
// Keyed by POSIX (dev, ino, size, mtime) plus converterGeneration() so a
// decoder pin bump invalidates every entry. Failures are never stored.
// Nothing is written beside the source recording.
#pragma once

#include <QJsonObject>
#include <QtGlobal>
#include <QString>

namespace omatrack {

struct FileIdentity {
    quint64 device = 0;
    quint64 inode = 0;
    qint64 size = -1;
    qint64 mtimeNs = 0;
    QString generation;
    bool valid = false;
};

/// Identity of `path` plus `omatrack::converterGeneration()`. Invalid when
/// the file cannot be stated.
FileIdentity fileIdentity(const QString& path);

/// `$XDG_CACHE_HOME/omatrack/index/{generation}/…`. Empty when identity is
/// invalid.
QString indexCachePath(const FileIdentity& identity);

/// Stored JSON, or an empty object on miss / malformed / generation mismatch.
QJsonObject loadIndexCache(const QString& path);

/// Write a successful openIndex snapshot. No-op when identity is invalid.
bool storeIndexCache(const QString& path, const QJsonObject& metadata);

}  // namespace omatrack
