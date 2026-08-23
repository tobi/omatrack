#include "AppUpdate.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

#include <algorithm>

namespace omatrack {
namespace {

QString stripVersionDecorations(QString version) {
    version = version.trimmed();
    if (version.startsWith(QLatin1Char('v')) ||
        version.startsWith(QLatin1Char('V')))
        version = version.mid(1);
    const int plus = version.indexOf(QLatin1Char('+'));
    if (plus >= 0) version = version.left(plus);
    const int dash = version.indexOf(QLatin1Char('-'));
    if (dash >= 0) version = version.left(dash);
    return version.trimmed();
}

QFile::Permissions executablePermissions() {
    return QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner |
           QFile::ReadGroup | QFile::ExeGroup | QFile::ReadOther |
           QFile::ExeOther;
}

}  // namespace

QString normalizeVersion(const QString& version) {
    return stripVersionDecorations(version);
}

int compareVersions(const QString& left, const QString& right) {
    const QString a = stripVersionDecorations(left);
    const QString b = stripVersionDecorations(right);
    const QStringList leftParts = a.split(QLatin1Char('.'));
    const QStringList rightParts = b.split(QLatin1Char('.'));
    const qsizetype widest = (std::max)(leftParts.size(), rightParts.size());
    const int count = int((std::max)(qsizetype{3}, widest));
    for (int i = 0; i < count; ++i) {
        const int lv = i < leftParts.size() ? leftParts.at(i).toInt() : 0;
        const int rv = i < rightParts.size() ? rightParts.at(i).toInt() : 0;
        if (lv < rv) return -1;
        if (lv > rv) return 1;
    }
    return 0;
}

std::optional<GithubRelease> parseGithubRelease(const QByteArray& body) {
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(body, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject())
        return std::nullopt;

    const QJsonObject object = document.object();
    GithubRelease release;
    release.tag = object.value(QStringLiteral("tag_name")).toString().trimmed();
    release.version = normalizeVersion(release.tag);
    release.notes = object.value(QStringLiteral("body")).toString();
    release.htmlUrl =
        object.value(QStringLiteral("html_url")).toString().trimmed();
    if (release.version.isEmpty()) return std::nullopt;

    static const QRegularExpression appImageName(
        QStringLiteral("^Omatrack-.+-linux-x86_64\\.AppImage$"));
    static const QRegularExpression windowsZipName(
        QStringLiteral("^Omatrack-.+-windows-x86_64\\.zip$"));
    static const QRegularExpression windowsNupkgName(
        QStringLiteral("^io\\.github\\.tobi\\.omatrack-.+-full\\.nupkg$"));
    static const QRegularExpression windowsSetupName(
        QStringLiteral("^Omatrack-.+-windows-x86_64-Setup\\.exe$"));
    static const QRegularExpression macDmgName(
        QStringLiteral("^Omatrack-.+-macOS-arm64\\.dmg$"));
    const QJsonArray assets = object.value(QStringLiteral("assets")).toArray();
    for (const QJsonValue& value : assets) {
        const QJsonObject asset = value.toObject();
        const QString name = asset.value(QStringLiteral("name")).toString();
        const QUrl url(
            asset.value(QStringLiteral("browser_download_url")).toString());
        if (!url.isValid()) continue;
        const qint64 size =
            asset.value(QStringLiteral("size")).toVariant().toLongLong();
        if (appImageName.match(name).hasMatch()) {
            release.linuxAssetName = name;
            release.linuxAssetUrl = url;
            release.linuxAssetSize = size;
        } else if (windowsZipName.match(name).hasMatch()) {
            release.windowsAssetName = name;
            release.windowsAssetUrl = url;
            release.windowsAssetSize = size;
        } else if (windowsNupkgName.match(name).hasMatch()) {
            release.windowsNupkgName = name;
            release.windowsNupkgUrl = url;
            release.windowsNupkgSize = size;
        } else if (windowsSetupName.match(name).hasMatch()) {
            release.windowsSetupName = name;
            release.windowsSetupUrl = url;
            release.windowsSetupSize = size;
        } else if (macDmgName.match(name).hasMatch()) {
            release.macAssetName = name;
            release.macAssetUrl = url;
            release.macAssetSize = size;
        } else if (name.startsWith(QStringLiteral("SHA256SUMS"))) {
            release.checksumsUrl = url;
        }
    }
    if (release.linuxAssetName.isEmpty() &&
        release.windowsAssetName.isEmpty() &&
        release.windowsNupkgName.isEmpty() &&
        release.windowsSetupName.isEmpty() && release.macAssetName.isEmpty())
        return std::nullopt;
    return release;
}

bool selectReleaseAsset(GithubRelease* release, UpdateChannel channel) {
    if (!release) return false;
    if (channel == UpdateChannel::WindowsZip) {
        if (release->windowsAssetName.isEmpty() ||
            !release->windowsAssetUrl.isValid())
            return false;
        release->assetName = release->windowsAssetName;
        release->assetUrl = release->windowsAssetUrl;
        release->assetSize = release->windowsAssetSize;
        return true;
    }
    if (channel == UpdateChannel::WindowsNupkg) {
        if (release->windowsNupkgName.isEmpty() ||
            !release->windowsNupkgUrl.isValid())
            return false;
        release->assetName = release->windowsNupkgName;
        release->assetUrl = release->windowsNupkgUrl;
        release->assetSize = release->windowsNupkgSize;
        return true;
    }
    if (channel == UpdateChannel::WindowsSetup) {
        if (release->windowsSetupName.isEmpty() ||
            !release->windowsSetupUrl.isValid())
            return false;
        release->assetName = release->windowsSetupName;
        release->assetUrl = release->windowsSetupUrl;
        release->assetSize = release->windowsSetupSize;
        return true;
    }
    if (channel == UpdateChannel::MacDmg) {
        if (release->macAssetName.isEmpty() || !release->macAssetUrl.isValid())
            return false;
        release->assetName = release->macAssetName;
        release->assetUrl = release->macAssetUrl;
        release->assetSize = release->macAssetSize;
        return true;
    }
    if (release->linuxAssetName.isEmpty() || !release->linuxAssetUrl.isValid())
        return false;
    release->assetName = release->linuxAssetName;
    release->assetUrl = release->linuxAssetUrl;
    release->assetSize = release->linuxAssetSize;
    return true;
}

bool selectWindowsUpdateAsset(GithubRelease* release, bool velopackInstall) {
    if (velopackInstall &&
        selectReleaseAsset(release, UpdateChannel::WindowsNupkg))
        return true;
    if (selectReleaseAsset(release, UpdateChannel::WindowsSetup)) return true;
    return selectReleaseAsset(release, UpdateChannel::WindowsZip);
}

QString checksumForFile(const QByteArray& sums, const QString& fileName) {
    if (fileName.isEmpty()) return {};
    const QString listing = QString::fromUtf8(sums);
    static const QRegularExpression lineBreaks(QStringLiteral("[\r\n]+"));
    static const QRegularExpression entry(
        QStringLiteral("^([0-9a-fA-F]{64})\\s+\\*?(\\S+)\\s*$"));
    const QStringList lines = listing.split(lineBreaks, Qt::SkipEmptyParts);
    for (const QString& raw : lines) {
        const QString line = raw.trimmed();
        if (line.startsWith(QLatin1Char('#'))) continue;
        const QRegularExpressionMatch match = entry.match(line);
        if (!match.hasMatch()) continue;
        if (match.captured(2) == fileName) return match.captured(1).toLower();
    }
    return {};
}

QString fileSha256(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        const QByteArray chunk = file.read(64 * 1024);
        if (chunk.isEmpty() && file.error() != QFile::NoError) return {};
        hash.addData(chunk);
    }
    return QString::fromLatin1(hash.result().toHex());
}

