// origin: PUBLIC — opt-in HF downloads; no telemetry/media upload capability.
#include "ImageModelManager.h"
#include "GaugeReader.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QNetworkRequest>
#include <QPointer>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <algorithm>
#include <chrono>
#include <stdexcept>

using namespace omatrack::image_model;

struct ImageModelCatalog {
    Manifest manifest;
    QByteArray bytes;
    QString revision;
    qint64 resolvedAt = 0;
};
struct ImageModelWorkResult {
    QString error, message, installedVersion, pendingPath;
    qint64 lastAttempt = 0;
    bool cancelled = false, throttled = false, activate = false;
    std::shared_ptr<const ImageModelCatalog> catalog, pending;
};

namespace {
constexpr qint64 Day = 24 * 60 * 60;
constexpr qint64 ManualCooldown = 30;
constexpr char CatalogFile[] = "catalog.json";
using Cancel = omatrack::IoCancel;
using Progress = omatrack::DownloadProgress;
struct Failure : std::runtime_error {
    explicit Failure(const QString& message)
        : std::runtime_error(message.toStdString()) {}
};
void check(const Cancel& cancel) {
    if (omatrack::ioCancelled(cancel))
        throw Failure(QStringLiteral("Model operation cancelled"));
}
void require(bool condition, const char* message) {
    if (!condition) throw Failure(QString::fromLatin1(message));
}
QString absolute(const QString& path) {
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

omatrack::FileDownload guardedTransfer(const QUrl& url, const QString& path,
                                       qint64 limit, const Progress& progress,
                                       const Cancel& cancel) {
    require(allowedDownloadUrl(url), "Untrusted model download URL");
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(120);
    auto rejected = std::make_shared<std::atomic<bool>>(false);
    auto hops = std::make_shared<std::atomic<int>>(0);
    const omatrack::RequestFactory factory = [cancel, rejected, hops,
                                              deadline](const QUrl& hop) {
        if (omatrack::ioCancelled(cancel) || ++*hops > 6 ||
            !allowedDownloadUrl(hop) ||
            std::chrono::steady_clock::now() > deadline) {
            rejected->store(true);
            // Do not throw across the dedicated I/O thread's Qt callbacks.
            return omatrack::makeRequest(QUrl());
        }
        auto request = omatrack::makeRequest(hop);
        request.setAttribute(QNetworkRequest::CookieLoadControlAttribute,
                             QNetworkRequest::Manual);
        request.setAttribute(QNetworkRequest::CookieSaveControlAttribute,
                             QNetworkRequest::Manual);
        request.setAttribute(QNetworkRequest::AuthenticationReuseAttribute,
                             QNetworkRequest::Manual);
        request.setRawHeader("User-Agent", "Omatrack public model manager");
        request.setRawHeader("Accept-Encoding", "identity");
        request.setTransferTimeout(30000);
        return request;
    };
    const auto bounded = [cancel, progress, limit, deadline](qint64 received,
                                                             qint64 total) {
        return !omatrack::ioCancelled(cancel) && received <= limit &&
               (total < 0 || total <= limit) &&
               std::chrono::steady_clock::now() <= deadline &&
               (!progress || progress(received, total));
    };
    auto result = omatrack::downloadFile(url, factory, path, bounded, cancel);
    if (rejected->load())
        result.error =
            QStringLiteral("Untrusted or excessive model redirect blocked");
    else if (result.bytes > limit)
        result.error = QStringLiteral("Model response exceeds its size bound");
    return result;
}
ImageModelServices resolved(ImageModelServices services) {
    if (services.dataRoot.isEmpty())
        services.dataRoot =
            QDir(QStandardPaths::writableLocation(
                     QStandardPaths::AppLocalDataLocation))
                .filePath(QStringLiteral(
                    "image-models/tobil--omatrack-telemetry-reader"));
    if (services.cacheRoot.isEmpty())
        services.cacheRoot =
            QDir(
                QStandardPaths::writableLocation(QStandardPaths::CacheLocation))
                .filePath(QStringLiteral(
                    "image-models/tobil--omatrack-telemetry-reader"));
    if (services.appVersion.isEmpty())
        services.appVersion = QCoreApplication::applicationVersion();
    services.dataRoot = absolute(services.dataRoot);
    services.cacheRoot = absolute(services.cacheRoot);
    return services;
}
QByteArray readBounded(const QString& path, qint64 maximum) {
    const QFileInfo info(path);
    require(!info.isSymLink() && info.isFile() && info.size() >= 0 &&
                info.size() <= maximum,
            "Invalid managed model metadata file");
    QFile file(path);
    require(file.open(QIODevice::ReadOnly),
            "Cannot read managed model metadata");
    const auto bytes = file.read(maximum + 1);
    require(bytes.size() <= maximum && file.atEnd(),
            "Managed model metadata exceeds size bound");
    return bytes;
}
void writeAtomic(const QString& path, const QByteArray& bytes) {
    require(!QFileInfo(path).isSymLink(),
            "Managed model metadata cannot be a symlink");
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    require(file.open(QIODevice::WriteOnly),
            "Cannot stage managed model metadata");
    require(file.write(bytes) == bytes.size() && file.commit(),
            "Cannot publish managed model metadata");
}
QByteArray hashFile(const QString& path, qint64 size, const Cancel& cancel) {
    const QFileInfo before(path);
    require(!before.isSymLink() && before.isFile() && before.size() == size,
            "Model byte count does not match manifest");
    QFile file(path);
    require(file.open(QIODevice::ReadOnly), "Cannot read downloaded model");
    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 read = 0;
    while (!file.atEnd()) {
        check(cancel);
        const auto block = file.read(128 * 1024);
        require(!block.isEmpty(), "Model read failed");
        read += block.size();
        require(read <= size, "Model grew during verification");
        hash.addData(block);
    }
    const QFileInfo after(path);
    require(read == size && after.size() == size &&
                after.lastModified() == before.lastModified(),
            "Model changed during verification");
    return hash.result().toHex();
}
void verifyArtifact(const ImageModelServices& services, const QString& path,
                    const Manifest& manifest, const Cancel& cancel) {
    require(hashFile(path, manifest.sizeBytes, cancel) == manifest.sha256,
            "Model SHA-256 does not match manifest");
    check(cancel);
    QString error;
    if (!services.verifyModel(path, manifest, &error))
        throw Failure(
            error.isEmpty()
                ? QStringLiteral("Model compatibility validation failed")
                : error);
    check(cancel);
    require(hashFile(path, manifest.sizeBytes, cancel) == manifest.sha256,
            "Model changed during compatibility validation");
}
std::shared_ptr<ImageModelCatalog> parseCatalog(const QByteArray& manifest,
                                                const QString& revision,
                                                qint64 resolvedAt,
                                                const QString& appVersion) {
    require(manifestUrl(revision).isValid(),
            "Invalid immutable model revision");
    QString error;
    auto parsed = parseManifest(manifest, appVersion, &error);
    if (!parsed) throw Failure(error);
    auto result = std::make_shared<ImageModelCatalog>();
    result->manifest = std::move(*parsed);
    result->bytes = manifest;
    result->revision = revision;
    result->resolvedAt = resolvedAt;
    return result;
}
struct CachedCatalog {
    qint64 lastAttempt = 0;
    std::shared_ptr<const ImageModelCatalog> catalog;
};
CachedCatalog readCatalogCache(const ImageModelServices& services) {
    CachedCatalog result;
    const QString path =
        QDir(services.cacheRoot).filePath(QLatin1String(CatalogFile));
    if (!QFileInfo::exists(path)) return result;
    try {
        const auto doc = QJsonDocument::fromJson(
            readBounded(path, MaximumManifestBytes + 4096));
        if (!doc.isObject()) return result;
        const auto object = doc.object();
        bool ok = false;
        const auto attempt = object.value(QStringLiteral("attempted_at"))
                                 .toString()
                                 .toLongLong(&ok);
        if (ok && attempt >= 0) result.lastAttempt = attempt;
        const auto at = object.value(QStringLiteral("resolved_at"))
                            .toString()
                            .toLongLong(&ok);
        if (ok && at > 0 && object.value(QStringLiteral("manifest")).isObject())
            result.catalog = parseCatalog(
                QJsonDocument(
                    object.value(QStringLiteral("manifest")).toObject())
                    .toJson(QJsonDocument::Compact),
                object.value(QStringLiteral("revision")).toString(), at,
                services.appVersion);
    } catch (const Failure&) {
        // A bad upstream cache is never a trusted model or a user preference.
    }
    // Wall clocks can move backwards. Future cache times are stale evidence,
    // not a reason to disable model checks indefinitely.
    const auto now = services.now();
    if (result.lastAttempt > now) result.lastAttempt = 0;
    if (result.catalog && result.catalog->resolvedAt > now)
        result.catalog.reset();
    return result;
}
void writeCatalogCache(const ImageModelServices& services,
                       const CachedCatalog& cached) {
    QJsonObject object{
        {QStringLiteral("attempted_at"), QString::number(cached.lastAttempt)}};
    if (cached.catalog) {
        object.insert(QStringLiteral("resolved_at"),
                      QString::number(cached.catalog->resolvedAt));
        object.insert(QStringLiteral("revision"), cached.catalog->revision);
        object.insert(QStringLiteral("manifest"),
                      QJsonDocument::fromJson(cached.catalog->bytes).object());
    }
    writeAtomic(QDir(services.cacheRoot).filePath(QLatin1String(CatalogFile)),
                QJsonDocument(object).toJson(QJsonDocument::Compact));
}
QByteArray fetchMetadata(const ImageModelServices& services, const QUrl& url,
                         const QString& path, qint64 maximum,
                         const Cancel& cancel) {
    check(cancel);
    const auto result = services.transfer(url, path, maximum, {}, cancel);
    check(cancel);
    require(result.status == 200 && result.error.isEmpty(),
            "Public model metadata download failed");
    return readBounded(path, maximum);
}
QString versionDirectory(const ImageModelServices& services,
                         const ImageModelCatalog& catalog) {
    return QDir(services.dataRoot)
        .filePath(QStringLiteral("versions/%1-%2")
                      .arg(catalog.manifest.version,
                           QString::fromLatin1(catalog.manifest.sha256)));
}
std::shared_ptr<const ImageModelCatalog> installedCatalog(
    const ImageModelServices& services, const QString& active,
    const Cancel& cancel) {
    if (active.isEmpty()) return {};
    const QString prefix =
        QDir(services.dataRoot).filePath(QStringLiteral("versions")) +
        QLatin1Char('/');
    if (!absolute(active).startsWith(prefix))
        return {};  // Local model remains a separate user choice.
    const QFileInfo file(active);
    require(!file.isSymLink() && !QFileInfo(file.absolutePath()).isSymLink() &&
                file.isFile(),
            "Selected managed model is missing or unsafe");
    const QString canonicalRoot =
        QFileInfo(services.dataRoot).canonicalFilePath();
    require(
        !canonicalRoot.isEmpty() &&
            file.fileName() == QLatin1String(ModelFilename) &&
            file.canonicalFilePath().startsWith(canonicalRoot +
                                                QStringLiteral("/versions/")) &&
            !QFileInfo(
                 QDir(services.dataRoot).filePath(QStringLiteral("versions")))
                 .isSymLink(),
        "Managed model escaped its data directory");
    const auto manifest = readBounded(
        QDir(file.absolutePath()).filePath(QStringLiteral("manifest.json")),
        MaximumManifestBytes);
    const auto revision =
        QString::fromLatin1(
            readBounded(QDir(file.absolutePath())
                            .filePath(QStringLiteral("revision.txt")),
                        64))
            .trimmed();
    auto catalog = parseCatalog(manifest, revision, 0, services.appVersion);
    verifyArtifact(services, active, catalog->manifest, cancel);
    return catalog;
}
void requireNotDowngrade(
    const std::shared_ptr<const ImageModelCatalog>& installed,
    const ImageModelCatalog& candidate) {
    if (!installed) return;
    const auto current = *versionParts(installed->manifest.version),
               next = *versionParts(candidate.manifest.version);
    require(next >= current, "Refusing a managed model downgrade");
    require(next != current ||
                installed->manifest.sha256 == candidate.manifest.sha256,
            "Model publisher changed artifact bytes without a version change");
}
QString stage(const ImageModelServices& services,
              const ImageModelCatalog& catalog, const Progress& progress,
              const Cancel& cancel) {
    check(cancel);
    const QString versions =
        QDir(services.dataRoot).filePath(QStringLiteral("versions"));
    require(!QFileInfo(versions).isSymLink() && QDir().mkpath(versions),
            "Cannot create safe managed model directory");
    const QString destination = versionDirectory(services, catalog);
    const QString finalPath =
        QDir(destination).filePath(QLatin1String(ModelFilename));
    if (QFileInfo::exists(destination)) {
        const auto existing = installedCatalog(services, finalPath, cancel);
        require(existing &&
                    existing->manifest.version == catalog.manifest.version &&
                    existing->manifest.sha256 == catalog.manifest.sha256 &&
                    existing->manifest.metadata == catalog.manifest.metadata,
                "Existing managed version receipt disagrees with the public "
                "manifest");
        return finalPath;
    }
    QTemporaryDir temporary(
        QDir(services.dataRoot).filePath(QStringLiteral(".stage-XXXXXX")));
    require(temporary.isValid(),
            "Cannot create model download staging directory");
    const QString model =
        QDir(temporary.path()).filePath(QLatin1String(ModelFilename));
    const auto response =
        services.transfer(modelUrl(catalog.revision), model,
                          catalog.manifest.sizeBytes, progress, cancel);
    check(cancel);
    require(response.status == 200 && response.error.isEmpty() &&
                response.bytes == catalog.manifest.sizeBytes,
            "Public model download failed or has the wrong byte count");
    verifyArtifact(services, model, catalog.manifest, cancel);
    writeAtomic(
        QDir(temporary.path()).filePath(QStringLiteral("manifest.json")),
        catalog.bytes);
    writeAtomic(QDir(temporary.path()).filePath(QStringLiteral("revision.txt")),
                catalog.revision.toLatin1());
    check(cancel);
    // The selected path is never overwritten. One directory rename publishes
    // only the fully verified model plus its immutable provenance receipt.
    require(QDir().rename(temporary.path(), destination),
            "Cannot atomically publish verified model version");
    temporary.setAutoRemove(false);
    return finalPath;
}
}  // namespace

std::shared_ptr<ImageModelWorkResult> ImageModelManager::work(
    Action action, ImageModelServices services, const QString& activePath,
    std::shared_ptr<const ImageModelCatalog> pending,
    const QString& pendingPath, const Cancel& cancel,
    const Progress& progress) {
    auto result = std::make_shared<ImageModelWorkResult>();
    try {
        check(cancel);
        services = resolved(std::move(services));
        require(QDir().mkpath(services.cacheRoot),
                "Cannot create model catalog cache directory");
        auto cached = readCatalogCache(services);
        result->lastAttempt = cached.lastAttempt;
        result->catalog = cached.catalog;
        const auto installed = installedCatalog(services, activePath, cancel);
        if (installed) result->installedVersion = installed->manifest.version;
        if (action == Action::Activate) {
            require(bool(pending) && !pendingPath.isEmpty(),
                    "No verified model is pending");
            requireNotDowngrade(installed, *pending);
            const auto verified =
                installedCatalog(services, pendingPath, cancel);
            require(
                verified &&
                    verified->manifest.version == pending->manifest.version &&
                    verified->manifest.sha256 == pending->manifest.sha256 &&
                    verified->manifest.metadata == pending->manifest.metadata,
                "Pending managed model receipt changed before activation");
            result->pending = pending;
            result->pendingPath = pendingPath;
            result->activate = true;
            return result;
        }
        const auto now = services.now();
        if (action != Action::Inspect) {
            const bool fresh = cached.catalog &&
                               now >= cached.catalog->resolvedAt &&
                               now - cached.catalog->resolvedAt < Day;
            const bool needCheck = action != Action::Download || !fresh;
            if (needCheck) {
                QLockFile lock(QDir(services.cacheRoot)
                                   .filePath(QStringLiteral("catalog.lock")));
                lock.setStaleLockTime(0);
                require(lock.tryLock(0),
                        "Another public model catalog check is running");
                cached = readCatalogCache(services);
                result->lastAttempt = cached.lastAttempt;
                const qint64 cooldown =
                    action == Action::AutoCheck ? Day : ManualCooldown;
                if (cached.lastAttempt > 0 &&
                    now - cached.lastAttempt < cooldown) {
                    result->catalog = cached.catalog;
                    result->throttled = true;
                    result->message =
                        action == Action::AutoCheck
                            ? QStringLiteral(
                                  "Public model catalog checked recently")
                            : QStringLiteral(
                                  "Please wait 30 seconds between catalog "
                                  "checks");
                    return result;
                }
                cached.lastAttempt = now;
                result->lastAttempt = now;
                writeCatalogCache(
                    services,
                    cached);  // Attempt metadata, not user configuration.
                QTemporaryDir temporary(
                    QDir(services.cacheRoot)
                        .filePath(QStringLiteral(".catalog-XXXXXX")));
                require(temporary.isValid(),
                        "Cannot stage public model catalog");
                QString error;
                const auto revision = parseRevision(
                    fetchMetadata(
                        services, revisionUrl(),
                        QDir(temporary.path())
                            .filePath(QStringLiteral("repository.json")),
                        MaximumCatalogBytes, cancel),
                    &error);
                if (!revision) throw Failure(error);
                const auto manifest = fetchMetadata(
                    services, manifestUrl(*revision),
                    QDir(temporary.path())
                        .filePath(QStringLiteral("manifest.json")),
                    MaximumManifestBytes, cancel);
                cached.catalog =
                    parseCatalog(manifest, *revision, now, services.appVersion);
                check(cancel);
                writeCatalogCache(services, cached);
                result->catalog = cached.catalog;
            }
        }
        if (!result->catalog) return result;
        requireNotDowngrade(installed, *result->catalog);
        const bool current =
            installed &&
            installed->manifest.version == result->catalog->manifest.version &&
            installed->manifest.sha256 == result->catalog->manifest.sha256;
        if (!current && action == Action::Download) {
            result->pending = result->catalog;
            result->pendingPath =
                stage(services, *result->catalog, progress, cancel);
        } else if (!current && action == Action::Inspect) {
            const QString existing =
                QDir(versionDirectory(services, *result->catalog))
                    .filePath(QLatin1String(ModelFilename));
            if (QFileInfo::exists(existing)) {
                const auto verified =
                    installedCatalog(services, existing, cancel);
                require(verified &&
                            verified->manifest.version ==
                                result->catalog->manifest.version &&
                            verified->manifest.sha256 ==
                                result->catalog->manifest.sha256 &&
                            verified->manifest.metadata ==
                                result->catalog->manifest.metadata,
                        "Cached managed model receipt does not match catalog");
                result->pending = result->catalog;
                result->pendingPath = existing;
            }
        }
        check(cancel);
    } catch (const std::exception& failure) {
        result->cancelled = omatrack::ioCancelled(cancel);
        result->error = result->cancelled
                            ? QStringLiteral("Model operation cancelled")
                            : QString::fromUtf8(failure.what());
    }
    return result;
}

ImageModelManager::ImageModelManager(QObject* parent)
    : ImageModelManager(ImageModelServices{}, parent) {}
ImageModelManager::ImageModelManager(ImageModelServices services,
                                     QObject* parent)
    : QObject(parent), services_(std::move(services)), job_(this) {
    if (!services_.now)
        services_.now = [] { return QDateTime::currentSecsSinceEpoch(); };
    if (!services_.transfer) services_.transfer = guardedTransfer;
    const auto transport = services_.transfer;
    const auto requests = networkRequests_;
    services_.transfer = [transport, requests](
                             const QUrl& url, const QString& path,
                             qint64 maximum, const Progress& progress,
                             const Cancel& cancel) {
        check(cancel);
        require(allowedDownloadUrl(url), "Untrusted public model endpoint");
        ++*requests;
        return transport(url, path, maximum, progress, cancel);
    };
    if (!services_.verifyModel)
        services_.verifyModel = [](const QString& path,
                                   const Manifest& manifest, QString* error) {
            omatrack::inference::GaugeReader reader(path.toStdString());
            if (!reader.ready()) {
                if (error)
                    *error = QStringLiteral(
                        "Model failed reader metadata/tensor compatibility "
                        "validation");
                return false;
            }
            QMap<QString, QString> actual;
            for (const auto& [key, value] : reader.modelMetadata())
                actual.insert(QString::fromStdString(key),
                              QString::fromStdString(value));
            if (actual != manifest.metadata) {
                if (error)
                    *error = QStringLiteral(
                        "Model metadata does not match its public manifest");
                return false;
            }
            return true;
        };
    timer_.setInterval(60 * 60 * 1000);
    connect(&timer_, &QTimer::timeout, this,
            &ImageModelManager::automaticCheck);
    connect(&job_, &AsyncJobBase::runningChanged, this,
            &ImageModelManager::stateChanged);
    timer_.start();  // A disabled manager returns without I/O/network on every
                     // tick.
}
ImageModelManager::~ImageModelManager() {
    timer_.stop();
    job_.reset();
    job_.wait();
}
void ImageModelManager::setStatus(const QString& status, const QString& error) {
    status_ = status;
    error_ = error;
    emit stateChanged();
}
QString ImageModelManager::availableVersion() const {
    return catalog_ ? catalog_->manifest.version : QString();
}
QString ImageModelManager::pendingVersion() const {
    return pending_ ? pending_->manifest.version : QString();
}
bool ImageModelManager::updateAvailable() const {
    if (!managed_ || !catalog_) return false;
    const auto installed = versionParts(installedVersion_);
    return !installed || *versionParts(catalog_->manifest.version) > *installed;
}
void ImageModelManager::setManaged(bool value) {
    if (managed_ == value) return;
    managed_ = value;
    if (!value) {
        job_.reset();
        queued_.reset();
        pending_.reset();
        pendingPath_.clear();
        pendingAutoActivation_ = false;
        progress_ = 0;
        emit progressChanged();
        setStatus(QStringLiteral(
            "Managed downloads off; selected model remains usable locally"));
    }
    emit managedChanged();
    if (value) launch(Action::Inspect);
}
void ImageModelManager::setAutoUpdate(bool value) {
    if (autoUpdate_ == value) return;
    autoUpdate_ = value;
    if (!value && automaticJob_ && busy()) cancel();
    if (!value && pendingAutomatic_) pendingAutoActivation_ = false;
    emit autoUpdateChanged();
    if (value) automaticCheck();
}
void ImageModelManager::setActivationBlocked(bool value) {
    if (activationBlocked_ == value) return;
    activationBlocked_ = value;
    emit activationBlockedChanged();
    if (!value) maybeActivate();
}
void ImageModelManager::setActivePath(const QString& path) {
    if (activePath_ == path) return;
    job_.reset();
    queued_.reset();
    pending_.reset();
    pendingPath_.clear();
    pendingAutoActivation_ = false;
    activePath_ = path;
    installedVersion_.clear();
    emit activePathChanged();
    emit stateChanged();
    if (managed_) launch(Action::Inspect);
}
void ImageModelManager::request(Action action) {
    if (!managed_) {
        setStatus(QStringLiteral(
            "Enable managed downloads before contacting Hugging Face"));
        return;
    }
    if (busy()) {
        if (inspecting_ || currentAction_ == Action::Check ||
            currentAction_ == Action::AutoCheck)
            if (!queued_ || action == Action::Download) queued_ = action;
        return;
    }
    launch(action);
}
void ImageModelManager::checkForUpdates() { request(Action::Check); }
void ImageModelManager::downloadLatest() { request(Action::Download); }
void ImageModelManager::cancel() {
    job_.reset();
    queued_.reset();
    pendingAutoActivation_ = false;
    progress_ = 0;
    emit progressChanged();
    setStatus(
        QStringLiteral("Model operation cancelled; selected model unchanged"));
}
void ImageModelManager::applyPending() {
    if (!managed_ || !readyToApply() || busy()) return;
    launch(Action::Activate, true, false);
}
void ImageModelManager::automaticCheck() {
    if (!managed_ || !autoUpdate_ || busy()) return;
    const auto now = services_.now();
    if (lastAttempt_ > 0 && lastAttempt_ <= now && now - lastAttempt_ < Day)
        return;
    launch(Action::AutoCheck, false, true);
}
void ImageModelManager::maybeActivate() {
    if (!managed_ || activationBlocked_ || busy() || !readyToApply() ||
        !pendingAutoActivation_)
        return;
    launch(Action::Activate, false, pendingAutomatic_);
}
void ImageModelManager::launch(Action action, bool explicitActivation,
                               bool automatic) {
    if (!managed_) return;
    inspecting_ = action == Action::Inspect;
    currentAction_ = action;
    automaticJob_ = automatic;
    progress_ = 0;
    status_ =
        action == Action::Inspect
            ? QStringLiteral("Inspecting managed model files")
        : action == Action::Activate
            ? QStringLiteral("Verifying model before activation")
        : action == Action::Download
            ? QStringLiteral("Downloading and verifying public model")
            : QStringLiteral("Checking public Hugging Face model catalog");
    error_.clear();
    // AsyncJob installs its cancellation token before emitting runningChanged.
    // Do not emit earlier state signals that could re-entrantly cancel/opt-out
    // before there is a token to cancel.
    const auto services = services_;
    const auto active = activePath_;
    const auto pending = pending_;
    const auto pendingPath = pendingPath_;
    const QPointer<ImageModelManager> guard(this);
    job_.start(
        [action, services, active, pending, pendingPath, guard](Cancel cancel) {
            const Progress progress = [guard, cancel](qint64 received,
                                                      qint64 total) {
                if (omatrack::ioCancelled(cancel)) return false;
                if (guard)
                    QMetaObject::invokeMethod(
                        guard,
                        [guard, cancel, received, total] {
                            if (!guard || !guard->managed_ ||
                                omatrack::ioCancelled(cancel) ||
                                !guard->busy() ||
                                guard->job_.cancel() != cancel)
                                return;
                            guard->progress_ =
                                total > 0 ? std::clamp(double(received) / total,
                                                       0.0, 1.0)
                                          : 0;
                            emit guard->progressChanged();
                        },
                        Qt::QueuedConnection);
                return true;
            };
            return work(action, services, active, pending, pendingPath, cancel,
                        progress);
        },
        [this, action, explicitActivation,
         automatic](std::shared_ptr<ImageModelWorkResult> result) {
            inspecting_ = false;
            if (!managed_ || result->cancelled) return;
            lastAttempt_ = result->lastAttempt;
            if (result->catalog) catalog_ = result->catalog;
            if (!result->installedVersion.isEmpty())
                installedVersion_ = result->installedVersion;
            if (!result->error.isEmpty()) {
                queued_.reset();
                setStatus(QStringLiteral(
                              "Model update failed; selected model unchanged"),
                          result->error);
                return;
            }
            if (result->activate) {
                if (activationBlocked_ && !explicitActivation) {
                    setStatus(
                        QStringLiteral("Verified model pending; activation "
                                       "waits until video closes"));
                    return;
                }
                // Everything was verified on a worker; publishing the selected
                // path is just a signal. The host persists it in the one
                // omatrack.yml.
                const auto path = result->pendingPath,
                           version = result->pending->manifest.version;
                activePath_ = path;
                installedVersion_ = version;
                pending_.reset();
                pendingPath_.clear();
                pendingAutoActivation_ = false;
                progress_ = 1;
                emit modelActivated(path, version);
                if (!managed_ || activePath_ != path) return;
                emit activePathChanged();
                emit progressChanged();
                setStatus(QStringLiteral("Managed model activated"));
                return;
            }
            if (result->pending) {
                pending_ = result->pending;
                pendingPath_ = result->pendingPath;
                pendingAutomatic_ = automatic || action == Action::Inspect;
                pendingAutoActivation_ = !pendingAutomatic_ || autoUpdate_;
                progress_ = 1;
                emit progressChanged();
                setStatus(QStringLiteral(
                    "Verified model downloaded; ready to apply"));
            } else if (result->throttled)
                setStatus(result->message);
            else
                setStatus(updateAvailable()
                              ? QStringLiteral(
                                    "Compatible public model update available")
                          : installedVersion_.isEmpty()
                              ? QStringLiteral("No managed model installed")
                              : QStringLiteral("Managed model is up to date"));
            if (!managed_)
                return;  // State signals can cause a re-entrant opt-out.
            if (queued_) {
                const auto next = *queued_;
                queued_.reset();
                launch(next);
                return;
            }
            if (readyToApply()) {
                maybeActivate();
                return;
            }
            if (action == Action::Inspect) {
                automaticCheck();
                return;
            }
            if (action == Action::AutoCheck && !result->throttled &&
                autoUpdate_ && updateAvailable())
                launch(Action::Download, false, true);
        });
    emit progressChanged();
}
