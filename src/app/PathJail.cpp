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

QString destCanonical(const QString& destRoot) {
    const QString absolute =
        QDir::cleanPath(QFileInfo(destRoot).absoluteFilePath());
    const QString canonical = QFileInfo(absolute).canonicalFilePath();
    return canonical.isEmpty() ? absolute : canonical;
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

QString expandCopyFormat(const QString& format, const QVariantMap& ctx) {
    QString result = format;
    static const QRegularExpression token(
        QStringLiteral("\\{([A-Za-z0-9_]+)\\}"));
    QRegularExpressionMatchIterator it = token.globalMatch(format);
    QVector<QRegularExpressionMatch> matches;
    while (it.hasNext()) matches.append(it.next());
    for (int i = matches.size() - 1; i >= 0; --i) {
        const QRegularExpressionMatch match = matches.at(i);
        const QString key = match.captured(1);
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

    const QString dest = destCanonical(destRoot);
    const QString full = QDir::cleanPath(QDir(dest).filePath(cleanedRelative));
    if (full == dest) {
        result.error = QStringLiteral("Rename targeted the destination root");
        return result;
    }
    const QString parent = QFileInfo(full).path();
    QString parentCanon = QFileInfo(parent).canonicalFilePath();
    if (parentCanon.isEmpty()) parentCanon = QDir::cleanPath(parent);
    if (!staysUnder(parentCanon, dest)) {
        result.error = QStringLiteral("Rename escaped the destination");
        return result;
    }
    result.ok = true;
    result.absolutePath = full;
    result.relativePath = cleanedRelative;
    return result;
}

}  // namespace omatrack
