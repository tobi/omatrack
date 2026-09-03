// Destination-path jail for USB copy and Lua rename.
//
// A rename script or format string may only return a relative path. After
// QDir::cleanPath(dest/relative) the parent must stay under dest; absolute
// paths, null bytes, `..`, Windows drives, and UNC prefixes are rejected.
#pragma once

#include <QString>
#include <QVariantMap>

namespace omatrack {

struct PathJailResult {
    bool ok = false;
    QString absolutePath;
    QString relativePath;
    QString error;
};

/// Default copy layout: `{track}/{date}/{session}/{original}`.
QString defaultCopyFormat();

/// First unknown `{token}` in `format`, or empty when every token resolves.
/// The copy planner flags these; expansion below is the validated path.
QString unknownFormatToken(const QString& format, const QVariantMap& ctx);

/// Substitute `{token}` placeholders from `ctx`. Unknown tokens stay as-is,
/// so validate with unknownFormatToken() first.
QString expandCopyFormat(const QString& format, const QVariantMap& ctx);

/// True when `relative` is an absolute Unix path, a Windows drive path, or UNC.
bool isForbiddenRelative(const QString& relative);

/// Resolve `relative` under `destRoot`. Never creates files.
PathJailResult jailRelativePath(const QString& destRoot,
                                const QString& relative);

}  // namespace omatrack
