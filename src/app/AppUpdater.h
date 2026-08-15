// GitHub Releases checker and one-click portable installer.
//
// Linux AppImages (`$APPIMAGE`), Windows zip trees (`qt.conf` next to
// omatrack.exe), and macOS `Omatrack.app` bundles ask
// `/repos/tobi/omatrack/releases/latest` on the I/O thread and surface
// the result in the header. Linux replaces the running AppImage in place.
// Windows and macOS stage the new package, then a helper waits for this
// process to exit, replaces the install, and relaunches. Source builds
// stay silent. Preferences live under `updates` in omatrack.yml.
#pragma once

#include "AppUpdate.h"

#include <QObject>
#include <QString>
#include <QtQml/qqmlregistration.h>

#include <atomic>
#include <memory>

class QTimer;

class AppUpdater : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(Updater)
    QML_SINGLETON
    Q_PROPERTY(bool supported READ supported CONSTANT)
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged)
    Q_PROPERTY(bool available READ available NOTIFY availableChanged)
    Q_PROPERTY(
        bool bannerVisible READ bannerVisible NOTIFY bannerVisibleChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY statusChanged)
    Q_PROPERTY(QString currentVersion READ currentVersion CONSTANT)
    Q_PROPERTY(QString latestVersion READ latestVersion NOTIFY availableChanged)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(QString error READ error NOTIFY statusChanged)
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(bool associationPrompt READ associationPrompt NOTIFY
                   associationPromptChanged)
    Q_PROPERTY(int associationCount READ associationCount NOTIFY
                   associationPromptChanged)

public:
    explicit AppUpdater(QObject* parent = nullptr);
    ~AppUpdater() override;

    bool supported() const { return supported_; }
    bool enabled() const { return enabled_; }
    bool available() const { return available_; }
    bool bannerVisible() const;
    bool busy() const;
    QString currentVersion() const { return currentVersion_; }
    QString latestVersion() const { return latest_.version; }
    QString status() const { return status_; }
    QString error() const { return error_; }
    double progress() const { return progress_; }
    bool associationPrompt() const { return associationPrompt_; }
    int associationCount() const;

    Q_INVOKABLE void setEnabled(bool enabled);
    Q_INVOKABLE void checkNow();
    Q_INVOKABLE void install();
    Q_INVOKABLE void snooze();
    Q_INVOKABLE void cancel();
    Q_INVOKABLE QString associationExtension(int index) const;
    Q_INVOKABLE QString associationLabel(int index) const;
    Q_INVOKABLE bool associationEnabled(int index) const;
    Q_INVOKABLE bool associationVideo(int index) const;
    Q_INVOKABLE void setAssociationEnabled(int index, bool enabled);
    Q_INVOKABLE void finishAssociationPrompt();

signals:
    void enabledChanged();
    void availableChanged();
    void bannerVisibleChanged();
    void statusChanged();
    void progressChanged();
    void associationPromptChanged();

private:
    enum class Phase { Idle, Checking, Downloading, Installing };

    void loadState();
    void saveState() const;
    void scheduleCheck(int delayMs);
    void startCheck(bool force);
    void adoptRelease(const omatrack::GithubRelease& release,
                      const QString& sha256);
    void clearAvailable();
    void setPhase(Phase phase, const QString& status = {},
                  const QString& error = {});
    void setProgress(double progress);
    void emitVisibility();
    bool snoozed() const;

    bool supported_ = false;
    bool enabled_ = true;
    bool available_ = false;
    Phase phase_ = Phase::Idle;
    QString currentVersion_;
    QString installPath_;
    QString updateExePath_;
    bool associationPrompt_ = false;
    QString status_;
    QString error_;
    QString sha256_;
    QString snoozeUntil_;
    QString snoozeVersion_;
    QString lastCheck_;
    double progress_ = 0.0;
    quint64 generation_ = 0;
    omatrack::GithubRelease latest_;
    std::shared_ptr<std::atomic<bool>> cancel_;
    QTimer* pollTimer_ = nullptr;
};
