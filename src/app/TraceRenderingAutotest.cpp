// Native frame-paced trace acceptance; excluded from production builds.
#include "AutotestHarness.h"
#include "TelemetryStore.h"
#include "TraceView.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QImage>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <memory>

namespace {
struct Sweep {
    QElapsedTimer timeout;
    QElapsedTimer frameClock;
    QVector<double> intervals;
    QMetaObject::Connection connection;
    int phase = 0;
    int frame = 0;
    int skip = 0;
    bool saved = true;
    bool finished = false;
};
}  // namespace

bool omatrack::autotest::installTraceRendering(QQmlApplicationEngine& engine,
                                               TelemetryStore& store) {
    const QString source =
        qEnvironmentVariable("OMATRACK_AUTOTEST_TRACE_RENDERING");
    if (source.isEmpty()) return false;
    const QString shot = qEnvironmentVariable("OMATRACK_AUTOTEST");
    const QFileInfo file(shot);
    const QString prefix =
        file.absolutePath() + QLatin1Char('/') + file.completeBaseName();
    auto state = std::make_shared<Sweep>();
    state->timeout.start();
    auto* timer = new QTimer(&engine);
    timer->setInterval(100);
    QObject::connect(
        timer, &QTimer::timeout, &engine,
        [&engine, &store, timer, state, source, shot, prefix] {
            if (state->timeout.elapsed() > 60000) {
                qWarning() << "AUTOTEST traces timeout";
                QCoreApplication::exit(1);
                return;
            }
            if (engine.rootObjects().isEmpty() || store.loading() ||
                store.lapLoading())
                return;
            auto* window =
                qobject_cast<QQuickWindow*>(engine.rootObjects().first());
            if (!window) return;
            if (state->phase == 0) {
                if (qEnvironmentVariableIsSet(
                        "OMATRACK_AUTOTEST_TRACE_RESTORE")) {
                    const auto style =
                        store.channelAppearance(QStringLiteral("throttle"));
                    const bool restored = style.strokeWidth == 0.75 &&
                                          style.fillOpacity == 0.55 &&
                                          style.referenceColor ==
                                              QColor(QStringLiteral("#7fbbb3"));
                    qWarning() << "AUTOTEST traces restored style:" << restored;
                    if (!restored) {
                        QCoreApplication::exit(1);
                        return;
                    }
                }
                store.openFile(source);
                state->phase = 1;
                return;
            }
            if (state->phase == 1) {
                if (!store.primarySession() || !store.primaryUnified()) return;
                for (const auto& lap : store.primarySession()->laps()) {
                    if (lap.countsForBest() &&
                        lap.lapId != store.primaryLapIndex()) {
                        store.compareLap(store.primarySessionKey(), lap.lapId);
                        state->phase = 2;
                        return;
                    }
                }
                qWarning() << "AUTOTEST traces needs two complete laps";
                QCoreApplication::exit(1);
                return;
            }
            if (!store.comparing() || !store.compareUnified()) return;
            timer->stop();
            store.resetView();
            store.setChannelVisible(QStringLiteral("delta"), true);
            state->frameClock.start();
            state->connection = QObject::connect(
                window, &QQuickWindow::frameSwapped, &engine,
                [&engine, &store, window, state, shot, prefix] {
                    if (state->finished) return;
                    const double interval =
                        state->frameClock.nsecsElapsed() / 1e6;
                    state->frameClock.restart();
                    const int frame = state->frame++;
                    if (frame > 20 && state->skip == 0)
                        state->intervals.append(interval);
                    if (state->skip > 0) --state->skip;
                    if (frame == 10 || frame == 40 || frame == 80) {
                        const QString path =
                            frame == 10
                                ? shot
                                : prefix +
                                      (frame == 40
                                           ? QStringLiteral("_zoom.png")
                                           : QStringLiteral("_detail.png"));
                        state->saved &= window->grabWindow().save(path);
                        state->skip =
                            5;  // screenshot readback/PNG is not a zoom frame
                        state->frameClock.restart();
                    }
                    if (frame < 340) {
                        const double phase =
                            std::clamp((frame - 20) / 320.0, 0.0, 1.0);
                        const double span =
                            std::pow(10.0, -4.0 * std::sin(M_PI * phase));
                        store.zoomAt(0.42, span / store.viewSpan());
                        window->requestUpdate();
                        return;
                    }
                    state->finished = true;
                    QObject::disconnect(state->connection);
                    std::sort(state->intervals.begin(), state->intervals.end());
                    const auto percentile = [&](double p) {
                        return state
                            ->intervals[int(p * (state->intervals.size() - 1))];
                    };
                    qWarning() << "AUTOTEST traces frame_callback_ms p50:"
                               << percentile(0.5) << "p95:" << percentile(0.95)
                               << "p99:" << percentile(0.99)
                               << "frames:" << state->intervals.size()
                               << "dpr:" << window->effectiveDevicePixelRatio();
                    store.setChannelAppearance(QStringLiteral("throttle"), 0.75,
                                               0.55, QStringLiteral("#7fbbb3"));
                    const auto style =
                        store.channelAppearance(QStringLiteral("throttle"));
                    state->saved &=
                        style.strokeWidth == 0.75 && style.fillOpacity == 0.55;
                    store.resetView();
                    auto* channels = window->findChild<QQuickWindow*>(
                        QStringLiteral("channelsWindow"));
                    if (!channels) {
                        QCoreApplication::exit(1);
                        return;
                    }
                    channels->setProperty("appearanceKey",
                                          QStringLiteral("throttle"));
                    channels->show();
                    QTimer::singleShot(
                        350, &engine, [channels, window, state, prefix] {
                            state->saved &= window->grabWindow().save(
                                prefix + QStringLiteral("_styled.png"));
                            state->saved &= channels->grabWindow().save(
                                prefix + QStringLiteral("_channels.png"));
                            qWarning() << "AUTOTEST traces captured and styled:"
                                       << state->saved;
                            QCoreApplication::exit(state->saved ? 0 : 1);
                        });
                },
                Qt::QueuedConnection);
            window->requestUpdate();
        });
    timer->start();
    QTimer::singleShot(60000, &engine, [state] {
        if (!state->finished) {
            qWarning() << "AUTOTEST traces: no frame callbacks (is the "
                          "headless workspace active?)";
            QCoreApplication::exit(1);
        }
    });
    return true;
}
