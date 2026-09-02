// Video synchronisation and timing controller.
//
// Owns the reference-recording sync state, the primary-clock slow-motion flag,
// and the next-lap 3-2-1 countdown — all the timing logic that used to live as
// inline functions and Timers in Main.qml. QML keeps layout (compose modes,
// fullscreen, controls) and binds to the properties/signals exposed here.
//
// The controller reads TelemetryStore public API only and is driven by the two
// MpvVideoItem pointers QML assigns after creation. It connects to player and
// store signals itself, so QML no longer needs nested Connections blocks or
// hand-rolled Timers for sync.
//
// libmpv time-pos only stores the last media time. Overlay, header, traces
// and channels are enqueued on DeadlineQueue with deadlines and pumped from
// QTimer / QQuickWindow::afterAnimating — never from processEvents.

#pragma once

#include "DeadlineQueue.h"

#include <QObject>
#include <QPointer>
#include <QQmlEngine>
#include <QString>
#include <QTimer>

class MpvVideoItem;
class QQuickWindow;
class TelemetryStore;

class VideoSyncController : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_NAMED_ELEMENT(VideoSync)

    Q_PROPERTY(MpvVideoItem* primaryPlayer READ primaryPlayer WRITE
                   setPrimaryPlayer NOTIFY primaryPlayerChanged)
    Q_PROPERTY(MpvVideoItem* referencePlayer READ referencePlayer WRITE
                   setReferencePlayer NOTIFY referencePlayerChanged)
    Q_PROPERTY(
        TelemetryStore* store READ store WRITE setStore NOTIFY storeChanged)
    Q_PROPERTY(bool telemetryVideoActive READ telemetryVideoActive WRITE
                   setTelemetryVideoActive NOTIFY telemetryVideoActiveChanged)
    Q_PROPERTY(bool videoFullscreen READ videoFullscreen WRITE
                   setVideoFullscreen NOTIFY videoFullscreenChanged)
    Q_PROPERTY(
        bool videoSlowMotion READ videoSlowMotion NOTIFY videoSlowMotionChanged)
    Q_PROPERTY(bool dualVideo READ dualVideo NOTIFY dualVideoChanged)
    Q_PROPERTY(QString referenceSyncState READ referenceSyncState NOTIFY
                   referenceSyncStateChanged)
    Q_PROPERTY(double referenceSyncError READ referenceSyncError NOTIFY
                   referenceSyncStateChanged)
    Q_PROPERTY(double referenceSyncBaseRate READ referenceSyncBaseRate NOTIFY
                   referenceSyncStateChanged)
    Q_PROPERTY(
        int lapAdvanceCount READ lapAdvanceCount NOTIFY lapAdvanceCountChanged)
    Q_PROPERTY(QString lapAdvanceNextLabel READ lapAdvanceNextLabel NOTIFY
                   lapAdvanceCountChanged)
    Q_PROPERTY(double sampledMediaTime READ sampledMediaTime NOTIFY
                   sampledMediaTimeChanged)

public:
    explicit VideoSyncController(QObject* parent = nullptr);

    MpvVideoItem* primaryPlayer() const { return primaryPlayer_; }
    MpvVideoItem* referencePlayer() const { return referencePlayer_; }
    TelemetryStore* store() const { return store_; }
    bool telemetryVideoActive() const { return telemetryVideoActive_; }
    bool videoFullscreen() const { return videoFullscreen_; }
    bool videoSlowMotion() const { return videoSlowMotion_; }
    bool dualVideo() const;
    QString referenceSyncState() const { return referenceSyncState_; }
    double referenceSyncError() const { return referenceSyncError_; }
    double referenceSyncBaseRate() const { return referenceSyncBaseRate_; }
    int lapAdvanceCount() const { return lapAdvanceCount_; }
    QString lapAdvanceNextLabel() const { return lapAdvanceNextLabel_; }
    double sampledMediaTime() const { return sampledMediaTime_; }

    void setPrimaryPlayer(MpvVideoItem* player);
    void setReferencePlayer(MpvVideoItem* player);
    void setStore(TelemetryStore* store);
    void setTelemetryVideoActive(bool active);
    void setVideoFullscreen(bool fullscreen);

    // User actions called from QML.
    Q_INVOKABLE void togglePaused();
    Q_INVOKABLE void seekRelative(double seconds);
    Q_INVOKABLE void seekToTelemetry();
    Q_INVOKABLE void syncReferenceSource();
    Q_INVOKABLE void toggleSlowMotion();
    Q_INVOKABLE void cancelLapAdvance();
    Q_INVOKABLE void reopenExpiredVideo(MpvVideoItem* player);

