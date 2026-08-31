// Lua trace-group plugins: `$XDG_CONFIG_HOME/omatrack/plugins/*/plugin.lua`.
//
// A plugin offers extra channels for the open session (weather, timing
// feeds, lab data …) and is drawn as an overlay group, exactly like an MTX
// sidecar. Every call runs in a fresh sandboxed Lua state on a worker; state
// survives between calls only through the plugin's `kv` store. Nothing here
// touches the GUI thread except the completion callbacks.
//
// Lua contract (`plugin.lua` returns this table):
//
//   return {
//     id = "weather",          -- stable, [a-z0-9_-]; channel keys derive from
//     it name = "Weather",        -- group title in the channel list version =
//     1,
//     -- Offer channels for a session. May use http/kv. Return {} to offer
//     none. channels = function(session)
//       return { { key = "temp", name = "Air temperature", unit = "°C",
//                  default_visible = true }, … }
//     end,
//     -- Deliver samples for the requested keys. Times are integer nanoseconds,
//     -- either file-relative (`t`) or Unix-epoch (`utc_ns`). Sorted ascending.
//     samples = function(session, keys)
//       return { temp = { utc_ns = {…}, v = {…} }, … }
//     end,
//   }
//
// `session`: path, name, track, driver, date, timezone, utc_start_ns (nil when
// the recording carries no wall-clock), start_ns, end_ns, duration_ns,
// latitude, longitude (nil without GPS), lap = { id, start_ns, end_ns } | nil.
//
// Globals: http.get(url, {timeout_ms=, headers={}}) -> {status, body, headers,
// error}; json.decode/encode; kv.get(key) / kv.set(key, value, ttl_seconds) /
// kv.delete(key) (persisted per plugin, values are JSON-serialisable); io.read
// / io.write / io.exists / io.list relative to the plugin directory (read) or
// its cache directory (read/write) — never outside; log(...); os.time / os.date
// / os.clock. No load/require/dofile.
#pragma once

#include "AsyncJob.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QHash>
#include <QVector>

#include <memory>
#include <vector>

namespace omatrack {

struct PluginSession {
    QString path;
    QString name;
    QString track;
    QString driver;
    QString date;
    QString timezone;
    qint64 utcStartNs = -1;  // < 0: unknown
    qint64 startNs = 0;
    qint64 endNs = 0;
    double latitude = 0.0;
    double longitude = 0.0;
    bool hasLocation = false;
    int lapId = -1;
    qint64 lapStartNs = 0;
    qint64 lapEndNs = 0;
    bool operator==(const PluginSession& other) const {
        // Everything a plugin can branch on. GPS location and wall clock
        // arrive after the first selection signal, so they must count.
        return path == other.path && utcStartNs == other.utcStartNs &&
               startNs == other.startNs && endNs == other.endNs &&
               lapId == other.lapId && hasLocation == other.hasLocation &&
               (!hasLocation ||
                (latitude == other.latitude && longitude == other.longitude)) &&
               track == other.track && timezone == other.timezone;
    }
};

struct PluginChannelOffer {
    QString key;
    QString name;
    QString unit;
    bool defaultVisible = true;
};

struct PluginSeries {
    QString key;
    // Host (file-relative) nanoseconds, ascending.
    std::shared_ptr<std::vector<qint64>> times;
    std::shared_ptr<std::vector<double>> values;
};

struct PluginInfo {
    QString id;
    QString name;
    QString directory;
    int version = 0;
    QString error;  // load-time problem; the plugin stays listed but disabled
};

struct PluginOffer {
    PluginInfo plugin;
    QVector<PluginChannelOffer> channels;
    QString error;
    /// Optional second return value of channels(): why nothing is offered.
    QString note;
    bool loading = false;
};

struct PluginSamplesResult {
    QString pluginId;
    PluginSession session;
    QVector<PluginSeries> series;
    QString error;
    QStringList logLines;
};

/// Runtime paths, injectable for tests.
struct PluginPaths {
    QString pluginRoot;  // …/omatrack/plugins
    QString cacheRoot;   // …/omatrack/plugins (under XDG_CACHE_HOME)
    static PluginPaths defaults();
};

/// Worker-only. Loads `plugin.lua`, calls one entry point, converts results.
/// Exposed for tests and for the CLI; the host wraps it in jobs.
PluginInfo describePlugin(const QString& directory);
PluginOffer runPluginChannels(const PluginInfo& plugin,
                              const PluginSession& session,
                              const PluginPaths& paths, const IoCancel& cancel,
                              int timeoutMs = 60000);
PluginSamplesResult runPluginSamples(const PluginInfo& plugin,
                                     const PluginSession& session,
                                     const QStringList& keys,
                                     const PluginPaths& paths,
                                     const IoCancel& cancel,
                                     int timeoutMs = 60000);

class PluginHost : public QObject {
    Q_OBJECT
public:
    explicit PluginHost(QObject* parent = nullptr,
                        PluginPaths paths = PluginPaths::defaults());

    /// Re-read the plugin directory (worker). Emits pluginsChanged.
    void discover();
    /// Current session; a change re-runs `channels` for every plugin.
    void setSession(const PluginSession& session);
    /// Plugins whose groups should be attached; persisted by the store.
    void setEnabled(const QStringList& ids);
    QStringList enabled() const { return enabled_; }
    /// Run `samples` for an enabled plugin (worker); emits samplesReady.
    void requestSamples(const QString& pluginId);

    const QVector<PluginOffer>& offers() const { return offers_; }
    /// Rows for QML: id, name, version, directory, status, error,
    /// channelCount, enabled, loading.
    QVariantList library() const;
    bool hasSession() const { return session_.endNs > session_.startNs; }

signals:
    void pluginsChanged();
    void samplesReady(const omatrack::PluginSamplesResult& result);
    void operationError(const QString& title, const QString& message);

private:
    void runChannelsForAll();

    PluginPaths paths_;
    PluginSession session_;
    QStringList enabled_;
    QVector<PluginOffer> offers_;
    AsyncJob<QVector<PluginInfo>> discoveryJob_;
    // Latest-wins per plugin: a session change supersedes an in-flight offer
    // or samples run of the same plugin.
    QHash<QString, AsyncJob<PluginOffer>*> channelJobs_;  // Qt-owned
    QHash<QString, AsyncJob<PluginSamplesResult>*> sampleJobs_;
};

}  // namespace omatrack

Q_DECLARE_METATYPE(omatrack::PluginSamplesResult)
