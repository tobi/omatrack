#include "AppUpdater.h"

#include "RemoteCache.h"
#include "VerboseLog.h"
#include "YamlConfig.h"
#ifdef Q_OS_WIN
#include "WindowsAssociations.h"
#endif

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QNetworkRequest>
#include <QProcess>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTimer>

#include <chrono>
#include <memory>

namespace {

constexpr auto kCheckInterval = std::chrono::hours(24);
constexpr int kSnoozeDays = 7;
constexpr int kStartupCheckDelayMs = 4000;
constexpr int kPollIntervalMs = 60 * 60 * 1000;

const QUrl kDefaultReleaseUrl(QStringLiteral(
    "https://api.github.com/repos/tobi/omatrack/releases/latest"));

QString isoNow() {
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
}

QDateTime parseIso(const QString& text) {
    return QDateTime::fromString(text, Qt::ISODate).toUTC();
}

bool envFlag(const char* name) {
    return qEnvironmentVariableIntValue(name) != 0 ||
           !qEnvironmentVariable(name).isEmpty();
}

QString currentInstallPath() {
    const QString overridePath = qEnvironmentVariable("OMATRACK_UPDATE_ROOT");
    if (!overridePath.isEmpty())
        return QFileInfo(overridePath).absoluteFilePath();
#ifdef Q_OS_WIN
    const QString directory = QCoreApplication::applicationDirPath();
    if (QFileInfo::exists(directory + QStringLiteral("/qt.conf")) &&
        QFileInfo::exists(directory + QStringLiteral("/omatrack.exe")))
        return QFileInfo(directory).absoluteFilePath();
#endif
#ifdef Q_OS_MACOS
    if (QSysInfo::currentCpuArchitecture() != QStringLiteral("arm64"))
        return {};
    QDir dir(QCoreApplication::applicationDirPath());
    if (dir.dirName() != QStringLiteral("MacOS")) return {};
    if (!dir.cdUp() || dir.dirName() != QStringLiteral("Contents")) return {};
    if (!dir.cdUp() || !dir.dirName().endsWith(QStringLiteral(".app")))
        return {};
    if (!QFileInfo::exists(dir.filePath(QStringLiteral("Contents/Info.plist"))))
        return {};
    return dir.absolutePath();
#endif
#ifdef Q_OS_LINUX
    const QString appImageOverride =
        qEnvironmentVariable("OMATRACK_UPDATE_APPIMAGE");
    if (!appImageOverride.isEmpty())
        return QFileInfo(appImageOverride).absoluteFilePath();
    const QString appImage = qEnvironmentVariable("APPIMAGE");
    if (!appImage.isEmpty() && QFileInfo::exists(appImage))
        return QFileInfo(appImage).absoluteFilePath();
#endif
    return {};
}

QUrl releaseApiUrl() {
    const QString override = qEnvironmentVariable("OMATRACK_UPDATE_API");
    if (!override.isEmpty()) return QUrl(override);
    return kDefaultReleaseUrl;
}

QNetworkRequest githubRequest(const QUrl& url, const QString& version) {
    QNetworkRequest request = omatrack::makeRequest(url);
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("Omatrack/") + version);
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setRawHeader("X-GitHub-Api-Version", "2022-11-28");
    return request;
}

#ifdef Q_OS_WIN
QString powershellLiteral(const QString& path) {
    QString escaped = path;
    escaped.replace(QLatin1Char('\''), QStringLiteral("''"));
    return QLatin1Char('\'') + escaped + QLatin1Char('\'');
}

bool extractWindowsZip(const QString& zip, const QString& destination,
                       QString* error) {
    QDir().mkpath(destination);
    QProcess unzip;
    unzip.start(
        QStringLiteral("powershell.exe"),
        {QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"),
         QStringLiteral("-ExecutionPolicy"), QStringLiteral("Bypass"),
         QStringLiteral("-Command"),
         QStringLiteral("Expand-Archive -LiteralPath %1 "
                        "-DestinationPath %2 -Force")
             .arg(powershellLiteral(zip), powershellLiteral(destination))});
    if (!unzip.waitForFinished(10 * 60 * 1000)) {
        unzip.kill();
        if (error) *error = QStringLiteral("Unpack timed out");
        return false;
    }
    if (unzip.exitStatus() != QProcess::NormalExit || unzip.exitCode() != 0) {
        const QString detail =
            QString::fromUtf8(unzip.readAllStandardError()).trimmed();
        if (error)
            *error =
                detail.isEmpty() ? QStringLiteral("Unpack failed") : detail;
        return false;
    }
    return true;
}
#endif

