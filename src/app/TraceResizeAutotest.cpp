// Native resize-mode acceptance, on a copied recording and scratch config.
#include "AutotestHarness.h"
#include "TelemetryStore.h"
#include "TraceView.h"
#include "TraceLaneSizing.h"
#include "YamlConfig.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QKeyEvent>
#include <QKeySequence>
#include <QMouseEvent>
#include <QPointer>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QTimer>

#include <cmath>
#include <memory>

namespace {
struct Check {
    QElapsedTimer elapsed;
    int phase = 0;
    QString rawKey;
    QList<TraceLaneRow> original;
    QPointer<QQuickItem> label;
    double cursor = 0, start = 0, end = 1;
};
QList<TraceLaneRow> samples(TraceView* trace) {
    QList<TraceLaneRow> rows;
    for (const auto& row : trace->laneRows())
        if (row.kind == QStringLiteral("sample")) rows.append(row);
    return rows;
}
QQuickItem* visualItem(QQuickItem* root, const QString& name) {
    if (root->objectName() == name) return root;
    for (auto* child : root->childItems())
        if (auto* found = visualItem(child, name)) return found;
    return nullptr;
}
bool near(double a, double b) { return std::abs(a - b) < 1e-6; }
bool click(QObject* root, const char* name) {
    auto* item = root->findChild<QQuickItem*>(QLatin1String(name));
    if (!item || !item->isEnabled()) return false;
    const QPointF p(item->width() / 2, item->height() / 2);
    QMouseEvent press(QEvent::MouseButtonPress, p, p, p, Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, p, p, p, Qt::LeftButton,
                        Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(item, &press);
    QCoreApplication::sendEvent(item, &release);
    return true;
}
void drag(TraceView* trace, double y, double delta) {
    const QPointF from(trace->labelWidth() + 150, y), to(from.x(), y + delta);
    QMouseEvent press(QEvent::MouseButtonPress, from, from, from,
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent move(QEvent::MouseMove, to, to, to, Qt::NoButton,
                     Qt::LeftButton, Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, to, to, to, Qt::LeftButton,
                        Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(trace, &press);
    // A raw-channel load can finish during a drag. Its data notification
    // must not discard the grab when the lane identities are unchanged.
    QMetaObject::invokeMethod(trace->store(), "channelConfigChanged",
                              Qt::DirectConnection);
    QCoreApplication::sendEvent(trace, &move);
    QCoreApplication::sendEvent(trace, &release);
}
bool fits(const QList<TraceLaneRow>& rows,
          const QList<TraceLaneRow>& original) {
    if (rows.size() != original.size()) return false;
    double before = 0, after = 0;
    for (int i = 0; i < rows.size(); ++i) {
        if (rows[i].key != original[i].key || rows[i].height < 19.99)
            return false;
        before += original[i].height;
        after += rows[i].height;
    }
    return near(before, after);
}
}  // namespace

bool omatrack::autotest::installTraceResize(QQmlApplicationEngine& engine,
                                            TelemetryStore& store) {
    const QString source =
        qEnvironmentVariable("OMATRACK_AUTOTEST_TRACE_RESIZE");
    if (source.isEmpty()) return false;
    const QString shot = qEnvironmentVariable("OMATRACK_AUTOTEST");
    auto state = std::make_shared<Check>();
    state->elapsed.start();
    auto* timer = new QTimer(&engine);
    timer->setInterval(100);
    QObject::connect(
        timer, &QTimer::timeout, &engine,
        [&engine, &store, state, timer, source, shot] {
            const auto require = [timer](bool ok, const char* why) {
                if (!ok) {
                    timer->stop();
                    qWarning() << "AUTOTEST trace resize FAILED:" << why;
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
            auto* trace =
                window
                    ? window->findChild<TraceView*>(QStringLiteral("traceView"))
                    : nullptr;
            if (!require(window && trace, "no trace view")) return;
            if (state->phase == 0) {
                store.openFile(source);
                state->phase = 1;
            } else if (state->phase == 1) {
                if (!store.primarySession() || !store.primaryUnified()) return;
                const auto channels = store.primarySession()->sourceChannels();
                if (!require(!channels.isEmpty(), "need source channels"))
                    return;
                state->rawKey = QStringLiteral("raw:") + channels.front().name;
                store.setChannelVisible(state->rawKey, true);
                if (qEnvironmentVariableIsSet(
                        "OMATRACK_AUTOTEST_TRACE_RESIZE_RESTORE")) {
                    if (!require(store.channelWeight(state->rawKey) > 2.0,
                                 "raw height not restored"))
                        return;
                    qWarning() << "AUTOTEST trace resize restored raw weight:"
                               << store.channelWeight(state->rawKey);
                    timer->stop();
                    QCoreApplication::exit(0);
                    return;
                }
                state->cursor = store.cursorFrac();
                state->start = store.viewStart();
                state->end = store.viewEnd();
                state->phase = 2;
            } else if (state->phase == 2) {
                state->original = samples(trace);
                state->label = visualItem(window->contentItem(),
                                          QStringLiteral("traceLane-speed"));
                if (!require(state->original.size() >= 4 &&
                                 state->original.back().key == state->rawKey,
                             "need standard and raw lanes"))
                    return;
                if (!require(click(window, "resizeTracesButton") &&
                                 store.resizingTraces(),
                             "toolbar entry"))
                    return;
                const auto& lane = state->original.front();
                drag(trace, lane.y + lane.height, trace->height() * 0.55);
                state->phase = 3;
            } else if (state->phase == 3) {
                const auto rows = samples(trace);
                if (!require(
                        state->label &&
                            state->label ==
                                visualItem(window->contentItem(),
                                           QStringLiteral("traceLane-speed")),
                        "lane delegates rebuilt during drag"))
                    return;
                if (!require(rows.front().height >
                                     state->original.front().height * 2.0 &&
                                 fits(rows, state->original),
                             "grow beyond 2x and keep all lanes fitted"))
                    return;
                if (!require(near(store.cursorFrac(), state->cursor) &&
                                 near(store.viewStart(), state->start) &&
                                 near(store.viewEnd(), state->end),
                             "resize moved the playhead/viewport"))
                    return;
                // An unrelated preference write must not serialize draft
                // heights.
                const QString color =
                    store.channelColor(QStringLiteral("brake"));
                store.setChannelColor(QStringLiteral("brake"),
                                      color == QStringLiteral("#7fbbb3")
                                          ? QStringLiteral("#e67e80")
                                          : QStringLiteral("#7fbbb3"));
                if (!require(near(YamlConfig::instance()
                                      .value({QStringLiteral("channels"),
                                              rows.front().key,
                                              QStringLiteral("weight")},
                                             1.0)
                                      .toDouble(),
                                  1.0),
                             "draft leaked into preferences"))
                    return;
                store.setChannelColor(QStringLiteral("brake"), color);
                if (!require(window->grabWindow().save(shot), "screenshot"))
                    return;
                if (!require(click(window, "traceResizeCancel") &&
                                 !store.resizingTraces(),
                             "cancel button"))
                    return;
                state->phase = 4;
            } else if (state->phase == 4) {
                const auto rows = samples(trace);
                for (int i = 0; i < rows.size(); ++i)
                    if (!require(
                            near(rows[i].height, state->original[i].height),
                            "cancel did not restore heights"))
                        return;
                click(window, "resizeTracesButton");
                const auto& lane = rows[rows.size() - 2];
                drag(trace, lane.y + lane.height, -trace->height() * 0.60);
                if (!require(click(window, "traceResizeReset"), "reset button"))
                    return;
                const auto reset = samples(trace);
                for (int i = 0; i < reset.size(); ++i)
                    if (!require(
                            near(reset[i].height, state->original[i].height),
                            "reset defaults"))
                        return;
                drag(trace, lane.y + lane.height, -trace->height() * 0.60);
                if (!require(store.channelWeight(state->rawKey) > 2.0 &&
                                 fits(samples(trace), state->original),
                             "raw lane resize"))
                    return;
                if (!require(click(window, "traceResizeSave") &&
                                 !store.resizingTraces(),
                             "save button"))
                    return;
                state->phase = 5;
            } else {
                if (!store.extraChannelData(state->rawKey, false)) return;
                if (!require(store.channelWeight(state->rawKey) > 2.0,
                             "saved raw weight clamped"))
                    return;
                const double savedWeight = store.channelWeight(state->rawKey);
                store.beginTraceResize();
                store.setChannelWeight(state->rawKey, savedWeight * 1.5);
                QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape,
                                 Qt::NoModifier);
                QCoreApplication::sendEvent(trace, &escape);
                if (!require(!store.resizingTraces() &&
                                 near(store.channelWeight(state->rawKey),
                                      savedWeight),
                             "Escape cancels the draft"))
                    return;
                store.beginTraceResize();
                store.beginCornerEdit();
                if (!require(!store.resizingTraces() && store.editingCorners(),
                             "edit modes overlap"))
                    return;
                store.cancelCornerEdit();
                store.beginTraceResize();
                auto* save = window->findChild<QObject*>(
                    QStringLiteral("traceResizeSaveShortcut"));
                if (!require(
                        save && save->property("enabled").toBool() &&
                            save->property("sequence").value<QKeySequence>() ==
                                QKeySequence(QStringLiteral("Ctrl+S")) &&
                            QMetaObject::invokeMethod(save, "activated") &&
                            !store.resizingTraces(),
                        "Ctrl+S binding"))
                    return;
                const auto result = trace->benchmarkGeometry(60);
                qWarning() << "AUTOTEST trace resize geometry average_ms:"
                           << result.value("averageMs").toDouble();
                timer->stop();
                qWarning()
                    << "AUTOTEST trace resize: drag, push neighbours, fit, "
                       "cursor, draft isolation, cancel, reset, raw save PASS";
                QCoreApplication::exit(0);
            }
        });
    timer->start();
    return true;
}
