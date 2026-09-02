#include "VideoSyncController.h"

#include "MpvVideoItem.h"
#include "TelemetryStore.h"

#include <QDeadlineTimer>
#include <QQuickItem>
#include <QQuickWindow>
#include <QUrl>
#include <QtMath>

#include <chrono>

namespace {

QDeadlineTimer deadlineFrom(bool immediate, std::chrono::milliseconds delay) {
    if (immediate) return QDeadlineTimer();
    return QDeadlineTimer(delay);
}

}  // namespace

VideoSyncController::VideoSyncController(QObject* parent) : QObject(parent) {
    continuousSyncTimer_.setInterval(100);
    QObject::connect(&continuousSyncTimer_, &QTimer::timeout, this,
                     &VideoSyncController::onContinuousSyncTick);

    pausedAlignmentTimer_.setInterval(120);
    pausedAlignmentTimer_.setSingleShot(true);
    QObject::connect(&pausedAlignmentTimer_, &QTimer::timeout, this,
                     &VideoSyncController::onPausedAlignmentTick);

    lapAdvanceTimer_.setInterval(500);
    QObject::connect(&lapAdvanceTimer_, &QTimer::timeout, this,
                     &VideoSyncController::onLapAdvanceTick);
}

// ── properties ───────────────────────────────────────────────────

bool VideoSyncController::dualVideo() const {
    if (!telemetryVideoActive_ || !store_) return false;
    const QUrl compare = store_->compareVideoSource();
    if (compare.toString().isEmpty()) return false;
    return compare != store_->primaryVideoSource();
}

void VideoSyncController::setPrimaryPlayer(MpvVideoItem* player) {
    if (primaryPlayer_ == player) return;
    disconnectPrimary();
    primaryPlayer_ = player;
    connectPrimary();
    attachDisplayPump();
    updateContinuousSyncTimer();
    emit primaryPlayerChanged();
}

void VideoSyncController::setReferencePlayer(MpvVideoItem* player) {
    if (referencePlayer_ == player) return;
    disconnectReference();
    referencePlayer_ = player;
    connectReference();
    if (referencePlayer_) syncReferenceSource();
    emit referencePlayerChanged();
}

void VideoSyncController::setStore(TelemetryStore* store) {
    if (store_ == store) return;
    disconnectStore();
    store_ = store;
    connectStore();
    updateDualVideo();
    emit storeChanged();
}

void VideoSyncController::setTelemetryVideoActive(bool active) {
    if (telemetryVideoActive_ == active) return;
    telemetryVideoActive_ = active;
    updateDualVideo();
    emit telemetryVideoActiveChanged();
}

void VideoSyncController::setVideoFullscreen(bool fullscreen) {
    if (videoFullscreen_ == fullscreen) return;
    videoFullscreen_ = fullscreen;
    if (!fullscreen && videoSlowMotion_) {
        videoSlowMotion_ = false;
        lockPrimaryRealtime();
        emit videoSlowMotionChanged();
    }
    emit videoFullscreenChanged();
}

// ── signal wiring ────────────────────────────────────────────────

void VideoSyncController::connectPrimary() {
    if (!primaryPlayer_) return;
    primaryLoadedConn_ =
        QObject::connect(primaryPlayer_, &MpvVideoItem::loadedChanged, this,
                         &VideoSyncController::onPrimaryLoadedChanged);
    primaryPositionConn_ =
        QObject::connect(primaryPlayer_, &MpvVideoItem::positionChanged, this,
                         &VideoSyncController::onPrimaryPositionChanged);
    primarySeekingConn_ =
        QObject::connect(primaryPlayer_, &MpvVideoItem::seekingChanged, this,
                         &VideoSyncController::onPrimarySeekingChanged);
    primaryPausedConn_ =
        QObject::connect(primaryPlayer_, &MpvVideoItem::pausedChanged, this,
                         &VideoSyncController::onPrimaryPausedChanged);
    primaryWindowConn_ =
        QObject::connect(primaryPlayer_, &QQuickItem::windowChanged, this,
                         [this](QQuickWindow*) { attachDisplayPump(); });
}