bool writableLocation(const QString& path, QString* error) {
    const QFileInfo info(path);
#ifdef Q_OS_WIN
    const QFileInfo directory(info.isDir() ? info.absoluteFilePath()
                                           : info.absolutePath());
    if (!directory.isDir() || !directory.isWritable()) {
        if (error)
            *error = QStringLiteral(
                "Cannot write this Omatrack folder. Copy it to a writable "
                "location.");
        return false;
    }
#elif defined(Q_OS_MACOS)
    const QString app = info.absoluteFilePath();
    if (app.startsWith(QStringLiteral("/Volumes/"))) {
        if (error)
            *error = QStringLiteral(
                "Copy Omatrack.app out of the disk image first.");
        return false;
    }
    const QFileInfo parent(info.absolutePath());
    if (!parent.isDir() || !parent.isWritable()) {
        if (error)
            *error = QStringLiteral(
                "Cannot write this Omatrack.app. Copy it to a writable "
                "folder.");
        return false;
    }
#else
    const QFileInfo directory(info.absolutePath());
    if (!directory.isDir() || !directory.isWritable()) {
        if (error)
            *error = QStringLiteral(
                "Cannot write this AppImage. Move it to a writable folder.");
        return false;
    }
#endif
    return true;
}

#ifdef Q_OS_MACOS
bool runMacTool(const QString& program, const QStringList& arguments,
                QString* error, int timeoutMs = 180000) {
    QProcess process;
    process.start(program, arguments);
    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        if (error) *error = program + QStringLiteral(" timed out");
        return false;
    }
    if (process.exitStatus() != QProcess::NormalExit ||
        process.exitCode() != 0) {
        const QString detail =
            QString::fromUtf8(process.readAllStandardError()).trimmed();
        if (error)
            *error =
                detail.isEmpty() ? program + QStringLiteral(" failed") : detail;
        return false;
    }
    return true;
}

bool extractMacDmg(const QString& dmg, const QString& staging, QString* error) {
    QDir().mkpath(staging);
    const QString mount = QDir(staging).filePath(QStringLiteral("mnt"));
    QDir().mkpath(mount);
    if (!runMacTool(
            QStringLiteral("/usr/bin/hdiutil"),
            {QStringLiteral("attach"), dmg, QStringLiteral("-nobrowse"),
             QStringLiteral("-readonly"), QStringLiteral("-mountpoint"), mount},
            error))
        return false;
    const QString payload = omatrack::macPayloadRoot(mount);
    const QString copied =
        QDir(staging).filePath(QStringLiteral("Omatrack.app"));
    bool copiedOk = false;
    if (!payload.isEmpty())
        copiedOk = runMacTool(QStringLiteral("/usr/bin/ditto"),
                              {payload, copied}, error);
    else if (error)
        *error = QStringLiteral("Disk image is missing Omatrack.app");
    QString detachError;
    runMacTool(QStringLiteral("/usr/bin/hdiutil"),
               {QStringLiteral("detach"), mount, QStringLiteral("-force")},
               &detachError);
    QDir(mount).rmdir(mount);
    if (!copiedOk) {
        if (error && error->isEmpty())
            *error = QStringLiteral(
                "Unable to copy Omatrack.app from the disk image");
        return false;
    }
    return true;
}
#endif

// ── per-platform installers ─────────────────────────────────────────
// The shared download + checksum orchestration stays in AppUpdater::install;
// each platform's apply() is the on-disk swap that the ~160-line lambda used
// to inline.

/// Linux: replace the running AppImage in place. The running image stays
/// mounted; only the on-disk file changes.
class LinuxAppImageInstaller : public omatrack::UpdateInstaller {
public:
    QString partPath(const QString& installPath,
                     const QString& /*version*/) const override {
        return installPath + QStringLiteral(".part");
    }
    omatrack::UpdateInstallOutcome apply(
        const omatrack::UpdateInstallArgs& args) override {
        omatrack::UpdateInstallOutcome outcome;
        if (!omatrack::replaceAppImage(args.installPath, args.partPath,
                                       &outcome.error))
            QFile::remove(args.partPath);
        else
            outcome.ok = true;
        return outcome;
    }
};

/// Windows: Velopack nupkg, the Setup.exe a zip install migrates with, and
/// the portable zip itself. One class, branching on the asset name the
/// release selected.
class WindowsVelopackInstaller : public omatrack::UpdateInstaller {
public:
    explicit WindowsVelopackInstaller(QString assetName)
        : assetName_(std::move(assetName)) {}