bool replaceAppImage(const QString& currentPath, const QString& incomingPath,
                     QString* error) {
    const auto fail = [error](const QString& message) {
        if (error) *error = message;
        return false;
    };
    if (currentPath.isEmpty() || incomingPath.isEmpty())
        return fail(QStringLiteral("Missing AppImage path"));
    if (currentPath == incomingPath)
        return fail(QStringLiteral("Update file is the running AppImage"));

    QFileInfo currentInfo(currentPath);
    QFileInfo incomingInfo(incomingPath);
    if (!incomingInfo.exists() || incomingInfo.size() <= 0)
        return fail(QStringLiteral("Downloaded AppImage is missing"));
    if (!currentInfo.exists())
        return fail(QStringLiteral("Current AppImage is missing"));
    if (QDir::cleanPath(currentInfo.absolutePath()) !=
        QDir::cleanPath(incomingInfo.absolutePath()))
        return fail(QStringLiteral("Update must land next to the AppImage"));

    if (!QFile::setPermissions(incomingPath, executablePermissions()))
        return fail(
            QStringLiteral("Unable to mark the new AppImage executable"));

    const QString backupPath = currentPath + QStringLiteral(".old");
    QFile::remove(backupPath);
    if (!QFile::rename(currentPath, backupPath))
        return fail(
            QStringLiteral("Unable to move the running AppImage aside"));
    if (!QFile::rename(incomingPath, currentPath)) {
        QFile::rename(backupPath, currentPath);
        return fail(QStringLiteral("Unable to install the new AppImage"));
    }
    QFile::setPermissions(currentPath, executablePermissions());
    QFile::remove(backupPath);
    return true;
}

QString windowsPayloadRoot(const QString& extracted) {
    if (extracted.isEmpty()) return {};
    const QDir root(extracted);
    if (root.exists(QStringLiteral("omatrack.exe")))
        return QFileInfo(extracted).absoluteFilePath();
    const QFileInfoList children =
        root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
    if (children.size() != 1) return {};
    const QString nested = children.front().absoluteFilePath();
    if (QDir(nested).exists(QStringLiteral("omatrack.exe"))) return nested;
    return {};
}