void VideoSyncController::disconnectPrimary() {
    if (primaryLoadedConn_) QObject::disconnect(primaryLoadedConn_);
    if (primaryPositionConn_) QObject::disconnect(primaryPositionConn_);
    if (primarySeekingConn_) QObject::disconnect(primarySeekingConn_);
    if (primaryPausedConn_) QObject::disconnect(primaryPausedConn_);
    if (primaryWindowConn_) QObject::disconnect(primaryWindowConn_);
    if (displayTickConn_) QObject::disconnect(displayTickConn_);
    displayWindow_.clear();
}

void VideoSyncController::connectReference() {
    if (!referencePlayer_) return;
    referenceLoadedConn_ =
        QObject::connect(referencePlayer_, &MpvVideoItem::loadedChanged, this,
                         &VideoSyncController::onReferenceLoadedChanged);
    referencePausedConn_ =
        QObject::connect(referencePlayer_, &MpvVideoItem::pausedChanged, this,
                         &VideoSyncController::onReferencePausedChanged);
}

void VideoSyncController::disconnectReference() {
    if (referenceLoadedConn_) QObject::disconnect(referenceLoadedConn_);
    if (referencePausedConn_) QObject::disconnect(referencePausedConn_);
}

void VideoSyncController::connectStore() {
    if (!store_) return;
    storeVideoTimeConn_ =
        QObject::connect(store_, &TelemetryStore::videoTimeChanged, this,
                         &VideoSyncController::onStoreVideoTimeChanged);
    storeSyncStrategyConn_ = QObject::connect(
        store_, &TelemetryStore::comparisonSyncStrategyChanged, this,
        &VideoSyncController::onStoreComparisonSyncStrategyChanged);
    storeLapLoadingConn_ =
        QObject::connect(store_, &TelemetryStore::lapLoadingChanged, this,
                         &VideoSyncController::onStoreLapLoadingChanged);
    storePlaybackEndedConn_ =
        QObject::connect(store_, &TelemetryStore::primaryLapPlaybackEnded, this,
                         &VideoSyncController::onStorePrimaryLapPlaybackEnded);
    storeSelectionConn_ =
        QObject::connect(store_, &TelemetryStore::selectionChanged, this,
                         &VideoSyncController::onStoreSelectionChanged);
}

void VideoSyncController::disconnectStore() {
    if (storeVideoTimeConn_) QObject::disconnect(storeVideoTimeConn_);
    if (storeSyncStrategyConn_) QObject::disconnect(storeSyncStrategyConn_);
    if (storeLapLoadingConn_) QObject::disconnect(storeLapLoadingConn_);
    if (storePlaybackEndedConn_) QObject::disconnect(storePlaybackEndedConn_);
    if (storeSelectionConn_) QObject::disconnect(storeSelectionConn_);
}

void VideoSyncController::updateDualVideo() {
    emit dualVideoChanged();
    updateContinuousSyncTimer();
}

void VideoSyncController::updateContinuousSyncTimer() {
    const bool shouldRun = dualVideo() && primaryPlayer_ &&
                           primaryPlayer_->loaded() &&
                           !primaryPlayer_->paused();
    if (shouldRun == continuousSyncTimer_.isActive()) return;
    if (shouldRun)
        continuousSyncTimer_.start();
    else
        continuousSyncTimer_.stop();
}

// ── core sync logic (ported from Main.qml) ───────────────────────

double VideoSyncController::primaryClockRate() const {
    return (videoSlowMotion_ && videoFullscreen_) ? 0.25 : 1.0;
}

void VideoSyncController::lockPrimaryRealtime() {
    const double rate = primaryClockRate();
    if (primaryPlayer_ && primaryPlayer_->loaded() &&
        !qFuzzyCompare(primaryPlayer_->playbackRate(), rate))
        primaryPlayer_->setPlaybackRate(rate);
}