    QString partPath(const QString& /*installPath*/,
                     const QString& version) const override {
        const QString tempRoot =
            QStandardPaths::writableLocation(QStandardPaths::TempLocation);
        if (assetName_.endsWith(QStringLiteral(".nupkg"), Qt::CaseInsensitive))
            return QDir(tempRoot).filePath(
                QStringLiteral("omatrack-update-%1.nupkg").arg(version));
        if (assetName_.endsWith(QStringLiteral("Setup.exe"),
                                Qt::CaseInsensitive))
            return QDir(tempRoot).filePath(
                QStringLiteral("omatrack-update-%1-setup.exe").arg(version));
        return QDir(tempRoot).filePath(
            QStringLiteral("omatrack-update-%1.zip").arg(version));
    }

    omatrack::UpdateInstallOutcome apply(
        const omatrack::UpdateInstallArgs& args) override {
        omatrack::UpdateInstallOutcome outcome;
#ifdef Q_OS_WIN
        if (assetName_.endsWith(QStringLiteral(".nupkg"),
                                Qt::CaseInsensitive)) {
            if (args.updateExe.isEmpty()) {
                QFile::remove(args.partPath);
                outcome.error = QStringLiteral(
                    "This copy was not installed with the Omatrack setup. "
                    "Download the installer from GitHub Releases.");
                return outcome;
            }
            outcome.relaunchWithHelper = true;
            outcome.helperPath = args.updateExe;
            outcome.helperArgs = {QStringLiteral("apply"),
                                  QStringLiteral("--waitPid"),
                                  QString::number(args.pid),
                                  QStringLiteral("--package"),
                                  args.partPath,
                                  QStringLiteral("--")};
            outcome.helperArgs.append(args.relaunchArgs);
            outcome.ok = true;
            return outcome;
        }
        if (assetName_.endsWith(QStringLiteral("Setup.exe"),
                                Qt::CaseInsensitive)) {
            outcome.relaunchWithHelper = true;
            outcome.helperPath = args.partPath;
            outcome.ok = true;
            return outcome;
        }
        // Portable zip: extract, then a cmd helper waits for this process to
        // exit, robocopies the payload over the install, and relaunches.
        const QString staging =
            QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                .filePath(
                    QStringLiteral("omatrack-update-%1").arg(args.version));
        QDir(staging).removeRecursively();
        if (!extractWindowsZip(args.partPath, staging, &outcome.error)) {
            QFile::remove(args.partPath);
            QDir(staging).removeRecursively();
            return outcome;
        }
        const QString payload = omatrack::windowsPayloadRoot(staging);
        if (payload.isEmpty()) {
            QFile::remove(args.partPath);
            QDir(staging).removeRecursively();
            outcome.error =
                QStringLiteral("Update archive is missing omatrack.exe");
            return outcome;
        }
        const QString scriptPath =
            QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                .filePath(QStringLiteral("omatrack-apply-update.cmd"));
        QFile script(scriptPath);
        if (!script.open(QIODevice::WriteOnly | QIODevice::Truncate |
                         QIODevice::Text)) {
            QFile::remove(args.partPath);
            QDir(staging).removeRecursively();
            outcome.error = QStringLiteral("Unable to write the apply script");
            return outcome;
        }
        script.write(omatrack::windowsApplyScript(args.pid, payload,
                                                  args.installPath,
                                                  args.relaunchArgs, staging)
                         .toUtf8());
        script.close();
        QFile::remove(args.partPath);
        outcome.relaunchWithHelper = true;
        outcome.helperPath = scriptPath;
        outcome.ok = true;
        return outcome;
#else
        QFile::remove(args.partPath);
        outcome.error = QStringLiteral("Windows updates require Windows");
        return outcome;
#endif
    }

private:
    QString assetName_;
};