signals:
    void primaryPlayerChanged();
    void referencePlayerChanged();
    void storeChanged();
    void telemetryVideoActiveChanged();
    void videoFullscreenChanged();
    void videoSlowMotionChanged();
    void dualVideoChanged();
    void referenceSyncStateChanged();
    void lapAdvanceCountChanged();
    void sampledMediaTimeChanged();

private slots:
    void onPrimaryLoadedChanged();
    void onPrimaryPositionChanged();
    void onPrimarySeekingChanged();
    void onPrimaryPausedChanged();
    void onReferenceLoadedChanged();
    void onReferencePausedChanged();
    void onStoreVideoTimeChanged();
    void onStoreComparisonSyncStrategyChanged();
    void onStoreLapLoadingChanged();
    void onStorePrimaryLapPlaybackEnded();
    void onStoreSelectionChanged();
    void onContinuousSyncTick();
    void onPausedAlignmentTick();
    void onLapAdvanceTick();
    void onDisplayTick();

private:
    void connectPrimary();
    void disconnectPrimary();
    void connectReference();
    void disconnectReference();
    void connectStore();
    void disconnectStore();
    void updateDualVideo();
    void updateContinuousSyncTimer();
    double primaryClockRate() const;
    void lockPrimaryRealtime();
    void syncReferenceVideo(bool force);
    void realignPausedVideos();
    void verifyPausedVideoAlignment();
    void startLapAdvance();
    void tickLapAdvance();
    void finishLapAdvance();
    void tryResumeLapAdvance();
    void setReferenceSyncState(const QString& state);
    void setLapAdvanceCount(int count);
    void attachDisplayPump();
    void enqueueTelemetry(bool immediate);
    void applySampledCursor();
    double sampledPlayerTime() const;

    MpvVideoItem* primaryPlayer_ = nullptr;
    MpvVideoItem* referencePlayer_ = nullptr;
    TelemetryStore* store_ = nullptr;
    QPointer<QQuickWindow> displayWindow_;

    bool telemetryVideoActive_ = false;
    bool videoFullscreen_ = false;
    bool videoSlowMotion_ = false;

    QString referenceSyncState_ = QStringLiteral("WAIT");
    double referenceSyncError_ = 0;
    double referenceSyncBaseRate_ = 1;
    double referenceSyncLastPrimary_ = -1;
    double referenceSyncLastTarget_ = -1;
    int referenceSyncPauseAttempts_ = 0;
    double sampledMediaTime_ = 0.0;
    double lastPrimaryMediaTime_ = 0.0;

    int lapAdvanceCount_ = 0;
    int lapAdvanceNextId_ = -1;
    QString lapAdvanceNextLabel_;
    bool lapAdvanceResume_ = false;

    DeadlineQueue queue_;
    QTimer continuousSyncTimer_;
    QTimer pausedAlignmentTimer_;
    QTimer lapAdvanceTimer_;

    QMetaObject::Connection primaryLoadedConn_;
    QMetaObject::Connection primaryPositionConn_;
    QMetaObject::Connection primarySeekingConn_;
    QMetaObject::Connection primaryPausedConn_;
    QMetaObject::Connection primaryWindowConn_;
    QMetaObject::Connection displayTickConn_;
    QMetaObject::Connection referenceLoadedConn_;
    QMetaObject::Connection referencePausedConn_;
    QMetaObject::Connection storeVideoTimeConn_;
    QMetaObject::Connection storeSyncStrategyConn_;
    QMetaObject::Connection storeLapLoadingConn_;
    QMetaObject::Connection storePlaybackEndedConn_;
    QMetaObject::Connection storeSelectionConn_;
};