void VideoSyncController::syncReferenceVideo(bool force) {
    if (!referencePlayer_ || !referencePlayer_->loaded()) {
        setReferenceSyncState(QStringLiteral("WAIT"));
        return;
    }
    if (!store_) return;
    const double target = store_->compareVideoTime();
    if (target <= 0) {
        referencePlayer_->setPlaybackRate(1.0);
        setReferenceSyncState(QStringLiteral("NO MAP"));
        return;
    }

    const double error = target - referencePlayer_->position();
    const double primaryDelta =
        primaryPlayer_ ? primaryPlayer_->position() - referenceSyncLastPrimary_
                       : 0.0;
    referenceSyncError_ = error;
    if (force || (primaryPlayer_ && primaryPlayer_->paused()) ||
        qAbs(primaryDelta) > 0.5) {
        referencePlayer_->setPlaybackRate(
            (primaryPlayer_ && primaryPlayer_->paused()) ? 1.0
                                                         : primaryClockRate());
        referencePlayer_->seek(target);
        referenceSyncError_ = 0;
        referenceSyncBaseRate_ = 1;
        if (primaryPlayer_) {
            referenceSyncLastPrimary_ = primaryPlayer_->position();
        }
        referenceSyncLastTarget_ = target;
        setReferenceSyncState((primaryPlayer_ && primaryPlayer_->paused())
                                  ? QStringLiteral("ALIGNING")
                                  : QStringLiteral("LOCKED"));
        if (primaryPlayer_ && primaryPlayer_->paused())
            pausedAlignmentTimer_.start();
        return;
    }

    if (primaryPlayer_) referenceSyncLastPrimary_ = primaryPlayer_->position();
    referenceSyncLastTarget_ = target;
    const double rate =
        store_->referencePlaybackRate(referencePlayer_->position()) *
        primaryClockRate();
    referenceSyncBaseRate_ = rate;
    referencePlayer_->setPlaybackRate(rate);
    if (store_->cursorInCorner())
        setReferenceSyncState(qAbs(error) < 0.08 ? QStringLiteral("CORNER")
                                                 : QStringLiteral("HOLD"));
    else
        setReferenceSyncState(qAbs(error) < 0.08 ? QStringLiteral("LOCKED")
                                                 : QStringLiteral("STRAIGHT"));
}

void VideoSyncController::realignPausedVideos() {
    if (!dualVideo() || !primaryPlayer_ || !primaryPlayer_->loaded() ||
        !primaryPlayer_->paused() || !referencePlayer_ ||
        !referencePlayer_->loaded())
        return;
    if (!store_) return;

    enqueueTelemetry(true);
    queue_.pump();
    const double target = store_->compareVideoTime();
    referenceSyncPauseAttempts_ = 0;
    if (target <= 0) {
        referencePlayer_->setPlaybackRate(1.0);
        setReferenceSyncState(QStringLiteral("NO MAP"));
        return;
    }

    referencePlayer_->setPaused(true);
    referencePlayer_->setPlaybackRate(1.0);
    referenceSyncBaseRate_ = 1;
    referenceSyncError_ = target - referencePlayer_->position();
    referenceSyncLastPrimary_ = primaryPlayer_->position();
    referenceSyncLastTarget_ = target;
    referenceSyncPauseAttempts_ = 1;
    setReferenceSyncState(QStringLiteral("ALIGNING"));
    referencePlayer_->seek(target);
    pausedAlignmentTimer_.start();
}

void VideoSyncController::verifyPausedVideoAlignment() {
    if (!dualVideo() || !primaryPlayer_ || !primaryPlayer_->paused() ||
        !referencePlayer_ || !referencePlayer_->loaded())
        return;
    if (!store_) return;
    if (referencePlayer_->seeking()) {
        pausedAlignmentTimer_.start();
        return;
    }

    enqueueTelemetry(true);
    queue_.pump();
    const double target = store_->compareVideoTime();
    if (target <= 0) {
        setReferenceSyncState(QStringLiteral("NO MAP"));
        return;
    }
    const double error = target - referencePlayer_->position();
    referenceSyncError_ = error;
    referenceSyncLastPrimary_ = primaryPlayer_->position();
    referenceSyncLastTarget_ = target;
    if (qAbs(error) > 0.025 && referenceSyncPauseAttempts_ < 3) {
        ++referenceSyncPauseAttempts_;
        setReferenceSyncState(QStringLiteral("ALIGNING"));
        referencePlayer_->seek(target);
        pausedAlignmentTimer_.start();
        return;
    }
    referenceSyncPauseAttempts_ = 0;
    setReferenceSyncState(qAbs(error) <= 0.05 ? QStringLiteral("LOCKED")
                                              : QStringLiteral("BEST"));
}