/// macOS: mount the dmg, copy Omatrack.app out, then a bash helper waits for
/// this process to exit, swaps the bundle via ditto, and relaunches.
class MacDmgInstaller : public omatrack::UpdateInstaller {
public:
    QString partPath(const QString& /*installPath*/,
                     const QString& version) const override {
        return QDir(QStandardPaths::writableLocation(
                        QStandardPaths::TempLocation))
            .filePath(QStringLiteral("omatrack-update-%1.dmg").arg(version));
    }
    omatrack::UpdateInstallOutcome apply(
        const omatrack::UpdateInstallArgs& args) override {
        omatrack::UpdateInstallOutcome outcome;
#ifdef Q_OS_MACOS
        const QString staging =
            QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                .filePath(
                    QStringLiteral("omatrack-update-%1").arg(args.version));
        QDir(staging).removeRecursively();
        if (!extractMacDmg(args.partPath, staging, &outcome.error)) {
            QFile::remove(args.partPath);
            QDir(staging).removeRecursively();
            return outcome;
        }
        const QString payload = omatrack::macPayloadRoot(staging);
        if (payload.isEmpty()) {
            QFile::remove(args.partPath);
            QDir(staging).removeRecursively();
            outcome.error =
                QStringLiteral("Update disk image is missing Omatrack.app");
            return outcome;
        }
        const QString scriptPath =
            QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                .filePath(QStringLiteral("omatrack-apply-update.sh"));
        QFile script(scriptPath);
        if (!script.open(QIODevice::WriteOnly | QIODevice::Truncate |
                         QIODevice::Text)) {
            QFile::remove(args.partPath);
            QDir(staging).removeRecursively();
            outcome.error = QStringLiteral("Unable to write the apply script");
            return outcome;
        }
        script.write(omatrack::macApplyScript(args.pid, payload,
                                              args.installPath,
                                              args.relaunchArgs, staging)
                         .toUtf8());
        script.close();
        QFile::setPermissions(
            scriptPath, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner |
                            QFile::ReadGroup | QFile::ExeGroup |
                            QFile::ReadOther | QFile::ExeOther);
        QFile::remove(args.partPath);
        outcome.relaunchWithHelper = true;
        outcome.helperPath = scriptPath;
        outcome.ok = true;
        return outcome;
#else
        QFile::remove(args.partPath);
        outcome.error = QStringLiteral("macOS updates require macOS");
        return outcome;
#endif
    }
};

std::unique_ptr<omatrack::UpdateInstaller> installerForAsset(
    const QString& assetName) {
    if (assetName.endsWith(QStringLiteral(".dmg"), Qt::CaseInsensitive))
        return std::make_unique<MacDmgInstaller>();
    if (assetName.endsWith(QStringLiteral(".nupkg"), Qt::CaseInsensitive) ||
        assetName.endsWith(QStringLiteral("Setup.exe"), Qt::CaseInsensitive) ||
        assetName.endsWith(QStringLiteral(".zip"), Qt::CaseInsensitive))
        return std::make_unique<WindowsVelopackInstaller>(assetName);
    return std::make_unique<LinuxAppImageInstaller>();
}

}  // namespace

AppUpdater::AppUpdater(QObject* parent)
    : QObject(parent), checkJob_(this), installJob_(this) {
    currentVersion_ = QCoreApplication::applicationVersion();
    installPath_ = currentInstallPath();
#ifdef Q_OS_WIN
    updateExePath_ =
        omatrack::velopackUpdateExe(QCoreApplication::applicationDirPath());
    if (!envFlag("OMATRACK_AUTOTEST") && !installPath_.isEmpty()) {
        const bool prompted = omatrack::YamlConfig::instance()
                                  .value({QStringLiteral("associations"),
                                          QStringLiteral("prompted")},
                                         false)
                                  .toBool();
        associationPrompt_ = omatrack::velopackFirstRun() || !prompted;
    }
#endif
    supported_ = !installPath_.isEmpty();
    loadState();
    pollTimer_ = new QTimer(this);
    pollTimer_->setInterval(kPollIntervalMs);
    connect(pollTimer_, &QTimer::timeout, this,
            [this]() { startCheck(false); });
    if (supported_ && enabled_ && !envFlag("OMATRACK_AUTOTEST")) {
        pollTimer_->start();
        scheduleCheck(kStartupCheckDelayMs);
    }
}

AppUpdater::~AppUpdater() {
    // checkJob_/installJob_ cancel their workers and wait for them as they
    // are destroyed here, so the QPointer-guarded progress posts below can
    // never touch this after free. Pending queued events are discarded by
    // ~QObject, but the worker threads themselves stop first.
}

bool AppUpdater::bannerVisible() const {
    return supported_ && enabled_ && available_ && (busy() || !snoozed());
}

bool AppUpdater::busy() const { return phase_ != Phase::Idle; }

void AppUpdater::setEnabled(bool enabled) {
    if (enabled_ == enabled) return;
    enabled_ = enabled;
    saveState();
    emit enabledChanged();
    if (!enabled_) {
        cancel();
        setPhase(Phase::Idle);
    } else if (supported_ && !envFlag("OMATRACK_AUTOTEST")) {
        if (!pollTimer_->isActive()) pollTimer_->start();
        startCheck(true);
    }
    emitVisibility();
}

