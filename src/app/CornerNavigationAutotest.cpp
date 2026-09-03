// Native corner navigation acceptance; no source or user-config mutations.
#include "AutotestHarness.h"
#include "TelemetryStore.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QKeySequence>
#include <QMouseEvent>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTimer>
#include <QVariantAnimation>

#include <algorithm>
#include <cmath>
#include <memory>

namespace {
struct Check {
    QElapsedTimer total, phaseTime;
    int phase = 0;
    bool interpolated = false;
    QPointF from, held;
};
bool near(double a, double b) { return std::abs(a - b) < 1e-8; }
QPointF target(const CornerZone& corner) {
    const double span = std::clamp(
        std::max(0.002, corner.end - corner.start) / 0.30, 0.004, 1.0);
    const double start = corner.mid() - span * 0.25;
    return {start, start + span};
}
bool click(QQuickItem* button) {
    if (!button || !button->isEnabled()) return false;
    const QPointF p(button->width() / 2, button->height() / 2);
    QMouseEvent press(QEvent::MouseButtonPress, p, p, p, Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, p, p, p, Qt::LeftButton,
                        Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(button, &press);
    QCoreApplication::sendEvent(button, &release);
    return true;
}
bool shortcut(QObject* root, const char* name, const char* key) {
    QObject* action = root->findChild<QObject*>(QLatin1String(name));
    // A headless window cannot receive desktop keyboard focus. Exercise the
    // actual shortcut handler and verify its enabled state and key binding.
    return action && action->property("enabled").toBool() &&
           action->property("sequence").value<QKeySequence>() ==
               QKeySequence(QLatin1String(key)) &&
           QMetaObject::invokeMethod(action, "activated");
}
}  // namespace

bool omatrack::autotest::installCornerNavigation(QQmlApplicationEngine& engine,
                                                 TelemetryStore& store) {
    const QString source =
        qEnvironmentVariable("OMATRACK_AUTOTEST_CORNER_NAVIGATION");
    if (source.isEmpty()) return false;
    const QString shot = qEnvironmentVariable("OMATRACK_AUTOTEST");
    auto check = std::make_shared<Check>();
    check->total.start();
    auto* timer = new QTimer(&engine);
    timer->setInterval(20);
    QObject::connect(
        timer, &QTimer::timeout, &engine,
        [&engine, &store, timer, check, source, shot] {
            const auto require = [timer](bool ok, const char* message) {
                if (!ok) {
                    timer->stop();
                    qWarning()
                        << "AUTOTEST corner navigation FAILED:" << message;
                    QCoreApplication::exit(1);
                }
                return ok;
            };
            if (!require(check->total.elapsed() < 60000, "timeout")) return;
            if (engine.rootObjects().isEmpty() || store.loading() ||
                store.lapLoading())
                return;
            auto* window =
                qobject_cast<QQuickWindow*>(engine.rootObjects().first());
            if (!require(window, "no window")) return;
            auto* animation = store.findChild<QVariantAnimation*>(
                QStringLiteral("cornerFocusTransition"));
            if (!require(animation, "no transition")) return;
            const bool running =
                animation->state() == QAbstractAnimation::Running;
            const QPointF bounds(store.viewStart(), store.viewEnd());
            auto* previous = window->findChild<QQuickItem*>(
                QStringLiteral("previousCornerButton"));
            auto* next = window->findChild<QQuickItem*>(
                QStringLiteral("nextCornerButton"));
            if (check->phase == 0) {
                store.openFile(source);
                check->phase = 1;
            } else if (check->phase == 1) {
                if (!store.primaryUnified()) return;
                if (store.cornerCount() < 3) store.autoGenerateCorners();
                // Owned test zones exercise the deliberately unclamped lap
                // edges.
                store.addCorner(0.0, 0.012);
                store.addCorner(0.985, 1.0);
                if (!require(store.cornerCount() >= 4,
                             "need real braking zones"))
                    return;
                store.setViewStart(0.1);
                store.setViewEnd(0.85);
                store.focusCorner(0);
                check->phase = 2;
            } else if (check->phase == 2) {
                if (!require(previous && next && !previous->isEnabled() &&
                                 next->isEnabled() && bounds.x() < 0 &&
                                 !running,
                             "first corner / controls"))
                    return;
                check->from = bounds;
                if (!require(
                        click(next) && store.focusedCorner() == 1 &&
                            animation->state() == QAbstractAnimation::Running &&
                            near(store.viewStart(), bounds.x()),
                        "next button starts without jumping"))
                    return;
                check->phase = 3;
            } else if (check->phase == 3) {
                const QPointF goal = target(store.corners()[1]);
                if (running) {
                    check->interpolated |= !near(bounds.x(), check->from.x()) &&
                                           !near(bounds.x(), goal.x());
                    return;
                }
                if (!require(check->interpolated &&
                                 near(bounds.x(), goal.x()) &&
                                 near(bounds.y(), goal.y()),
                             "animated target / width"))
                    return;
                if (!require(window->grabWindow().save(shot), "screenshot"))
                    return;
                // Repeated commands retarget from the current viewport; no
                // queue of old destinations is allowed to pull the view back
                // afterwards.
                if (!require(
                        shortcut(window, "nextCornerShortcut", "J") &&
                            store.focusedCorner() == 2 &&
                            shortcut(window, "previousCornerShortcut", "H") &&
                            store.focusedCorner() == 1 &&
                            shortcut(window, "nextCornerShortcut", "J") &&
                            store.focusedCorner() == 2,
                        "H/J rapid retarget"))
                    return;
                check->phase = 4;
            } else if (check->phase == 4) {
                if (running) return;
                const QPointF goal = target(store.corners()[2]);
                if (!require(near(bounds.x(), goal.x()) &&
                                 near(bounds.y(), goal.y()),
                             "latest target wins"))
                    return;
                if (!require(click(previous) && store.focusedCorner() == 1,
                             "previous button"))
                    return;
                store.clearCornerFocus();
                check->phaseTime.start();
                check->phase = 5;
            } else if (check->phase == 5) {
                if (check->phaseTime.elapsed() < 220) return;
                if (!require(
                        !running && store.focusedCorner() == -1 &&
                            near(bounds.x(), 0.1) && near(bounds.y(), 0.85),
                        "cancel restores original viewport and stays there"))
                    return;
                store.focusCorner(store.cornerCount() - 1);
                check->phase = 6;
            } else if (check->phase == 6) {
                if (!require(next && !next->isEnabled() && bounds.y() > 1.0,
                             "last corner / no wrap"))
                    return;
                // Text editing must own H/J even while a corner is focused.
                QQmlComponent component(&engine);
                component.setData("import QtQuick\nTextInput {}", QUrl());
                std::unique_ptr<QObject> editor(component.create());
                auto* input = qobject_cast<QQuickItem*>(editor.get());
                if (!require(input, "text input")) return;
                input->setParentItem(window->contentItem());
                input->forceActiveFocus();
                if (!require(!window
                                  ->findChild<QObject*>(
                                      QStringLiteral("previousCornerShortcut"))
                                  ->property("enabled")
                                  .toBool(),
                             "H ignored while typing"))
                    return;
                store.stepFocusedCorner(-1);
                store.zoomAt(store.cursorFrac(), 0.8);
                check->held = {store.viewStart(), store.viewEnd()};
                check->phaseTime.start();
                check->phase = 7;
            } else {
                if (check->phaseTime.elapsed() < 220) return;
                if (!require(!running && near(bounds.x(), check->held.x()) &&
                                 near(bounds.y(), check->held.y()),
                             "manual zoom cancels transition"))
                    return;
                timer->stop();
                qWarning() << "AUTOTEST corner navigation: buttons, H/J, "
                              "interpolation, rapid retarget, cancel, lap "
                              "edges, typing PASS";
                QCoreApplication::exit(0);
            }
        });
    timer->start();
    return true;
}
