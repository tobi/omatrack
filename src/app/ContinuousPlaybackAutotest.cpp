// Continuous playback: the recording plays through a lap end without the
// next-lap countdown, the next lap is adopted without seeking the video, and
// the trace viewport keeps the playhead at a third of its width.
#include "AutotestHarness.h"
#include "MpvVideoItem.h"
#include "TelemetryStore.h"
#include "VideoSyncController.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QTimer>

#include <cmath>
#include <memory>

namespace {
struct Check {
    QElapsedTimer elapsed;
    QElapsedTimer phaseClock;
    int phase = 0;
    int startLap = -1;
    int nextLap = -1;
    double lastPosition = -1.0;
};

bool anchored(const TelemetryStore& store) {
    const double span = store.viewEnd() - store.viewStart();
    const double expected =
        store.cursorFrac() -
        VideoSyncController::kContinuousPlayheadAnchor * span;
    // One sampled frame of slack between cursor and viewport.
    return span > 0 && std::abs(store.viewStart() - expected) < 0.02 * span;
}
}  // namespace

bool omatrack::autotest::installContinuousPlayback(
    QQmlApplicationEngine& engine, TelemetryStore& store) {
    const QString source = qEnvironmentVariable("OMATRACK_AUTOTEST_CONTINUOUS");
    if (source.isEmpty()) return false;
    const QString reference =
        qEnvironmentVariable("OMATRACK_AUTOTEST_CONTINUOUS_REFERENCE");
    const QString shot = qEnvironmentVariable("OMATRACK_AUTOTEST");
    auto state = std::make_shared<Check>();
    state->elapsed.start();
    auto* timer = new QTimer(&engine);
    timer->setInterval(50);
    QObject::connect(
        timer, &QTimer::timeout, &engine,
        [&engine, &store, state, timer, source, reference, shot] {
            const auto require = [timer](bool ok, const char* why) {
                if (!ok) {
                    timer->stop();
                    qWarning() << "AUTOTEST continuous playback FAILED:" << why;
                    QCoreApplication::exit(1);
                }
                return ok;
            };
            if (!require(state->elapsed.elapsed() < 90000, "timeout")) return;
            if (engine.rootObjects().isEmpty()) return;
            auto* window =
                qobject_cast<QQuickWindow*>(engine.rootObjects().first());
            if (!require(window, "no window")) return;
            auto* player =
                window->findChild<MpvVideoItem*>(QStringLiteral("videoPlayer"));
            if (state->phase == 0) {
                store.openFile(source);
                state->phase = 1;
                return;
            }
            if (store.loading() || store.lapLoading()) return;
            if (state->phase == 1) {
                if (!store.primarySession() || !store.primaryUnified() ||
                    !player || !player->loaded())
                    return;
                if (!reference.isEmpty()) {
                    store.openFile(reference);
                    state->phase = 2;
                } else {
                    state->phase = 3;
                }
            } else if (state->phase == 2) {
                auto* other = window->findChild<MpvVideoItem*>(
                    QStringLiteral("videoPlayerReference"));
                if (store.compareSessionKey() != reference ||
                    !store.compareUnified() || !other || !other->loaded())
                    return;
                store.setReferencePlayback(QStringLiteral("recording"));
                state->phase = 3;
            } else if (state->phase == 3) {
                store.setContinuousPlayback(true);
                if (!require(store.nextPrimaryLapId() >= 0,
                             "selected lap has no next lap"))
                    return;
                state->startLap = store.primaryLapIndex();
                state->nextLap = store.nextPrimaryLapId();
                store.jumpToFraction(0.97);
                state->phaseClock.start();
                state->phase = 4;
            } else if (state->phase == 4) {
                if (player->seeking() || state->phaseClock.elapsed() < 800)
                    return;
                player->setPaused(false);
                state->lastPosition = player->position();
                state->phaseClock.restart();
                state->phase = 5;
            } else if (state->phase == 5) {
                // Through the crossing: never paused, never sought backwards.
                if (!require(!player->paused(), "paused at the lap end"))
                    return;
                if (!require(player->position() + 0.05 >= state->lastPosition,
                             "video sought backwards"))
                    return;
                state->lastPosition = player->position();
                if (store.primaryLapIndex() == state->startLap) {
                    if (!require(state->phaseClock.elapsed() < 20000,
                                 "lap never advanced"))
                        return;
                    if (store.cursorFrac() < 0.999 &&
                        !require(anchored(store), "playhead not anchored"))
                        return;
                    return;
                }
                if (!require(store.primaryLapIndex() == state->nextLap,
                             "advanced to the wrong lap"))
                    return;
                if (!require(store.cursorFrac() < 0.2,
                             "cursor not at the new lap start"))
                    return;
                state->phaseClock.restart();
                state->phase = 6;
            } else if (state->phase == 6) {
                if (!require(!player->paused() && player->position() + 0.05 >=
                                                      state->lastPosition,
                             "playback interrupted after the crossing"))
                    return;
                state->lastPosition = player->position();
                if (state->phaseClock.elapsed() < 1500) return;
                if (!require(anchored(store),
                             "playhead not anchored after "
                             "the crossing"))
                    return;
                if (!reference.isEmpty()) {
                    auto* other = window->findChild<MpvVideoItem*>(
                        QStringLiteral("videoPlayerReference"));
                    if (!require(
                            other && !other->paused() &&
                                std::abs(other->playbackRate() - 1.0) < 1e-6,
                            "reference not at recording speed"))
                        return;
                }
                if (!shot.isEmpty() && !window->grabWindow().save(shot)) {
                    require(false, "screenshot");
                    return;
                }
                QMetaObject::invokeMethod(window, "videoSetFullscreen",
                                          Q_ARG(QVariant, true));
                state->phaseClock.restart();
                state->phase = 7;
            } else if (state->phase == 7) {
                if (state->phaseClock.elapsed() < 1500) return;
                if (!shot.isEmpty() &&
                    !window->grabWindow().save(shot + "_fullscreen.png")) {
                    require(false, "fullscreen screenshot");
                    return;
                }
                player->setPaused(true);
                timer->stop();
                qWarning() << "AUTOTEST continuous playback: lap"
                           << state->startLap << "->" << state->nextLap
                           << "without pause or seek, playhead anchored PASS";
                QCoreApplication::exit(0);
            }
        });
    timer->start();
    return true;
}