void AppUpdater::checkNow() {
    if (!supported_) return;
    startCheck(true);
}

void AppUpdater::snooze() {
    if (!available_) return;
    snoozeUntil_ = QDateTime::currentDateTimeUtc()
                       .addDays(kSnoozeDays)
                       .toString(Qt::ISODate);
    snoozeVersion_ = latest_.version;
    saveState();
    emitVisibility();
}

void AppUpdater::cancel() {
    if (const omatrack::IoCancel cancel = checkJob_.cancel())
        cancel->store(true);
    if (const omatrack::IoCancel cancel = installJob_.cancel())
        cancel->store(true);
}

void AppUpdater::loadState() {
    auto& config = omatrack::YamlConfig::instance();
    enabled_ =
        config.value({QStringLiteral("updates"), QStringLiteral("check")}, true)
            .toBool();
    lastCheck_ =
        config.value({QStringLiteral("updates"), QStringLiteral("last_check")})
            .toString();
    snoozeUntil_ =
        config
            .value({QStringLiteral("updates"), QStringLiteral("snooze_until")})
            .toString();
    snoozeVersion_ = config
                         .value({QStringLiteral("updates"),
                                 QStringLiteral("snooze_version")})
                         .toString();
    const QString cachedVersion =
        config.value({QStringLiteral("updates"), QStringLiteral("latest")})
            .toString();
    const QString assetUrl =
        config.value({QStringLiteral("updates"), QStringLiteral("asset_url")})
            .toString();
    if (!supported_ || cachedVersion.isEmpty() ||
        !omatrack::versionIsNewer(cachedVersion, currentVersion_)) {
        available_ = false;
        return;
    }
    latest_.version = cachedVersion;
    latest_.assetName =
        config.value({QStringLiteral("updates"), QStringLiteral("asset_name")})
            .toString();
    latest_.assetUrl = QUrl(assetUrl);
    latest_.assetSize =
        config
            .value({QStringLiteral("updates"), QStringLiteral("asset_size")},
                   -1)
            .toLongLong();
    sha256_ =
        config.value({QStringLiteral("updates"), QStringLiteral("sha256")})
            .toString();
    available_ = latest_.assetUrl.isValid();
}

void AppUpdater::saveState() const {
    auto& config = omatrack::YamlConfig::instance();
    config.setValue({QStringLiteral("updates"), QStringLiteral("check")},
                    enabled_);
    if (lastCheck_.isEmpty())
        config.setValue(
            {QStringLiteral("updates"), QStringLiteral("last_check")},
            QVariant());
    else
        config.setValue(
            {QStringLiteral("updates"), QStringLiteral("last_check")},
            lastCheck_);
    if (snoozeUntil_.isEmpty()) {
        config.setValue(
            {QStringLiteral("updates"), QStringLiteral("snooze_until")},
            QVariant());
        config.setValue(
            {QStringLiteral("updates"), QStringLiteral("snooze_version")},
            QVariant());
    } else {
        config.setValue(
            {QStringLiteral("updates"), QStringLiteral("snooze_until")},
            snoozeUntil_);
        config.setValue(
            {QStringLiteral("updates"), QStringLiteral("snooze_version")},
            snoozeVersion_);
    }
    if (!available_) {
        config.setValue({QStringLiteral("updates"), QStringLiteral("latest")},
                        QVariant());
        config.setValue(
            {QStringLiteral("updates"), QStringLiteral("asset_name")},
            QVariant());
        config.setValue(
            {QStringLiteral("updates"), QStringLiteral("asset_url")},
            QVariant());
        config.setValue(
            {QStringLiteral("updates"), QStringLiteral("asset_size")},
            QVariant());
        config.setValue({QStringLiteral("updates"), QStringLiteral("sha256")},
                        QVariant());
    } else {
        config.setValue({QStringLiteral("updates"), QStringLiteral("latest")},
                        latest_.version);
        config.setValue(
            {QStringLiteral("updates"), QStringLiteral("asset_name")},
            latest_.assetName);
        config.setValue(
            {QStringLiteral("updates"), QStringLiteral("asset_url")},
            latest_.assetUrl.toString());
        config.setValue(
            {QStringLiteral("updates"), QStringLiteral("asset_size")},
            latest_.assetSize);
        config.setValue({QStringLiteral("updates"), QStringLiteral("sha256")},
                        sha256_);
    }
    config.save();
}

