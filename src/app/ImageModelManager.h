// origin: PUBLIC — opt-in public model lifecycle; preferences remain in Store.
#pragma once

#include "AsyncJob.h"
#include "ImageModelManifest.h"

#include <QObject>
#include <QTimer>
#include <QtQml/qqmlregistration.h>

#include <functional>
#include <memory>

struct ImageModelCatalog;
struct ImageModelWorkResult;

// Trusted C++ injection seam for deterministic tests. Not exposed to QML or
// configuration: production always uses the fixed HF repository/HTTPS policy.
struct ImageModelServices {
    QString dataRoot;
    QString cacheRoot;
    QString appVersion;
    std::function<qint64()> now;
    std::function<omatrack::FileDownload(const QUrl&, const QString&, qint64,
                                         const omatrack::DownloadProgress&,
                                         const omatrack::IoCancel&)>
        transfer;
    std::function<bool(const QString&, const omatrack::image_model::Manifest&,
                       QString*)>
        verifyModel;
};

class ImageModelManager : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(
        bool managed READ managed WRITE setManaged NOTIFY managedChanged FINAL)
    Q_PROPERTY(bool autoUpdate READ autoUpdate WRITE setAutoUpdate NOTIFY
                   autoUpdateChanged FINAL)
    Q_PROPERTY(bool activationBlocked READ activationBlocked WRITE
                   setActivationBlocked NOTIFY activationBlockedChanged FINAL)
    Q_PROPERTY(QString activePath READ activePath WRITE setActivePath NOTIFY
                   activePathChanged FINAL)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged FINAL)
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged FINAL)
    Q_PROPERTY(QString status READ status NOTIFY stateChanged FINAL)
    Q_PROPERTY(QString error READ error NOTIFY stateChanged FINAL)
    Q_PROPERTY(QString installedVersion READ installedVersion NOTIFY
                   stateChanged FINAL)
    Q_PROPERTY(QString availableVersion READ availableVersion NOTIFY
                   stateChanged FINAL)
    Q_PROPERTY(
        bool updateAvailable READ updateAvailable NOTIFY stateChanged FINAL)
    Q_PROPERTY(
        QString pendingVersion READ pendingVersion NOTIFY stateChanged FINAL)
    Q_PROPERTY(QString pendingPath READ pendingPath NOTIFY stateChanged FINAL)
    Q_PROPERTY(bool readyToApply READ readyToApply NOTIFY stateChanged FINAL)

public:
    explicit ImageModelManager(QObject* parent = nullptr);
    explicit ImageModelManager(ImageModelServices services,
                               QObject* parent = nullptr);
    ~ImageModelManager() override;
    bool managed() const { return managed_; }
    void setManaged(bool value);
    bool autoUpdate() const { return autoUpdate_; }
    void setAutoUpdate(bool value);
    bool activationBlocked() const { return activationBlocked_; }
    void setActivationBlocked(bool value);
    QString activePath() const { return activePath_; }
    void setActivePath(const QString& path);
    bool busy() const { return job_.running(); }
    double progress() const { return progress_; }
    QString status() const { return status_; }
    QString error() const { return error_; }
    QString installedVersion() const { return installedVersion_; }
    QString availableVersion() const;
    bool updateAvailable() const;
    QString pendingVersion() const;
    QString pendingPath() const { return pendingPath_; }
    bool readyToApply() const {
        return managed_ && pending_ && !pendingPath_.isEmpty();
    }
    // C++ acceptance instrumentation; zero proves no model transport attempt.
    quint64 networkRequestCount() const { return networkRequests_->load(); }

    // Host opts in (managed=true) before explicit Download. No method silently
    // enables management or accesses HF while management is disabled.
    Q_INVOKABLE void downloadLatest();
    Q_INVOKABLE void checkForUpdates();
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void applyPending();

signals:
    void managedChanged();
    void autoUpdateChanged();
    void activationBlockedChanged();
    void activePathChanged();
    void stateChanged();
    void progressChanged();
    void modelActivated(const QString& path, const QString& version);

private:
    enum class Action { Inspect, AutoCheck, Check, Download, Activate };
    void request(Action action);
    void launch(Action action, bool explicitActivation = false,
                bool automatic = false);
    void automaticCheck();
    void maybeActivate();
    void setStatus(const QString& status, const QString& error = {});
    static std::shared_ptr<ImageModelWorkResult> work(
        Action action, ImageModelServices services, const QString& activePath,
        std::shared_ptr<const ImageModelCatalog> pending,
        const QString& pendingPath, const omatrack::IoCancel& cancel,
        const omatrack::DownloadProgress& progress);

    ImageModelServices services_;
    std::shared_ptr<std::atomic<quint64>> networkRequests_ =
        std::make_shared<std::atomic<quint64>>(0);
    AsyncJob<std::shared_ptr<ImageModelWorkResult>> job_;
    QTimer timer_;
    bool managed_ = false;
    bool autoUpdate_ = true;
    bool activationBlocked_ = false;
    bool inspecting_ = false;
    bool pendingAutoActivation_ = false;
    bool automaticJob_ = false;
    bool pendingAutomatic_ = false;
    Action currentAction_ = Action::Inspect;
    std::optional<Action> queued_;
    QString activePath_, installedVersion_, pendingPath_;
    QString status_ = QStringLiteral("Managed model downloads are off");
    QString error_;
    double progress_ = 0;
    qint64 lastAttempt_ = 0;
    std::shared_ptr<const ImageModelCatalog> catalog_, pending_;
};
