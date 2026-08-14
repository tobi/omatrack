// Verbose I/O and seek logging. Off unless --verbose or OMATRACK_VERBOSE=1.
#pragma once

#include <QLoggingCategory>
#include <QString>
#include <QUrl>

Q_DECLARE_LOGGING_CATEGORY(lcIo)
Q_DECLARE_LOGGING_CATEGORY(lcSeek)

namespace omatrack {

void setVerbose(bool enabled);
bool isVerbose();

/// Compact size for log lines: `640 B`, `12.4 KiB`, `1.8 MiB`.
QString formatBytes(qint64 bytes);

/// Home- and cache-relative path. Never used for a signed stream URL.
QString displayPath(const QString& path);

/// Local files go through displayPath; remote URLs drop userinfo and query
/// so a presigned signature never lands in the log.
QString displayUrl(const QUrl& url);

}  // namespace omatrack
