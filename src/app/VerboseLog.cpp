#include "VerboseLog.h"

#include <QDir>
#include <QStandardPaths>
#include <QUrl>

Q_LOGGING_CATEGORY(lcIo, "omatrack.io", QtWarningMsg)
Q_LOGGING_CATEGORY(lcSeek, "omatrack.seek", QtWarningMsg)

namespace omatrack {
namespace {

bool g_verbose = false;

}  // namespace

void setVerbose(bool enabled) {
    g_verbose = enabled;
    if (!enabled) return;
    QLoggingCategory::setFilterRules(
        QStringLiteral("omatrack.io=true\nomatrack.seek=true"));
    qSetMessagePattern(
        QStringLiteral("%{time HH:mm:ss.zzz} %{category} %{message}"));
}

bool isVerbose() { return g_verbose; }

QString formatBytes(qint64 bytes) {
    if (bytes < 0) return QStringLiteral("?");
    if (bytes < 1024) return QString::number(bytes) + QStringLiteral(" B");
    const double value = double(bytes);
    if (bytes < 1024LL * 1024)
        return QString::number(value / 1024.0, 'f', 1) + QStringLiteral(" KiB");
    if (bytes < 1024LL * 1024 * 1024)
        return QString::number(value / (1024.0 * 1024.0), 'f', 1) +
               QStringLiteral(" MiB");
    return QString::number(value / (1024.0 * 1024.0 * 1024.0), 'f', 2) +
           QStringLiteral(" GiB");
}

QString displayPath(const QString& path) {
    if (path.isEmpty()) return QStringLiteral("-");
    const QString cleaned = QDir::cleanPath(path);
    const QString cache = QDir::cleanPath(
        QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) +
        QStringLiteral("/omatrack"));
    if (!cache.isEmpty() &&
        (cleaned == cache || cleaned.startsWith(cache + QLatin1Char('/'))))
        return QStringLiteral("~/.cache/omatrack") + cleaned.mid(cache.size());
    const QString home = QDir::cleanPath(QDir::homePath());
    if (!home.isEmpty() &&
        (cleaned == home || cleaned.startsWith(home + QLatin1Char('/'))))
        return QLatin1Char('~') + cleaned.mid(home.size());
    return cleaned;
}

QString displayUrl(const QUrl& url) {
    if (!url.isValid() || url.isEmpty()) return QStringLiteral("-");
    if (url.isLocalFile()) return displayPath(url.toLocalFile());
    return url.toDisplayString(QUrl::RemoveUserInfo | QUrl::RemoveQuery);
}

}  // namespace omatrack
