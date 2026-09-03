#include "PathJail.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QVector>

namespace omatrack {
namespace {

bool hasNull(const QString& text) { return text.contains(QChar(0)); }

bool isWindowsDrive(const QString& text) {
    if (text.size() < 2) return false;
    const QChar letter = text[0];
    if (!letter.isLetter()) return false;
    return text[1] == QLatin1Char(':');
}

bool hasDotDotComponent(const QString& cleaned) {
    const QStringList parts = QDir::fromNativeSeparators(cleaned).split(
        QLatin1Char('/'), Qt::SkipEmptyParts);
    return parts.contains(QStringLiteral(".."));
}

QString canonicalDirectoryWithMissingTail(const QString& path) {
    QString ancestor = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    QStringList tail;
    for (;;) {
        const QFileInfo info(ancestor);
        if (info.exists() || info.isSymbolicLink()) {
            // Resolve the nearest existing ancestor, not just the immediate
            // parent: canonicalFilePath() is empty for a missing tail and
            // otherwise conceals symlinks above it. Broken links fail closed.
            const QString canonical = info.canonicalFilePath();
            if (canonical.isEmpty() || !info.isDir()) return {};
            QString resolved = canonical;
            for (const auto& part : tail)
                resolved = QDir(resolved).filePath(part);
            return QDir::cleanPath(resolved);
        }
        const QString parent = info.absolutePath();
        if (parent == ancestor) return {};
        tail.prepend(info.fileName());
        ancestor = parent;
    }
}

bool staysUnder(const QString& parent, const QString& dest) {
    if (parent == dest) return true;
    const QString prefix =
        dest.endsWith(QLatin1Char('/')) ? dest : dest + QLatin1Char('/');
    return parent.startsWith(prefix);
}

}  // namespace

QString defaultCopyFormat() {
    return QStringLiteral("{track}/{date}/{session}/{original}");
}

const QRegularExpression& formatTokenPattern() {
    static const QRegularExpression token(
        QStringLiteral("\\{([A-Za-z0-9_]+)\\}"));
    return token;
}

QString unknownFormatToken(const QString& format, const QVariantMap& ctx) {
    QRegularExpressionMatchIterator it =
        formatTokenPattern().globalMatch(format);
    while (it.hasNext()) {
        const QString key = it.next().captured(1);
        if (!ctx.contains(key)) return key;
    }
    return {};
}

QString expandCopyFormat(const QString& format, const QVariantMap& ctx) {
    QString result = format;
    QRegularExpressionMatchIterator it =
        formatTokenPattern().globalMatch(format);
    QVector<QRegularExpressionMatch> matches;
    while (it.hasNext()) matches.append(it.next());
    for (int i = matches.size() - 1; i >= 0; --i) {
        const QRegularExpressionMatch match = matches.at(i);
        const QString key = match.captured(1);
        if (!ctx.contains(key)) continue;
        const QString value = ctx.value(key).toString();
        result.replace(match.capturedStart(), match.capturedLength(), value);
    }
    return result;
}

bool isForbiddenRelative(const QString& relative) {
    if (relative.isEmpty() || hasNull(relative)) return true;
    const QString slash = QDir::fromNativeSeparators(relative.trimmed());
    if (slash.startsWith(QLatin1Char('/'))) return true;
    if (slash.startsWith(QStringLiteral("//"))) return true;
    if (isWindowsDrive(slash)) return true;
    if (relative.contains(QStringLiteral("\\\\"))) return true;
    if (relative.contains(QChar(0))) return true;
    return false;
}

PathJailResult jailRelativePath(const QString& destRoot,
                                const QString& relative) {
    PathJailResult result;
    if (destRoot.trimmed().isEmpty()) {
        result.error = QStringLiteral("Destination folder is empty");
        return result;
    }
    if (relative.trimmed().isEmpty()) {
        result.error = QStringLiteral("Rename produced an empty path");
        return result;
    }
    if (hasNull(relative)) {
        result.error = QStringLiteral("Rename produced a null byte");
        return result;
    }
    if (isForbiddenRelative(relative)) {
        result.error = QStringLiteral("Rename produced an absolute path");
        return result;
    }
    const QString cleanedRelative =
        QDir::cleanPath(QDir::fromNativeSeparators(relative));
    if (cleanedRelative.isEmpty() || cleanedRelative == QLatin1Char('.') ||
        cleanedRelative == QStringLiteral("..") ||
        hasDotDotComponent(cleanedRelative) ||
        isForbiddenRelative(cleanedRelative)) {
        result.error = QStringLiteral("Rename escaped the destination");
        return result;
    }

    const QString dest = canonicalDirectoryWithMissingTail(destRoot);
    if (dest.isEmpty()) {
        result.error =
            QStringLiteral("Destination is not a resolvable directory");
        return result;
    }
    const QString full = QDir::cleanPath(QDir(dest).filePath(cleanedRelative));
    if (full == dest) {
        result.error = QStringLiteral("Rename targeted the destination root");
        return result;
    }
    const QString parent = QFileInfo(full).path();
    const QString parentCanon = canonicalDirectoryWithMissingTail(parent);
    if (parentCanon.isEmpty() || !staysUnder(parentCanon, dest)) {
        result.error = QStringLiteral("Rename escaped the destination");
        return result;
    }
    result.ok = true;
    result.absolutePath =
        QDir(parentCanon).filePath(QFileInfo(full).fileName());
    result.relativePath = cleanedRelative;
    return result;
}

}  // namespace omatrack