void VideoSyncController::syncReferenceSource() {
    if (!referencePlayer_ || !store_) return;
    const QUrl source = store_->compareVideoSource();
    if (source.toString().isEmpty()) {
        if (!referencePlayer_->source().toString().isEmpty())
            referencePlayer_->closeMedia();
        return;
    }
    if (referencePlayer_->source() != source)
        referencePlayer_->openMedia(source);
    else
        syncReferenceVideo(true);
}

void VideoSyncController::seekToTelemetry() {
    if (!telemetryVideoActive_ || !primaryPlayer_ || !primaryPlayer_->loaded())
        return;
    if (!store_) return;
    lockPrimaryRealtime();
    const double target = store_->primaryVideoTime();
    const double error = std::abs(primaryPlayer_->position() - target);
    if (primaryPlayer_->paused()) {
        if (error > 0.025) primaryPlayer_->seek(target);
        return;
    }
    // Playing: the primary file is the clock. Only honor an explicit cursor
    // jump — never tug the recording back onto a telemetry sample.
    if (error > 0.2) primaryPlayer_->seek(target);
}

// ── user actions ─────────────────────────────────────────────────

void VideoSyncController::togglePaused() {
    if (!primaryPlayer_ || !primaryPlayer_->loaded()) return;
    if (lapAdvanceCount_ > 0) {
        cancelLapAdvance();
        return;
    }
    lockPrimaryRealtime();
    primaryPlayer_->togglePaused();
    if (!primaryPlayer_->paused()) syncReferenceVideo(true);
}

void VideoSyncController::seekRelative(double seconds) {
    if (!primaryPlayer_ || !primaryPlayer_->loaded()) return;
    if (lapAdvanceCount_ > 0) cancelLapAdvance();
    // From the seek in flight, not the last reported frame: a second tap
    // before a multi-gigabyte exact seek lands must still add up.
    const double target =
        std::max(0.0, primaryPlayer_->targetPosition() + seconds);
    primaryPlayer_->seek(target);
    lastPrimaryMediaTime_ = target;
    enqueueTelemetry(true);
    if (dualVideo()) syncReferenceVideo(true);
}

void VideoSyncController::toggleSlowMotion() {
    if (!videoFullscreen_ || !primaryPlayer_ || !primaryPlayer_->loaded())
        return;
    videoSlowMotion_ = !videoSlowMotion_;
    lockPrimaryRealtime();
    if (dualVideo() && primaryPlayer_ && !primaryPlayer_->paused())
        syncReferenceVideo(false);
    emit videoSlowMotionChanged();
}

void VideoSyncController::reopenExpiredVideo(MpvVideoItem* player) {
    if (!player || !store_) return;
    const QUrl fresh = store_->refreshedVideoSource(player->source());
    if (!fresh.toString().isEmpty()) player->reopenMedia(fresh);
}

// ── lap advance countdown ────────────────────────────────────────

void VideoSyncController::cancelLapAdvance() {
    lapAdvanceTimer_.stop();
    setLapAdvanceCount(0);
    lapAdvanceNextId_ = -1;
    lapAdvanceNextLabel_ = QString();
    lapAdvanceResume_ = false;
}

void VideoSyncController::startLapAdvance() {
    if (lapAdvanceCount_ > 0) return;
    if (!store_ || !primaryPlayer_) return;
    const int nextId = store_->nextPrimaryLapId();
    primaryPlayer_->setPaused(true);
    if (nextId < 0) return;
    lapAdvanceNextId_ = nextId;
    lapAdvanceNextLabel_ =
        store_->lapLabel(store_->primarySessionKey(), nextId);
    setLapAdvanceCount(3);
    store_->prefetchLap(store_->primarySessionKey(), nextId);
    lapAdvanceTimer_.start();
}