void AppUpdater::scheduleCheck(int delayMs) {
    QTimer::singleShot(delayMs, this, [this]() { startCheck(false); });
}

void AppUpdater::startCheck(bool force) {
    if (!supported_ || !enabled_ || busy()) return;
    if (envFlag("OMATRACK_AUTOTEST") &&
        qEnvironmentVariable("OMATRACK_UPDATE_API").isEmpty())
        return;
    if (!force) {
        const QDateTime previous = parseIso(lastCheck_);
        if (previous.isValid() &&
            previous.secsTo(QDateTime::currentDateTimeUtc()) <
                std::chrono::seconds(kCheckInterval).count())
            return;
    }

    setPhase(Phase::Checking, QStringLiteral("Checking for updates…"));
    const QString version = currentVersion_;
    const QUrl api = releaseApiUrl();
    const bool velopack = !updateExePath_.isEmpty();
    checkJob_.start(
        [api, version, velopack](const omatrack::IoCancel& cancel) {
            CheckResult result;
            const omatrack::HttpResponse response = omatrack::sendFollowing(
                api, "GET",
                [version](const QUrl& hop) {
                    return githubRequest(hop, version);
                },
                {}, cancel);
            if (omatrack::ioCancelled(cancel)) {
                result.error = QStringLiteral("Cancelled");
                return result;
            }
            if (response.status != 200) {
                result.error = response.error.isEmpty()
                                   ? QStringLiteral("Update check failed")
                                   : response.error;
                qCInfo(lcIo).noquote() << "update check failed status"
                                       << response.status << response.error;
                return result;
            }
            const auto parsed = omatrack::parseGithubRelease(response.body);
            if (!parsed) {
                result.error = QStringLiteral("Update metadata invalid");
                return result;
            }
            result.release = *parsed;
#ifdef Q_OS_WIN
            const bool selected =
                omatrack::selectWindowsUpdateAsset(&result.release, velopack);
#else
            const bool selected = omatrack::selectReleaseAsset(
                &result.release, omatrack::currentUpdateChannel());
#endif
            if (!selected) {
                result.error = QStringLiteral("No download for this platform");
                return result;
            }
            result.ok = true;
            result.newer =
                omatrack::versionIsNewer(result.release.version, version);
            if (!result.newer || !result.release.checksumsUrl.isValid())
                return result;
            const omatrack::HttpResponse sums = omatrack::sendFollowing(
                result.release.checksumsUrl, "GET",
                [version](const QUrl& hop) {
                    return githubRequest(hop, version);
                },
                {}, cancel);
            if (sums.status == 200)
                result.sha256 = omatrack::checksumForFile(
                    sums.body, result.release.assetName);
            return result;
        },
        [this](CheckResult result) {
            lastCheck_ = isoNow();
            if (!result.ok) {
                const bool cancelled =
                    result.error == QStringLiteral("Cancelled");
                setPhase(Phase::Idle, {}, cancelled ? QString() : result.error);
                saveState();
                return;
            }
            if (!result.newer) {
                clearAvailable();
                setPhase(Phase::Idle);
                saveState();
                return;
            }
            adoptRelease(result.release, result.sha256);
            setPhase(Phase::Idle);
            saveState();
        });
}

