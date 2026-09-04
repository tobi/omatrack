// Header, filmstrip bookends and persistent fullscreen HUD placement.
#include "AutotestHarness.h"
#include "TelemetryStore.h"
#include "MpvVideoItem.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFont>
#include <QImage>
#include <QMouseEvent>
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTimer>

#include <cmath>
#include <memory>

namespace {
QQuickItem* visualItem(QQuickItem* root, const QString& name) {
    if (root->objectName() == name) return root;
    for (auto* child : root->childItems())
        if (auto* found = visualItem(child, name)) return found;
    return nullptr;
}
bool near(double a, double b) { return std::abs(a - b) < 0.01; }
struct Check {
    QElapsedTimer elapsed;
    int phase = 0;
    int reportedPhase = -1;
    int primaryLap = -1;
    int referenceLap = -1;
    QString primaryKey;
    bool video = false;
};
}  // namespace

bool omatrack::autotest::installSessionChrome(QQmlApplicationEngine& engine,
                                              TelemetryStore& store) {
    const QString source =
        qEnvironmentVariable("OMATRACK_AUTOTEST_SESSION_CHROME");
    if (source.isEmpty()) return false;
    const QString reference =
        qEnvironmentVariable("OMATRACK_AUTOTEST_CHROME_REFERENCE");
    const QString shot = qEnvironmentVariable("OMATRACK_AUTOTEST");
    auto state = std::make_shared<Check>();
    state->elapsed.start();
    auto* timer = new QTimer(&engine);
    timer->setInterval(150);
    QObject::connect(
        timer, &QTimer::timeout, &engine,
        [&engine, &store, state, timer, source, reference, shot] {
            const auto require = [timer](bool ok, const char* why) {
                if (!ok) {
                    timer->stop();
                    qWarning() << "AUTOTEST session chrome FAILED:" << why;
                    QCoreApplication::exit(1);
                }
                return ok;
            };
            if (!require(state->elapsed.elapsed() < 60000, "timeout")) return;
            if (engine.rootObjects().isEmpty() || store.loading() ||
                store.lapLoading())
                return;
            auto* window =
                qobject_cast<QQuickWindow*>(engine.rootObjects().first());
            if (!require(window, "no window")) return;
            auto* content = window->contentItem();
            if (state->phase != state->reportedPhase) {
                state->reportedPhase = state->phase;
                auto* player = window->findChild<MpvVideoItem*>(
                    QStringLiteral("videoPlayer"));
                qWarning() << "AUTOTEST chrome phase" << state->phase
                           << "exposed" << window->isExposed()
                           << "video visible"
                           << window->property("videoVisible").toBool()
                           << "ready/loaded" << (player && player->ready())
                           << (player && player->loaded());
            }
            if (state->phase == 0) {
                store.openFile(source);
                state->phase = 1;
            } else if (state->phase == 1) {
                if (!store.primarySession() || !store.primaryUnified()) return;
                state->primaryKey = store.primarySessionKey();
                state->primaryLap = store.primaryLapIndex();
                state->video = store.primarySession()->isVideo();
                if (!reference.isEmpty()) {
                    store.openFile(reference);
                    state->phase = 2;
                } else {
                    for (const auto& lap : store.primarySession()->laps()) {
                        if (lap.countsForBest() &&
                            lap.lapId != state->primaryLap) {
                            state->referenceLap = lap.lapId;
                            break;
                        }
                    }
                    if (!require(state->referenceLap >= 0,
                                 "need two timed laps"))
                        return;
                    store.compareLap(state->primaryKey, state->referenceLap);
                    state->phase = 4;
                }
            } else if (state->phase == 2) {
                if (!store.primaryUnified()) return;
                // Additional videos are automatically opened as the reference;
                // ordinary telemetry files replace primary until assigned.
                if (store.primarySessionKey() == state->primaryKey) {
                    if (store.compareSessionKey() == reference &&
                        store.compareUnified())
                        state->phase = 4;
                    return;
                }
                store.compareLap(state->primaryKey, state->primaryLap);
                state->phase = 3;
            } else if (state->phase == 3) {
                if (!store.comparing() || !store.compareUnified()) return;
                store.swapPrimaryWithReference();
                state->phase = 4;
            } else if (state->phase == 4) {
                if (state->video) {
                    auto* player = window->findChild<MpvVideoItem*>(
                        QStringLiteral("videoPlayer"));
                    if (!player || !player->loaded()) return;
                    if (!reference.isEmpty()) {
                        auto* other = window->findChild<MpvVideoItem*>(
                            QStringLiteral("videoPlayerReference"));
                        if (!other || !other->loaded()) return;
                    }
                }
                auto* track = window->findChild<QQuickItem*>(
                    QStringLiteral("headerTrackName"));
                auto* event = window->findChild<QQuickItem*>(
                    QStringLiteral("headerEventMode"));
                auto* configure = window->findChild<QQuickItem*>(
                    QStringLiteral("headerEventSettings"));
                if (!require(
                        track && event && configure &&
                            track->property("font")
                                    .value<QFont>()
                                    .pixelSize() >= 18 &&
                            event->mapToScene({}).y() >=
                                track->mapToScene({}).y() + track->height() - 1,
                        "track/event hierarchy"))
                    return;
                if (!require(!window->findChild<QObject*>(
                                 QStringLiteral("headerDriverName")) &&
                                 !window->findChild<QObject*>(
                                     QStringLiteral("headerCompare")),
                             "duplicate header details"))
                    return;
                QPointF primaryStart, primaryEnd;
                for (bool compare : {false, true}) {
                    const auto* session = compare ? store.compareSession()
                                                  : store.primarySession();
                    if (!require(session && session->laps().size() >= 2,
                                 "lap rows"))
                        return;
                    const auto& laps = session->laps();
                    const QString prefix =
                        compare ? QStringLiteral("referenceFilmstripLap-")
                                : QStringLiteral("activeFilmstripLap-");
                    auto* first = visualItem(
                        content, prefix + QString::number(laps.front().lapId));
                    auto* last = visualItem(
                        content, prefix + QString::number(laps.back().lapId));
                    if (!require(first && last, "bookend cells")) return;
                    if (!laps.front().countsForBest() &&
                        !require(near(first->width(), 72), "out width"))
                        return;
                    if (!laps.back().countsForBest() &&
                        !require(near(last->width(), 72), "in width"))
                        return;
                    const QPointF left = first->mapToScene({});
                    const QPointF right =
                        last->mapToScene(QPointF(last->width(), 0));
                    if (compare) {
                        if (!require(near(left.x(), primaryStart.x()) &&
                                         near(right.x(), primaryEnd.x()),
                                     "row edges do not align"))
                            return;
                    } else {
                        primaryStart = left;
                        primaryEnd = right;
                    }
                }
                if (!require(window->grabWindow().save(shot),
                             "desktop screenshot"))
                    return;
                qWarning() << "AUTOTEST session chrome: header and aligned "
                              "fixed bookends PASS";
                if (!state->video) {
                    timer->stop();
                    QCoreApplication::exit(0);
                    return;
                }
                store.setCursorFrac(0.35);
                QMetaObject::invokeMethod(window, "videoSetFullscreen",
                                          Q_ARG(QVariant, true));
                state->phase = 5;
            } else if (state->phase == 5) {
                auto* player = window->findChild<MpvVideoItem*>(
                    QStringLiteral("videoPlayer"));
                if (player && player->seeking()) return;
                auto* hud = window->findChild<QQuickItem*>(
                    QStringLiteral("videoTelemetryOverlay"));
                auto* active = window->findChild<QQuickItem*>(
                    QStringLiteral("activeVideoContext"));
                auto* ref = window->findChild<QQuickItem*>(
                    QStringLiteral("referenceVideoContext"));
                if (!require(hud && active && ref && active->isVisible() &&
                                 ref->isVisible() &&
                                 active->mapToScene({}).y() > 28 &&
                                 near(active->mapToScene({}).y(),
                                      ref->mapToScene({}).y()),
                             "fullscreen role labels"))
                    return;
                if (qEnvironmentVariableIsSet(
                        "OMATRACK_AUTOTEST_CHROME_RESTORE")) {
                    if (!require(near(store.videoHudPosition().x(), 0.18) &&
                                     near(store.videoHudPosition().y(), 0.42),
                                 "HUD position not restored"))
                        return;
                } else {
                    if (!require(window->grabWindow().save(
                                     shot +
                                     QStringLiteral("_fullscreen-default.png")),
                                 "default HUD screenshot"))
                        return;
                    const double x =
                        hud->property("availableX").toDouble() * 0.18;
                    const double y =
                        hud->property("availableY").toDouble() * 0.42;
                    const QPointF from = hud->mapToScene(
                        QPointF(hud->width() / 2, hud->height() / 2));
                    const QPointF to =
                        from + QPointF(x - hud->x(), y - hud->y());
                    QMouseEvent press(QEvent::MouseButtonPress, from, from,
                                      from, Qt::LeftButton, Qt::LeftButton,
                                      Qt::NoModifier);
                    QMouseEvent firstMove(QEvent::MouseMove,
                                          from + QPointF(18, 18),
                                          from + QPointF(18, 18), Qt::NoButton,
                                          Qt::LeftButton, Qt::NoModifier);
                    QMouseEvent move(QEvent::MouseMove, to, to, Qt::NoButton,
                                     Qt::LeftButton, Qt::NoModifier);
                    QMouseEvent release(QEvent::MouseButtonRelease, to, to, to,
                                        Qt::LeftButton, Qt::NoButton,
                                        Qt::NoModifier);
                    QCoreApplication::sendEvent(window, &press);
                    QCoreApplication::sendEvent(window, &firstMove);
                    QCoreApplication::sendEvent(window, &move);
                    QCoreApplication::sendEvent(window, &release);
                }
                state->phase = 6;
            } else {
                auto* hud = window->findChild<QQuickItem*>(
                    QStringLiteral("videoTelemetryOverlay"));
                if (!require(
                        hud &&
                            near(hud->x(),
                                 hud->property("availableX").toDouble() *
                                     0.18) &&
                            near(hud->y(),
                                 hud->property("availableY").toDouble() * 0.42),
                        "HUD placement"))
                    return;
                if (state->phase == 6) {
                    window->resize(window->width() - 120,
                                   window->height() - 60);
                    state->phase = 7;
                    return;
                }
                if (!require(window->grabWindow().save(
                                 shot + QStringLiteral("_fullscreen.png")),
                             "fullscreen screenshot"))
                    return;
                qWarning() << "AUTOTEST session chrome: fullscreen context and "
                              "HUD placement PASS";
                timer->stop();
                QCoreApplication::exit(0);
            }
        });
    timer->start();
    return true;
}
