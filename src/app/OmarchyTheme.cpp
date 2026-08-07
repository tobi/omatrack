#include "OmarchyTheme.h"

#ifdef Q_OS_LINUX
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStringList>

namespace {

QVariantMap loadOmarchyColors(const QString& currentStatePath) {
    QVariantMap colors;
    QFile file(currentStatePath + QStringLiteral("/theme/colors.toml"));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return colors;
    static const QRegularExpression assignment(
        QStringLiteral("^\\s*([a-z_]+)\\s*=\\s*\"(#[0-9a-fA-F]{6,8})\""));
    const QString text = QString::fromUtf8(file.readAll());
    const QStringList lines = text.split('\n');
    for (const QString& line : lines) {
        const QRegularExpressionMatch match = assignment.match(line);
        if (match.hasMatch())
            colors.insert(match.captured(1), match.captured(2));
    }
    return colors;
}

}  // namespace
#endif

OmarchyTheme::OmarchyTheme(QObject* parent) : QObject(parent) {
#ifdef Q_OS_LINUX
    const QString omarchyPath = qEnvironmentVariable(
        "OMARCHY_PATH", QStringLiteral("/usr/share/omarchy"));
    const QString stateHome = qEnvironmentVariable(
        "XDG_STATE_HOME", QDir::homePath() + QStringLiteral("/.local/state"));
    currentStatePath_ = stateHome + QStringLiteral("/omarchy/current");

    // The session plus both paths are required so an Omarchy installation or
    // stale/copyable state never enables integration on another desktop.
    const bool omarchySession =
        qEnvironmentVariable("DESKTOP_SESSION")
            .compare(QStringLiteral("omarchy"), Qt::CaseInsensitive) == 0;
    if (!omarchySession || !QFileInfo(omarchyPath).isDir() ||
        !QFileInfo(currentStatePath_).isDir()) {
        currentStatePath_.clear();
        return;
    }

    reloadTimer_.setSingleShot(true);
    reloadTimer_.setInterval(100);
    connect(&reloadTimer_, &QTimer::timeout, this, &OmarchyTheme::reload);
    connect(&watcher_, &QFileSystemWatcher::directoryChanged, this,
            [this] { reloadTimer_.start(); });
    connect(&watcher_, &QFileSystemWatcher::fileChanged, this,
            [this] { reloadTimer_.start(); });

    reload();
#endif
}

#ifdef Q_OS_LINUX
void OmarchyTheme::reload() {
    if (currentStatePath_.isEmpty()) return;

    const QVariantMap nextColors = loadOmarchyColors(currentStatePath_);
    refreshWatchPaths();
    if (nextColors == colors_) return;

    colors_ = nextColors;
    emit colorsChanged();
}

void OmarchyTheme::refreshWatchPaths() {
    if (!watcher_.files().isEmpty()) watcher_.removePaths(watcher_.files());
    if (!watcher_.directories().isEmpty())
        watcher_.removePaths(watcher_.directories());

    const QStringList candidates{
        currentStatePath_,
        currentStatePath_ + QStringLiteral("/theme"),
        currentStatePath_ + QStringLiteral("/theme.name"),
        currentStatePath_ + QStringLiteral("/theme/colors.toml"),
    };
    for (const QString& path : candidates) {
        if (QFileInfo::exists(path)) watcher_.addPath(path);
    }
}
#endif