void VideoSyncController::tickLapAdvance() {
    if (lapAdvanceCount_ <= 1) {
        finishLapAdvance();
        return;
    }
    setLapAdvanceCount(lapAdvanceCount_ - 1);
}

void VideoSyncController::finishLapAdvance() {
    if (!store_) return;
    const QString sessionKey = store_->primarySessionKey();
    const int nextId = lapAdvanceNextId_;
    cancelLapAdvance();
    if (sessionKey.isEmpty() || nextId < 0) return;
    store_->selectLap(sessionKey, nextId);
    lapAdvanceResume_ = true;
    tryResumeLapAdvance();
}

void VideoSyncController::tryResumeLapAdvance() {
    if (!lapAdvanceResume_) return;
    if (!store_ || store_->lapLoading()) return;
    if (primaryPlayer_ && primaryPlayer_->seeking()) return;
    lapAdvanceResume_ = false;
    if (!primaryPlayer_ || !primaryPlayer_->loaded()) return;
    lockPrimaryRealtime();
    primaryPlayer_->setPaused(false);
    if (dualVideo()) syncReferenceVideo(true);
}

// ── signal handlers ──────────────────────────────────────────────

void VideoSyncController::onPrimaryLoadedChanged() {
    if (!primaryPlayer_ || !primaryPlayer_->loaded() || !telemetryVideoActive_)
        return;
    // Qt.callLater equivalent: defer until the next event-loop turn so the
    // player has settled its loaded state before we seek.
    QMetaObject::invokeMethod(
        this,
        [this]() {
            lockPrimaryRealtime();
            if (primaryPlayer_) primaryPlayer_->setPaused(true);
            seekToTelemetry();
        },
        Qt::QueuedConnection);
}

void VideoSyncController::onPrimaryPositionChanged() {
    if (!primaryPlayer_ || !primaryPlayer_->loaded()) return;
    lastPrimaryMediaTime_ = primaryPlayer_->position();
    // Store-only plus a coalesced upsert. The pump (QTimer / afterAnimating)
    // runs the work; this must not call setCursorFromVideoTime.
    if (telemetryVideoActive_ && !primaryPlayer_->paused() &&
        !primaryPlayer_->seeking())
        enqueueTelemetry(false);
    else if (dualVideo() && primaryPlayer_->paused() &&
             referenceSyncPauseAttempts_ > 0)
        pausedAlignmentTimer_.start();
}

void VideoSyncController::onPrimarySeekingChanged() {
    if (!primaryPlayer_) return;
    if (primaryPlayer_->seeking()) enqueueTelemetry(true);
    if (!primaryPlayer_->seeking()) tryResumeLapAdvance();
}

void VideoSyncController::onPrimaryPausedChanged() {
    if (!primaryPlayer_) return;
    enqueueTelemetry(true);
    if (!referencePlayer_ || !referencePlayer_->loaded()) return;
    if (referencePlayer_->paused() != primaryPlayer_->paused())
        referencePlayer_->setPaused(primaryPlayer_->paused());
    if (primaryPlayer_->paused()) {
        realignPausedVideos();
    } else {
        pausedAlignmentTimer_.stop();
        referenceSyncPauseAttempts_ = 0;
        syncReferenceVideo(true);
    }
    updateContinuousSyncTimer();
}

void VideoSyncController::onReferenceLoadedChanged() {
    if (!referencePlayer_ || !referencePlayer_->loaded()) return;
    QMetaObject::invokeMethod(
        this,
        [this]() {
            if (!referencePlayer_ || !primaryPlayer_) return;
            referencePlayer_->setPaused(primaryPlayer_->paused());
            if (primaryPlayer_->paused())
                realignPausedVideos();
            else
                syncReferenceVideo(true);
        },
        Qt::QueuedConnection);
}

void VideoSyncController::onReferencePausedChanged() {
    if (!referencePlayer_ || !primaryPlayer_) return;
    if (referencePlayer_->paused() != primaryPlayer_->paused())
        referencePlayer_->setPaused(primaryPlayer_->paused());
}