namespace {

QString nativeDir(const QString& path) {
    QString native =
        QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
    while (native.endsWith(QLatin1Char('\\')) ||
           native.endsWith(QLatin1Char('/')))
        native.chop(1);
    return native;
}

QString quoteCmd(const QString& value) {
    if (value.isEmpty()) return QStringLiteral("\"\"");
    static const QRegularExpression special(
        QStringLiteral("[ \\t&()\\[\\]{}^=;!'+,`~|<>\"]"));
    if (!special.match(value).hasMatch()) return value;
    QString quoted = value;
    quoted.replace(QLatin1Char('"'), QStringLiteral("\"\""));
    return QLatin1Char('"') + quoted + QLatin1Char('"');
}

}  // namespace

QString windowsApplyScript(qint64 pid, const QString& source,
                           const QString& dest, const QStringList& relaunchArgs,
                           const QString& cleanupDir) {
    const QString src = nativeDir(source);
    const QString dst = nativeDir(dest);
    const QString cleanup =
        nativeDir(cleanupDir.isEmpty() ? source : cleanupDir);
    const QString exe = dst + QStringLiteral("\\omatrack.exe");
    QString start =
        QStringLiteral("start \"\" /D %1 %2").arg(quoteCmd(dst), quoteCmd(exe));
    for (const QString& argument : relaunchArgs)
        start += QLatin1Char(' ') + quoteCmd(argument);
    return QStringLiteral(
               "@echo off\r\n"
               "setlocal EnableExtensions\r\n"
               "set \"PID=%1\"\r\n"
               "powershell.exe -NoProfile -NonInteractive -Command "
               "\"Wait-Process -Id %PID% -ErrorAction SilentlyContinue\"\r\n"
               "timeout /t 1 /nobreak >nul\r\n"
               "robocopy %2 %3 /E /IS /IT /R:5 /W:1 /NFL /NDL /NJH /NJS\r\n"
               "if errorlevel 8 exit /b 1\r\n"
               "%4\r\n"
               "rmdir /s /q %5\r\n"
               "del \"%~f0\"\r\n")
        .arg(QString::number(pid), quoteCmd(src), quoteCmd(dst), start,
             quoteCmd(cleanup));
}

QString macPayloadRoot(const QString& extracted) {
    if (extracted.isEmpty()) return {};
    const QFileInfo info(extracted);
    const auto isAppBundle = [](const QString& path) {
        return QFileInfo::exists(path +
                                 QStringLiteral("/Contents/Info.plist")) &&
               QFileInfo::exists(path + QStringLiteral("/Contents/MacOS"));
    };
    if (info.fileName().endsWith(QStringLiteral(".app")) &&
        isAppBundle(info.absoluteFilePath()))
        return info.absoluteFilePath();
    const QString named =
        QDir(extracted).filePath(QStringLiteral("Omatrack.app"));
    if (isAppBundle(named)) return QFileInfo(named).absoluteFilePath();
    const QFileInfoList apps =
        QDir(extracted).entryInfoList({QStringLiteral("*.app")}, QDir::Dirs);
    if (apps.size() == 1 && isAppBundle(apps.front().absoluteFilePath()))
        return apps.front().absoluteFilePath();
    return {};
}

QString quoteSh(const QString& value) {
    QString escaped = value;
    escaped.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QLatin1Char('\'') + escaped + QLatin1Char('\'');
}

QString macApplyScript(qint64 pid, const QString& source, const QString& dest,
                       const QStringList& relaunchArgs,
                       const QString& cleanupDir) {
    const QString src = QFileInfo(source).absoluteFilePath();
    const QString dst = QFileInfo(dest).absoluteFilePath();
    const QString cleanup =
        QFileInfo(cleanupDir.isEmpty() ? source : cleanupDir)
            .absoluteFilePath();
    QString open = QStringLiteral("open -n %1").arg(quoteSh(dst));
    if (!relaunchArgs.isEmpty()) {
        open += QStringLiteral(" --args");
        for (const QString& argument : relaunchArgs)
            open += QLatin1Char(' ') + quoteSh(argument);
    }
    return QStringLiteral(
               "#!/bin/bash\n"
               "set -euo pipefail\n"
               "PID=%1\n"
               "while kill -0 \"$PID\" 2>/dev/null; do sleep 1; done\n"
               "sleep 1\n"
               "rm -rf %3\n"
               "ditto %2 %3\n"
               "xattr -dr com.apple.quarantine %3 2>/dev/null || true\n"
               "%4\n"
               "rm -rf %5\n"
               "rm -f -- \"$0\"\n")
        .arg(QString::number(pid), quoteSh(src), quoteSh(dst), open,
             quoteSh(cleanup));
}

}  // namespace omatrack
