// Pure helpers for the portable Linux/Windows/macOS updater.
//
// Parsing, version comparison, checksums, the AppImage swap, and the
// Windows/macOS apply-scripts live here so the unit tests can cover them
// without QML, HTTP, or TelemetryStore. The QObject that talks to GitHub
// and the header is AppUpdater.
#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QtGlobal>
#include <QUrl>
#include <memory>
#include <optional>

namespace omatrack {

enum class UpdateChannel {
    LinuxAppImage,
    WindowsZip,
    WindowsNupkg,
    WindowsSetup,
    MacDmg
};

struct GithubRelease {
    QString tag;
    QString version;
    QString notes;
    QString htmlUrl;
    QString linuxAssetName;
    QUrl linuxAssetUrl;
    qint64 linuxAssetSize = -1;
    QString windowsAssetName;
    QUrl windowsAssetUrl;
    qint64 windowsAssetSize = -1;
    QString windowsNupkgName;
    QUrl windowsNupkgUrl;
    qint64 windowsNupkgSize = -1;
    QString windowsSetupName;
    QUrl windowsSetupUrl;
    qint64 windowsSetupSize = -1;
    QString macAssetName;
    QUrl macAssetUrl;
    qint64 macAssetSize = -1;
    /// Filled by selectReleaseAsset() for the channel being installed.
    QString assetName;
    QUrl assetUrl;
    qint64 assetSize = -1;
    QUrl checksumsUrl;
};

/// Strip a leading `v` and any `+metadata` / `-prerelease` suffix.
QString normalizeVersion(const QString& version);

/// Semver-ish compare of the first three numeric components. Missing
/// components are zero, so `1.2` equals `1.2.0`.
int compareVersions(const QString& left, const QString& right);

inline bool versionIsNewer(const QString& latest, const QString& current) {
    return compareVersions(latest, current) > 0;
}

#ifdef Q_OS_WIN
inline UpdateChannel currentUpdateChannel() {
    return UpdateChannel::WindowsNupkg;
}
#elif defined(Q_OS_MACOS)
inline UpdateChannel currentUpdateChannel() { return UpdateChannel::MacDmg; }
#else
inline UpdateChannel currentUpdateChannel() {
    return UpdateChannel::LinuxAppImage;
}
#endif

/// Reads GitHub's `/releases/latest` JSON and records the Linux AppImage,
/// Windows zip, and macOS disk image when present, plus SHA256SUMS.
std::optional<GithubRelease> parseGithubRelease(const QByteArray& body);

/// Copies the channel's asset into `assetName` / `assetUrl` / `assetSize`.
bool selectReleaseAsset(GithubRelease* release, UpdateChannel channel);

/// Prefers the Velopack nupkg when `updateExe` is present, else the
/// Setup.exe so a zip install can migrate, else the portable zip.
bool selectWindowsUpdateAsset(GithubRelease* release, bool velopackInstall);

/// SHA-256 hex for `fileName` from a `sha256sum` listing, or empty.
QString checksumForFile(const QByteArray& sums, const QString& fileName);

/// SHA-256 hex of a file, or empty on I/O failure.
QString fileSha256(const QString& path);

/// Replace `currentPath` with `incomingPath` (same directory). The running
/// AppImage stays mounted; only the on-disk file changes. Returns false
/// without leaving the original missing.
bool replaceAppImage(const QString& currentPath, const QString& incomingPath,
                     QString* error = nullptr);

/// Directory that actually holds `omatrack.exe` after a zip extract: the
/// extract root, or its single child folder when the archive wrapped one.
QString windowsPayloadRoot(const QString& extracted);

/// cmd.exe script: wait for `pid`, robocopy `source` over `dest`, relaunch,
/// then delete the staging tree and the script.
QString windowsApplyScript(qint64 pid, const QString& source,
                           const QString& dest, const QStringList& relaunchArgs,
                           const QString& cleanupDir = {});

/// Path of `Omatrack.app` (or the sole `.app`) under a DMG mount or copy.
QString macPayloadRoot(const QString& extracted);

/// /bin/bash script: wait for `pid`, replace `dest` with `source` via ditto,
/// relaunch, then delete the staging tree and the script.
QString macApplyScript(qint64 pid, const QString& source, const QString& dest,
                       const QStringList& relaunchArgs,
                       const QString& cleanupDir = {});

/// Where a release asset should land before verify, and how to apply it once
/// verified. One concrete class per platform; the shared download + checksum
/// orchestration stays in AppUpdater, which calls apply() only after the part
/// file has been downloaded and its SHA-256 checked.
struct UpdateInstallArgs {
    /// The downloaded, verified part file.
    QString partPath;
    /// The current install location (AppImage, Windows install dir, .app).
    QString installPath;
    /// The version being installed.
    QString version;
    /// PID of the running process a helper script waits for.
    qint64 pid = 0;
    /// Args to relaunch the app with after the swap.
    QStringList relaunchArgs;
    /// Path to Velopack's Update.exe when present (Windows), else empty.
    QString updateExe;
};

struct UpdateInstallOutcome {
    bool ok = false;
    /// True when a detached helper process performs the swap after this one
    /// exits; `helperPath`/`helperArgs` describe how to launch it.
    bool relaunchWithHelper = false;
    QString helperPath;
    QStringList helperArgs;
    QString error;
};

class UpdateInstaller {
public:
    virtual ~UpdateInstaller() = default;
    /// Where to download the asset into, given the install path and version.
    virtual QString partPath(const QString& installPath,
                             const QString& version) const = 0;
    /// Apply the verified part file. On success, outcome.ok and the relaunch
    /// fields describe how to restart.
    virtual UpdateInstallOutcome apply(const UpdateInstallArgs& args) = 0;
};

}  // namespace omatrack