void VideoSyncController::onStoreVideoTimeChanged() {
    if (!primaryPlayer_ || !store_) return;
    const bool jumped =
        std::abs(primaryPlayer_->position() - store_->primaryVideoTime()) > 0.2;
    seekToTelemetry();
    syncReferenceVideo(primaryPlayer_->paused() || jumped);
}

void VideoSyncController::onStoreComparisonSyncStrategyChanged() {
    if (dualVideo()) syncReferenceVideo(true);
}

void VideoSyncController::onStoreLapLoadingChanged() { tryResumeLapAdvance(); }

void VideoSyncController::onStorePrimaryLapPlaybackEnded() {
    startLapAdvance();
}

void VideoSyncController::onStoreSelectionChanged() {
    if (lapAdvanceCount_ > 0) cancelLapAdvance();
    updateDualVideo();
}

void VideoSyncController::onContinuousSyncTick() { syncReferenceVideo(false); }

void VideoSyncController::onPausedAlignmentTick() {
    verifyPausedVideoAlignment();
}

void VideoSyncController::onLapAdvanceTick() { tickLapAdvance(); }

// ── helpers ──────────────────────────────────────────────────────

void VideoSyncController::setReferenceSyncState(const QString& state) {
    if (referenceSyncState_ == state) return;
    referenceSyncState_ = state;
    emit referenceSyncStateChanged();
}

void VideoSyncController::setLapAdvanceCount(int count) {
    if (lapAdvanceCount_ == count) return;
    lapAdvanceCount_ = count;
    emit lapAdvanceCountChanged();
}

double VideoSyncController::sampledPlayerTime() const {
    if (!primaryPlayer_) return lastPrimaryMediaTime_;
    return primaryPlayer_->targetPosition();
}

void VideoSyncController::applySampledCursor() {
    if (!primaryPlayer_ || !primaryPlayer_->loaded()) return;
    const double t = sampledPlayerTime();
    lastPrimaryMediaTime_ = t;
    if (!qFuzzyCompare(sampledMediaTime_ + 1.0, t + 1.0)) {
        sampledMediaTime_ = t;
        emit sampledMediaTimeChanged();
    }
    if (telemetryVideoActive_ && store_) store_->setCursorFromVideoTime(t);
}

void VideoSyncController::enqueueTelemetry(bool immediate) {
    if (!telemetryVideoActive_ || !store_) return;
    queue_.upsert(QStringLiteral("cursor"), DeadlineQueue::Priority::High,
                  deadlineFrom(immediate, std::chrono::milliseconds(16)),
                  [this]() { applySampledCursor(); });
    queue_.upsert(QStringLiteral("readout"), DeadlineQueue::Priority::Normal,
                  deadlineFrom(immediate, std::chrono::milliseconds(40)),
                  [this]() {
                      if (store_) store_->notifyCursorReadout();
                  });
    queue_.upsert(QStringLiteral("channels"), DeadlineQueue::Priority::Low,
                  deadlineFrom(immediate, std::chrono::milliseconds(50)),
                  [this]() {
                      if (store_) store_->notifyChannelsCursorTick();
                  });
}

void VideoSyncController::attachDisplayPump() {
    QQuickWindow* window = primaryPlayer_ ? primaryPlayer_->window() : nullptr;
    if (window == displayWindow_) return;
    if (displayTickConn_) QObject::disconnect(displayTickConn_);
    displayWindow_ = window;
    if (!displayWindow_) return;
    displayTickConn_ =
        QObject::connect(displayWindow_, &QQuickWindow::afterAnimating, this,
                         &VideoSyncController::onDisplayTick);
}

void VideoSyncController::onDisplayTick() {
    if (telemetryVideoActive_ && primaryPlayer_ && primaryPlayer_->loaded() &&
        !primaryPlayer_->paused() && !primaryPlayer_->seeking()) {
        lastPrimaryMediaTime_ = primaryPlayer_->position();
        enqueueTelemetry(false);
    }
    queue_.pump();
}