void AppUpdater::install() {
    if (!supported_ || !available_ || busy()) return;
    QString writableError;
    if (!writableLocation(installPath_, &writableError)) {
        setPhase(Phase::Idle, {}, writableError);
        return;
    }
    if (!latest_.assetUrl.isValid()) {
        setPhase(Phase::Idle, {}, QStringLiteral("Update download is missing"));
        return;
    }

    setProgress(0);
    setPhase(Phase::Downloading, QStringLiteral("Downloading update…"));
    const QString version = currentVersion_;
    const QString installPath = installPath_;
    const omatrack::GithubRelease release = latest_;
    const qint64 pid = QCoreApplication::applicationPid();
    const QStringList relaunchArgs = QCoreApplication::arguments().mid(1);
    QString expectedSha = sha256_;
    const QString updateExe = updateExePath_;
    // Shared (not unique) so the worker lambda below stays copy-constructible
    // for AsyncJob::start, which takes the work as a std::function.
    const std::shared_ptr<omatrack::UpdateInstaller> installer =
        installerForAsset(release.assetName);
    const QString partPath = installer->partPath(installPath, release.version);
    QFile::remove(partPath);
    // A weak guard so a progress callback queued from the worker thread can
    // no-op once this object is gone. The destructor cancels and waits too,
    // so the worker stops before `this` is destroyed; the guard covers any
    // event queued in the narrow window before ~QObject drains it.
    QPointer<AppUpdater> guard(this);
    installJob_.start(
        [guard, version, installPath, release, expectedSha, pid, relaunchArgs,
         updateExe, partPath, installer](const omatrack::IoCancel& cancel) {
            omatrack::UpdateInstallOutcome result;
            QString sha = expectedSha;
            if (sha.isEmpty() && release.checksumsUrl.isValid()) {
                const omatrack::HttpResponse sums = omatrack::sendFollowing(
                    release.checksumsUrl, "GET",
                    [version](const QUrl& hop) {
                        return githubRequest(hop, version);
                    },
                    {}, cancel);
                if (sums.status == 200)
                    sha =
                        omatrack::checksumForFile(sums.body, release.assetName);
            }
            if (sha.isEmpty()) {
                result.error =
                    QStringLiteral("Release is missing a SHA-256 checksum");
                return result;
            }
            const omatrack::FileDownload download = omatrack::downloadFile(
                release.assetUrl,
                [version](const QUrl& hop) {
                    return githubRequest(hop, version);
                },
                partPath,
                [guard, cancel](qint64 received, qint64 total) {
                    if (omatrack::ioCancelled(cancel)) return false;
                    const double value =
                        total > 0 ? double(received) / double(total) : 0.0;
                    QMetaObject::invokeMethod(
                        guard.data(),  // null-safe context; the functor
                                       // re-checks
                        [guard, received, total, value]() {
                            if (!guard) return;
                            guard->downloadBytes_ = received;
                            guard->downloadTotal_ = total;
                            guard->setProgress(value);
                            if (total > 0)
                                guard->setPhase(
                                    AppUpdater::Phase::Downloading,
                                    QStringLiteral("Downloading %1 of %2…")
                                        .arg(omatrack::formatBytes(received),
                                             omatrack::formatBytes(total)));
                        },
                        Qt::QueuedConnection);
                    return true;
                },
                cancel);
            if (omatrack::ioCancelled(cancel)) {
                QFile::remove(partPath);
                result.error = QStringLiteral("Cancelled");
                return result;
            }
            if (download.status != 200 || !download.error.isEmpty()) {
                QFile::remove(partPath);
                result.error = download.error.isEmpty()
                                   ? QStringLiteral("Update download failed")
                                   : download.error;
                return result;
            }
            const QString actual = omatrack::fileSha256(partPath);
            if (actual.compare(sha, Qt::CaseInsensitive) != 0) {
                QFile::remove(partPath);
                result.error =
                    QStringLiteral("Downloaded package failed checksum");
                qCInfo(lcIo).noquote() << "update checksum mismatch expected"
                                       << sha << "got" << actual;
                return result;
            }
            QMetaObject::invokeMethod(
                guard.data(),
                [guard]() {
                    if (!guard) return;
                    guard->setPhase(AppUpdater::Phase::Installing,
                                    QStringLiteral("Installing…"));
                    guard->setProgress(1.0);
                },
                Qt::QueuedConnection);
            return installer->apply({partPath, installPath, release.version,
                                     pid, relaunchArgs, updateExe});
        },
        [this](omatrack::UpdateInstallOutcome result) {
            if (!result.ok) {
                setProgress(0);
                const bool cancelled =
                    result.error == QStringLiteral("Cancelled");
                setPhase(Phase::Idle, {}, cancelled ? QString() : result.error);
                return;
            }
            setPhase(Phase::Idle, QStringLiteral("Restarting…"));
            bool launched = false;
            if (result.relaunchWithHelper) {
#ifdef Q_OS_WIN
                if (result.helperPath.endsWith(QStringLiteral(".cmd"),
                                               Qt::CaseInsensitive))
                    launched = QProcess::startDetached(
                        QStringLiteral("cmd.exe"),
                        {QStringLiteral("/d"), QStringLiteral("/c"),
                         result.helperPath});
                else
                    launched = QProcess::startDetached(result.helperPath,
                                                       result.helperArgs);
#elif defined(Q_OS_MACOS)
                launched = QProcess::startDetached(QStringLiteral("/bin/bash"),
                                                   {result.helperPath});
#else
                launched = false;
#endif
            } else {
                launched = QProcess::startDetached(
                    installPath_, QCoreApplication::arguments().mid(1),
                    QDir::currentPath());
            }
            if (!launched) {
                setPhase(Phase::Idle, {},
                         QStringLiteral("Updated, but could not restart. "
                                        "Relaunch Omatrack."));
                return;
            }
            QCoreApplication::quit();
        });
}

