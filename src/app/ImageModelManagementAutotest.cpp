// Acceptance-only: real QML preferences and optional public model download.
// No footage, fabricated model responses, or writes to caller-supplied models.
#include "ImageModelManagementAutotest.h"

#include "AsyncJob.h"
#include "ImageModelManager.h"
#include "TelemetryStore.h"
#include "YamlConfig.h"
#include "inference/GaugeReader.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QMouseEvent>
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QStandardPaths>
#include <QTimer>

#include <utility>

namespace {
constexpr qint64 HoldMs = 2000;
constexpr qint64 PhaseTimeoutMs = 20000;
constexpr qint64 DownloadTimeoutMs = 120000;
constexpr qint64 TotalTimeoutMs = 180000;
constexpr char SandboxMarker[] = "omatrack-image-model-acceptance-v1\n";

enum class Phase {
    Startup,
    Validate,
    Hold,
    ToggleBack,
    Download,
    VerifyModel,
    Persist,
    Capture,
    Finish
};
struct IoResult {
    bool ok = false;
    bool matched = false;
    QString error;
    QString configPath;
    QByteArray localSha256;
};

// Worker-only: resolve existing ancestors so symlinks cannot escape the
// sandbox, including when the application has not yet created its data/cache
// subfolder.
bool inside(const QString& path, const QString& root) {
    const QString clean = QDir::cleanPath(path);
    if (!QDir::isAbsolutePath(clean) ||
        !clean.startsWith(root + QLatin1Char('/')))
        return false;
    QFileInfo ancestor(clean);
    while (!ancestor.exists()) {
        const QString parent = ancestor.absolutePath();
        if (parent == ancestor.absoluteFilePath()) return false;
        ancestor.setFile(parent);
    }
    const QString resolved = ancestor.canonicalFilePath();
    return resolved == root || resolved.startsWith(root + QLatin1Char('/'));
}

QByteArray modelHash(const QString& path, const omatrack::IoCancel& cancel) {
    const QFileInfo info(path);
    if (!info.isFile() || info.size() <= 0 ||
        info.size() > omatrack::image_model::MaximumModelBytes)
        return {};
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    while (!file.atEnd()) {
        if (cancel->load()) return {};
        const QByteArray chunk = file.read(64 * 1024);
        if (chunk.isEmpty() && file.error() != QFileDevice::NoError) return {};
        hash.addData(chunk);
    }
    return hash.result().toHex();
}

IoResult validateInput(const QString& root, const QString& mode,
                       const QString& shot, const QString& local,
                       const QByteArray& expectedHash,
                       const omatrack::IoCancel& cancel) {
    IoResult result;
    const auto bad = [&result](const char* why) {
        result.error = QString::fromLatin1(why);
        return result;
    };
    if (cancel->load()) return bad("cancelled");
    if (!QDir::isAbsolutePath(root) || root == QDir::rootPath() ||
        QFileInfo(root).canonicalFilePath() != root)
        return bad(
            "scratch root must exist, be absolute and have no symlink aliases");
    QFile marker(
        QDir(root).filePath(QStringLiteral(".image-model-management-test")));
    if (!marker.open(QIODevice::ReadOnly) || marker.read(128) != SandboxMarker)
        return bad("scratch root lacks the explicit acceptance marker");
    const std::pair<const char*, const char*> locations[] = {
        {"HOME", "home"},
        {"XDG_CONFIG_HOME", "config"},
        {"XDG_CACHE_HOME", "cache"},
        {"XDG_DATA_HOME", "data"},
        {"XDG_RUNTIME_DIR", "runtime"}};
    for (const auto& [variable, leaf] : locations) {
        const QString expected = QDir(root).filePath(QString::fromLatin1(leaf));
        if (QDir::cleanPath(qEnvironmentVariable(variable)) != expected ||
            QFileInfo(expected).canonicalFilePath() != expected)
            return bad(
                "HOME/XDG directories must be isolated children of the scratch "
                "root");
    }
    // Verify Qt's actual platform locations, not just the environment strings.
    if (!inside(QStandardPaths::writableLocation(
                    QStandardPaths::AppLocalDataLocation),
                QDir(root).filePath(QStringLiteral("data"))) ||
        !inside(QStandardPaths::writableLocation(QStandardPaths::CacheLocation),
                QDir(root).filePath(QStringLiteral("cache"))))
        return bad(
            "Qt model data/cache locations are not isolated; use the "
            "documented Linux runner");
    result.configPath = omatrack::YamlConfig::filePath();
    if (!inside(result.configPath,
                QDir(root).filePath(QStringLiteral("config"))))
        return bad("actual omatrack.yml is outside scratch configuration");
    if (!inside(shot, root) ||
        !shot.endsWith(QStringLiteral(".png"), Qt::CaseInsensitive) ||
        !QFileInfo(shot).dir().exists() || QFileInfo::exists(shot))
        return bad("screenshot must be a new PNG inside the scratch root");
    if (mode == QStringLiteral("download") &&
        !omatrack::image_model::validSha256(QString::fromLatin1(expectedHash)))
        return bad("download mode requires the expected public model SHA256");
    for (const char* leaf : {"data", "cache"}) {
        QDirIterator models(QDir(root).filePath(QString::fromLatin1(leaf)),
                            {QStringLiteral("*.onnx")}, QDir::Files,
                            QDirIterator::Subdirectories);
        if (models.hasNext())
            return bad(
                "scratch model data/cache must initially contain no ONNX "
                "files");
    }
    QString error;
    const QVariantMap document =
        QFileInfo::exists(result.configPath)
            ? omatrack::YamlConfig::readDocument(result.configPath, &error)
            : QVariantMap{};
    if (!error.isEmpty())
        return bad("scratch input YAML is unreadable or malformed");
    const auto video = document.value(QStringLiteral("video")).toMap();
    if (video.value(QStringLiteral("image_model_managed"), false).toBool())
        return bad(
            "all input modes must begin without managed-download consent");
    if (mode == QStringLiteral("existing")) {
        if (local.isEmpty() || !QDir::isAbsolutePath(local) ||
            !video.contains(QStringLiteral("image_telemetry")) ||
            !video.value(QStringLiteral("image_telemetry")).toBool() ||
            video.value(QStringLiteral("image_model")).toString() != local ||
            !video.contains(QStringLiteral("image_model_updates")) ||
            video.value(QStringLiteral("image_model_updates")).toBool())
            return bad(
                "existing mode requires explicit extraction=true, "
                "updates=false and the supplied local model path");
        result.localSha256 = modelHash(local, cancel);
        if (result.localSha256.isEmpty())
            return bad("local fixture is not a readable bounded model file");
    } else if (!local.isEmpty() ||
               video.value(QStringLiteral("image_telemetry"), false).toBool() ||
               !video.value(QStringLiteral("image_model"))
                    .toString()
                    .isEmpty() ||
               !video.value(QStringLiteral("image_model_updates"), true)
                    .toBool()) {
        return bad(
            "fresh/download modes require default image preferences and no "
            "custom model");
    }
    result.ok = true;
    return result;
}

IoResult persisted(const QString& configPath, const QString& modelPath,
                   bool extraction, bool managed, bool updates,
                   const QString& local, const QByteArray& localHash,
                   const omatrack::IoCancel& cancel) {
    IoResult result;
    if (cancel->load()) return result;
    QString error;
    const auto document =
        omatrack::YamlConfig::readDocument(configPath, &error);
    if (!error.isEmpty())
        return result;  // The debounced writer may not have published yet.
    const auto video = document.value(QStringLiteral("video")).toMap();
    result.matched =
        video.contains(QStringLiteral("image_telemetry")) &&
        video.value(QStringLiteral("image_telemetry")).toBool() == extraction &&
        video.contains(QStringLiteral("image_model_managed")) &&
        video.value(QStringLiteral("image_model_managed")).toBool() ==
            managed &&
        video.contains(QStringLiteral("image_model_updates")) &&
        video.value(QStringLiteral("image_model_updates")).toBool() ==
            updates &&
        video.contains(QStringLiteral("image_model")) &&
        video.value(QStringLiteral("image_model")).toString() == modelPath;
    if (!local.isEmpty() && modelHash(local, cancel) != localHash) {
        result.error = QStringLiteral(
            "caller-supplied local model changed during the test");
        return result;
    }
    result.ok = true;
    return result;
}

class ModelManagementCheck final : public QObject {
public:
    ModelManagementCheck(QQmlApplicationEngine& engine, TelemetryStore& store,
                         QString mode)
        : QObject(&engine),
          engine_(engine),
          store_(store),
          mode_(std::move(mode)),
          io_(this) {
        rootPath_ = QDir::cleanPath(
            qEnvironmentVariable("OMATRACK_AUTOTEST_IMAGE_MODEL_ROOT"));
        localPath_ =
            qEnvironmentVariable("OMATRACK_AUTOTEST_IMAGE_MODEL_LOCAL");
        expectedHash_ =
            qgetenv("OMATRACK_AUTOTEST_IMAGE_MODEL_SHA256").toLower();
        shot_ = qEnvironmentVariable("OMATRACK_AUTOTEST");
        total_.start();
        phaseTime_.start();
        timer_.setInterval(50);
        connect(&timer_, &QTimer::timeout, this, [this] { tick(); });
        timer_.start();
    }

private:
    void enter(Phase phase) {
        phase_ = phase;
        phaseTime_.restart();
    }
    void fail(const QString& why) {
        if (finished_) return;
        finished_ = true;
        timer_.stop();
        io_.reset();
        qWarning() << "AUTOTEST image model FAIL" << mode_ << "phase"
                   << int(phase_) << why;
        // Do not dump paths, model bytes, or the user's configuration.
        QCoreApplication::exit(1);
    }
    bool require(bool condition, const char* why) {
        if (!condition) fail(QString::fromLatin1(why));
        return condition;
    }
    bool optedOut() {
        return require(
            !store_.imageModelManaged() && !manager_->managed() &&
                manager_->networkRequestCount() == 0 && activations_ == 0,
            "model network activity or activation occurred before consent");
    }
    bool originalSettings(bool inverted = false) {
        const bool existing = mode_ == QStringLiteral("existing");
        return require(
            store_.imageTelemetryEnabled() == (existing != inverted) &&
                store_.imageModelUpdates() == !existing &&
                manager_->autoUpdate() == !existing &&
                store_.imageTelemetryModel() == localPath_ &&
                manager_->activePath() == localPath_,
            "fresh defaults or explicit existing local preferences were not "
            "retained");
    }
    QQuickItem* control(const char* name) const {
        return settings_->findChild<QQuickItem*>(QString::fromLatin1(name));
    }
    bool click(const char* name) {
        auto* item = control(name);
        if (!require(item && item->isVisible() && item->isEnabled() &&
                         item->width() > 1 && item->height() > 1,
                     "real enabled preferences control is unavailable"))
            return false;
        const QPointF local(item->width() / 2, item->height() / 2);
        const QPointF scene = item->mapToScene(local);
        if (!require(QRectF(0, 0, settings_->width(), settings_->height())
                         .contains(scene),
                     "preferences click target is outside the visible window"))
            return false;
        const QPointF global = item->mapToGlobal(local);
        QMouseEvent press(QEvent::MouseButtonPress, local, scene, global,
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QMouseEvent release(QEvent::MouseButtonRelease, local, scene, global,
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(item, &press);
        QCoreApplication::sendEvent(item, &release);
        return true;
    }
    void tick() {
        if (finished_) return;
        if (total_.elapsed() > TotalTimeoutMs ||
            phaseTime_.elapsed() > (phase_ == Phase::Download
                                        ? DownloadTimeoutMs
                                        : PhaseTimeoutMs)) {
            fail(QStringLiteral("finite acceptance deadline exceeded"));
            return;
        }
        if (manager_ && !consented_ && !optedOut()) return;
        switch (phase_) {
            case Phase::Startup: {
                if (!require(mode_ == QStringLiteral("fresh") ||
                                 mode_ == QStringLiteral("existing") ||
                                 mode_ == QStringLiteral("download"),
                             "unknown image-model acceptance mode"))
                    return;
                if (!require(
                        qEnvironmentVariableIsEmpty("OMATRACK_VIDEO"),
                        "model-management smoke must not open source footage"))
                    return;
                if (engine_.rootObjects().isEmpty() || !store_.ready() ||
                    store_.loading())
                    return;
                window_ =
                    qobject_cast<QQuickWindow*>(engine_.rootObjects().first());
                manager_ = window_ ? window_->findChild<ImageModelManager*>(
                                         QStringLiteral("imageModelManager"))
                                   : nullptr;
                settings_ = window_ ? window_->findChild<QQuickWindow*>(
                                          QStringLiteral("settingsWindow"))
                                    : nullptr;
                if (!require(window_ && manager_ && settings_,
                             "application manager/preferences wiring missing"))
                    return;
                if (!require(!window_->property("videoVisible").toBool() &&
                                 store_.primarySessionKey().isEmpty(),
                             "smoke requires an empty video/session workspace"))
                    return;
                if (!optedOut() || !originalSettings()) return;
                connect(
                    manager_, &ImageModelManager::modelActivated, this,
                    [this](const QString& path, const QString& version) {
                        if (!require(consented_ &&
                                         mode_ == QStringLiteral("download") &&
                                         store_.imageModelManaged(),
                                     "unexpected model activation without test "
                                     "consent"))
                            return;
                        ++activations_;
                        activatedPath_ = path;
                        activatedVersion_ = version;
                    });
                enter(Phase::Validate);
                const QString root = rootPath_, mode = mode_, shot = shot_,
                              local = localPath_;
                const QByteArray hash = expectedHash_;
                io_.start(
                    [root, mode, shot, local, hash](omatrack::IoCancel cancel) {
                        return validateInput(root, mode, shot, local, hash,
                                             cancel);
                    },
                    [this](IoResult result) {
                        if (!result.ok) {
                            fail(result.error);
                            return;
                        }
                        configPath_ = result.configPath;
                        localHash_ = result.localSha256;
                        if (!require(QMetaObject::invokeMethod(
                                         settings_, "openImageTelemetry"),
                                     "image telemetry preferences entry point "
                                     "missing"))
                            return;
                        enter(Phase::Hold);
                    });
                return;
            }
            case Phase::Validate:
            case Phase::VerifyModel:
            case Phase::Finish: return;
            case Phase::Hold:
                if (!originalSettings()) return;
                if (phaseTime_.elapsed() < HoldMs || manager_->busy()) return;
                if (!require(
                        settings_->isVisible() &&
                            settings_->property("currentSection").toInt() == 7,
                        "image telemetry preferences did not open at the "
                        "appended section"))
                    return;
                qWarning() << "AUTOTEST image model: zero requests before "
                              "consent; original preferences retained";
                if (mode_ == QStringLiteral("download")) {
                    consented_ = true;
                    enter(Phase::Download);
                    if (!click("imageModelConsentButton")) return;
                    require(store_.imageModelManaged() &&
                                store_.imageTelemetryEnabled(),
                            "actual consent button did not enable management "
                            "and extraction");
                } else {
                    if (!click("imageTelemetryEnabledPreference") ||
                        !originalSettings(true))
                        return;
                    enter(Phase::ToggleBack);
                }
                return;
            case Phase::ToggleBack:
                if (phaseTime_.elapsed() < 150) return;
                if (!originalSettings(true) ||
                    !click("imageTelemetryEnabledPreference") ||
                    !originalSettings())
                    return;
                enter(Phase::Persist);
                return;
            case Phase::Download: {
                if (!require(store_.imageModelManaged() &&
                                 store_.imageTelemetryEnabled(),
                             "download consent or extraction setting was lost"))
                    return;
                if (!manager_->busy() && !manager_->error().isEmpty()) {
                    fail(
                        QStringLiteral("public model manager reported a "
                                       "download/verification error"));
                    return;
                }
                if (manager_->busy() || activatedPath_.isEmpty()) return;
                if (!require(
                        activations_ == 1 && !activatedVersion_.isEmpty() &&
                            manager_->networkRequestCount() > 0 &&
                            store_.imageTelemetryModel() == activatedPath_ &&
                            manager_->activePath() == activatedPath_ &&
                            manager_->installedVersion() == activatedVersion_,
                        "downloaded activation did not reach the real "
                        "Store/model binding"))
                    return;
                enter(Phase::VerifyModel);
                const QString path = activatedPath_, root = rootPath_;
                const QByteArray expected = expectedHash_;
                io_.start(
                    [path, root, expected](omatrack::IoCancel cancel) {
                        IoResult result;
                        if (!inside(path, QDir(root).filePath(
                                              QStringLiteral("data"))) ||
                            modelHash(path, cancel) != expected) {
                            result.error = QStringLiteral(
                                "activated model is outside scratch data or "
                                "its public SHA256 differs");
                            return result;
                        }
                        if (cancel->load()) return result;
                        omatrack::inference::GaugeReader reader(
                            path.toStdString());
                        result.ok = reader.ready();
                        if (!result.ok)
                            result.error = QStringLiteral(
                                "downloaded model did not load through the "
                                "native GaugeReader");
                        return result;
                    },
                    [this](IoResult result) {
                        if (!result.ok) {
                            fail(result.error);
                            return;
                        }
                        qWarning() << "AUTOTEST image model: public download "
                                      "hash and native model load verified";
                        enter(Phase::Persist);
                    });
                return;
            }
            case Phase::Persist: {
                if (io_.running() || total_.elapsed() < nextReadMs_) return;
                nextReadMs_ = total_.elapsed() + 250;
                const bool download = mode_ == QStringLiteral("download");
                const bool existing = mode_ == QStringLiteral("existing");
                const QString config = configPath_,
                              model = download ? activatedPath_ : localPath_;
                const QString local = localPath_;
                const QByteArray localHash = localHash_;
                io_.start(
                    [config, model, local, localHash, download,
                     existing](omatrack::IoCancel cancel) {
                        return persisted(config, model, download || existing,
                                         download, !existing, local, localHash,
                                         cancel);
                    },
                    [this](IoResult result) {
                        if (!result.error.isEmpty()) {
                            fail(result.error);
                            return;
                        }
                        if (!result.ok || !result.matched) return;
                        qWarning() << "AUTOTEST image model: debounced "
                                      "omatrack.yml persistence verified";
                        enter(Phase::Capture);
                    });
                return;
            }
            case Phase::Capture: {
                const QImage image = settings_->grabWindow();
                if (!require(!image.isNull(),
                             "preferences screenshot is empty"))
                    return;
                enter(Phase::Finish);
                const QString shot = shot_, root = rootPath_;
                io_.start(
                    [image, shot, root](omatrack::IoCancel cancel) {
                        IoResult result;
                        if (cancel->load() || !inside(shot, root))
                            return result;
                        QFile output(shot);
                        if (!output.open(QIODevice::WriteOnly |
                                         QIODevice::NewOnly))
                            return result;
                        result.ok =
                            image.save(&output, "PNG") && output.flush();
                        output.close();
                        if (!result.ok)
                            QFile::remove(
                                shot);  // Only our newly created artifact.
                        return result;
                    },
                    [this](IoResult result) {
                        if (!require(result.ok,
                                     "could not create private acceptance "
                                     "screenshot"))
                            return;
                        if (!consented_ && !optedOut()) return;
                        finished_ = true;
                        timer_.stop();
                        qWarning()
                            << "AUTOTEST image model PASS" << mode_
                            << "transport attempts"
                            << manager_->networkRequestCount() << "activations"
                            << activations_ << "elapsed ms" << total_.elapsed();
                        QCoreApplication::exit(0);
                    });
                return;
            }
        }
    }

    QQmlApplicationEngine& engine_;
    TelemetryStore& store_;
    const QString mode_;
    AsyncJob<IoResult> io_;
    QTimer timer_;
    QElapsedTimer total_, phaseTime_;
    QQuickWindow* window_ = nullptr;
    QQuickWindow* settings_ = nullptr;
    ImageModelManager* manager_ = nullptr;
    QString rootPath_, localPath_, shot_, configPath_;
    QString activatedPath_, activatedVersion_;
    QByteArray expectedHash_, localHash_;
    Phase phase_ = Phase::Startup;
    qint64 nextReadMs_ = 0;
    int activations_ = 0;
    bool consented_ = false;
    bool finished_ = false;
};
}  // namespace

bool omatrack::autotest::installImageModelManagement(
    QQmlApplicationEngine& engine, TelemetryStore& store) {
    const QString mode = qEnvironmentVariable("OMATRACK_AUTOTEST_IMAGE_MODEL");
    if (mode.isEmpty()) return false;
    new ModelManagementCheck(engine, store, mode);
    return true;
}
