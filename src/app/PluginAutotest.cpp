// Native trace-group plugin acceptance. Compiled only with the harness.
//
// OMATRACK_AUTOTEST_PLUGIN=<plugin id> with OMATRACK_AUTOTEST_PLUGIN_FILE=
// <copied recording with GPS + wall clock>. XDG_CONFIG_HOME must hold the
// plugin under omatrack/plugins/<id>/. The check waits for the plugin to
// offer channels for the session, enables it, and requires a finite
// resampled value on the lap grid for at least one channel — a real round
// trip through discovery → channels() → samples() → resample → channel row.
#include "AutotestHarness.h"
#include "OverlayManager.h"
#include "TelemetryStore.h"

#include <QCoreApplication>
#include <QDebug>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QTimer>

#include <cmath>

bool omatrack::autotest::installPlugin(QQmlApplicationEngine& engine,
                                       TelemetryStore& store) {
    const QString pluginId = qEnvironmentVariable("OMATRACK_AUTOTEST_PLUGIN");
    const QString file = qEnvironmentVariable("OMATRACK_AUTOTEST_PLUGIN_FILE");
    if (pluginId.isEmpty() || file.isEmpty()) return false;
    const QString shot = qEnvironmentVariable("OMATRACK_AUTOTEST");
    auto* timer = new QTimer(&engine);
    timer->setInterval(200);
    auto phase = std::make_shared<int>(0);
    auto ticks = std::make_shared<int>(0);
    QObject::connect(
        timer, &QTimer::timeout, &engine,
        [&, timer, phase, ticks, pluginId, file, shot]() {
            const auto fail = [&](const QString& why) {
                timer->stop();
                qWarning() << "AUTOTEST plugin FAILED:" << why;
                QCoreApplication::exit(1);
            };
            if (++*ticks > 600)
                return fail(
                    QStringLiteral("timed out in phase %1").arg(*phase));
            QObject* rootObject = engine.rootObjects().isEmpty()
                                      ? nullptr
                                      : engine.rootObjects().first();
            auto* window = qobject_cast<QQuickWindow*>(rootObject);
            if (!window) return;
            const auto pluginRow = [&]() -> QVariantMap {
                for (const QVariant& row : store.pluginLibrary())
                    if (row.toMap().value(QStringLiteral("id")).toString() ==
                        pluginId)
                        return row.toMap();
                return {};
            };
            if (*phase == 0) {
                if (!store.ready()) return;
                store.openFile(file);
                *phase = 1;
                *ticks = 0;
                return;
            }
            if (*phase == 1) {
                const QVariantMap row = pluginRow();
                if (*ticks % 25 == 0)
                    qWarning()
                        << "AUTOTEST plugin: waiting; session"
                        << store.primarySessionKey() << "unified"
                        << (store.primaryUnified() != nullptr) << "row" << row;
                if (store.primarySessionKey().isEmpty() ||
                    !store.primaryUnified())
                    return;
                if (row.isEmpty())
                    return fail(QStringLiteral(
                        "plugin not discovered under the scratch config"));
                if (!row.value(QStringLiteral("error")).toString().isEmpty())
                    return fail(QStringLiteral("plugin error: ") +
                                row.value(QStringLiteral("error")).toString());
                if (row.value(QStringLiteral("loading")).toBool() ||
                    row.value(QStringLiteral("channelCount")).toInt() == 0)
                    return;  // channels() still running (network)
                qWarning() << "AUTOTEST plugin: offered"
                           << row.value(QStringLiteral("channelCount")).toInt()
                           << "channels;"
                           << row.value(QStringLiteral("status")).toString();
                store.setPluginEnabled(pluginId, true);
                *phase = 2;
                *ticks = 0;
                return;
            }
            if (*phase == 2) {
                const OverlayGroup* group = nullptr;
                for (const OverlayGroup& candidate : store.overlayGroups())
                    if (candidate.pluginId == pluginId) group = &candidate;
                if (!group) return;  // samples() still running
                int finiteChannels = 0;
                QStringList report;
                for (const OverlayChannel& channel : group->channels) {
                    const std::vector<double>* data =
                        store.overlayChannelData(channel.key);
                    if (!data) return;  // resample pending
                    double first = std::nan("");
                    double last = std::nan("");
                    int finite = 0;
                    for (double value : *data)
                        if (std::isfinite(value)) {
                            if (!std::isfinite(first)) first = value;
                            last = value;
                            ++finite;
                        }
                    if (finite > 0) ++finiteChannels;
                    report << QStringLiteral("%1: %2/%3 finite, %4 → %5 %6")
                                  .arg(channel.name)
                                  .arg(finite)
                                  .arg(data->size())
                                  .arg(first, 0, 'f', 2)
                                  .arg(last, 0, 'f', 2)
                                  .arg(channel.unit);
                }
                if (finiteChannels == 0)
                    return fail(QStringLiteral("no plugin channel has a finite "
                                               "value on the lap grid: ") +
                                report.join("; "));
                bool listed = false;
                auto* channels = store.channelsModel();
                for (int row = 0; channels && row < channels->rowCount(); ++row)
                    if (channels->index(row, 0)
                            .data(ChannelListModel::KeyRole)
                            .toString() == group->channels.first().key)
                        listed = true;
                if (!listed)
                    return fail(QStringLiteral(
                        "plugin channel missing from the channel list"));
                // The plugin group must sit after every sidecar/source group.
                if (store.overlayGroups().last().pluginId != pluginId)
                    return fail(QStringLiteral(
                        "plugin group is not the last overlay group"));
                window->grabWindow().save(shot);
                timer->stop();
                qWarning().noquote() << "AUTOTEST plugin:" << pluginId
                                     << "attached:" << report.join("; ");
                QCoreApplication::exit(0);
            }
        });
    timer->start();
    return true;
}