void AppUpdater::adoptRelease(const omatrack::GithubRelease& release,
                              const QString& sha256) {
    const bool wasAvailable = available_;
    const QString previousVersion = latest_.version;
    latest_ = release;
    sha256_ = sha256;
    available_ = true;
    if (snoozeVersion_ != latest_.version) {
        snoozeUntil_.clear();
        snoozeVersion_.clear();
    }
    if (!wasAvailable || previousVersion != latest_.version)
        emit availableChanged();
    emitVisibility();
}

void AppUpdater::clearAvailable() {
    if (!available_ && latest_.version.isEmpty()) return;
    available_ = false;
    latest_ = {};
    sha256_.clear();
    snoozeUntil_.clear();
    snoozeVersion_.clear();
    emit availableChanged();
    emitVisibility();
}

void AppUpdater::setPhase(Phase phase, const QString& status,
                          const QString& error) {
    const bool wasBusy = busy();
    const bool wasBanner = bannerVisible();
    phase_ = phase;
    status_ = status;
    error_ = error;
    emit statusChanged();
    if (wasBusy != busy() || wasBanner != bannerVisible()) emitVisibility();
}

void AppUpdater::setProgress(double progress) {
    if (qFuzzyCompare(progress_, progress) && progress_ == progress) {
        emit progressChanged();
        return;
    }
    progress_ = progress;
    emit progressChanged();
}

QString AppUpdater::downloadLabel() const {
    if (downloadTotal_ > 0)
        return QStringLiteral("%1% · %2 / %3")
            .arg(int(progress_ * 100.0 + 0.5))
            .arg(omatrack::formatBytes(downloadBytes_),
                 omatrack::formatBytes(downloadTotal_));
    if (downloadBytes_ > 0) return omatrack::formatBytes(downloadBytes_);
    return {};
}

void AppUpdater::emitVisibility() { emit bannerVisibleChanged(); }

bool AppUpdater::snoozed() const {
    if (snoozeUntil_.isEmpty() || snoozeVersion_ != latest_.version)
        return false;
    const QDateTime until = parseIso(snoozeUntil_);
    return until.isValid() && until > QDateTime::currentDateTimeUtc();
}

int AppUpdater::associationCount() const {
#ifdef Q_OS_WIN
    return omatrack::fileAssociations().size();
#else
    return 0;
#endif
}

QString AppUpdater::associationExtension(int index) const {
#ifdef Q_OS_WIN
    const auto rows = omatrack::fileAssociations();
    if (index < 0 || index >= rows.size()) return {};
    return rows.at(index).extension;
#else
    Q_UNUSED(index);
    return {};
#endif
}

QString AppUpdater::associationLabel(int index) const {
#ifdef Q_OS_WIN
    const auto rows = omatrack::fileAssociations();
    if (index < 0 || index >= rows.size()) return {};
    return QStringLiteral(".%1 — %2")
        .arg(rows.at(index).extension, rows.at(index).description);
#else
    Q_UNUSED(index);
    return {};
#endif
}

bool AppUpdater::associationEnabled(int index) const {
#ifdef Q_OS_WIN
    const auto rows = omatrack::fileAssociations();
    if (index < 0 || index >= rows.size()) return false;
    return omatrack::associationEnabled(rows.at(index).extension);
#else
    Q_UNUSED(index);
    return false;
#endif
}

bool AppUpdater::associationVideo(int index) const {
#ifdef Q_OS_WIN
    const auto rows = omatrack::fileAssociations();
    if (index < 0 || index >= rows.size()) return false;
    return rows.at(index).video;
#else
    Q_UNUSED(index);
    return false;
#endif
}

void AppUpdater::setAssociationEnabled(int index, bool enabled) {
#ifdef Q_OS_WIN
    const auto rows = omatrack::fileAssociations();
    if (index < 0 || index >= rows.size()) return;
    omatrack::setAssociationEnabled(rows.at(index).extension, enabled);
    emit associationPromptChanged();
#else
    Q_UNUSED(index);
    Q_UNUSED(enabled);
#endif
}

void AppUpdater::finishAssociationPrompt() {
#ifdef Q_OS_WIN
    omatrack::YamlConfig::instance().setValue(
        {QStringLiteral("associations"), QStringLiteral("prompted")}, true);
    omatrack::YamlConfig::instance().save();
#endif
    if (!associationPrompt_) return;
    associationPrompt_ = false;
    emit associationPromptChanged();
}
