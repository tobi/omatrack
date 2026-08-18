// Owns omatrack.yml serialisation, the debounced writer, and the typed
// in-memory preference fields. TelemetryStore reads and writes through this
// collaborator; schedulePreferencesSave() on the store remains the single
// entry point and forwards here.
#pragma once

#include "AsyncJob.h"

#include "TelemetryStore.h"  // SidebarPin, cornerConfigPath, SessionHandle fwd

#include "LibraryLocation.h"
#include "YamlConfig.h"

#include <QColor>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QVariantMap>
#include <QVector>

#include <QtConcurrent/QtConcurrentRun>
#include <QFutureWatcher>
#include <QTimer>

#include <functional>

class PreferencesStore : public QObject {
    Q_OBJECT
public:
    explicit PreferencesStore(QObject* parent = nullptr);
    ~PreferencesStore() override;

    // ── serialisation ───────────────────────────────────────────────
    void loadAll(const QString& defaultTelemetryDir);
    void migrateLegacyConfig();
    void loadPreferences();
    void loadChannelsConfig();
    void loadLocations(const QString& defaultTelemetryDir);
    void scheduleSave();
    void flush();
    bool saveInFlight() const { return saveJob_.running(); }

    static QStringList cornerConfigPath(const QString& track);

    // ── typed in-memory preference fields ───────────────────────────
    // Const + mutable accessors so the store reads and writes through here
    // without a parallel copy of the data.

    const QStringList& recentFiles() const { return recentFiles_; }
    QStringList& recentFiles() { return recentFiles_; }

    bool videoMuted() const { return videoMuted_; }
    void setVideoMuted(bool muted) { videoMuted_ = muted; }

    const QString& requestedSyncStrategy() const {
        return requestedComparisonSyncStrategy_;
    }
    QString& requestedSyncStrategy() {
        return requestedComparisonSyncStrategy_;
    }

    qint64 cacheLimitBytes() const { return cacheLimitBytes_; }

    const QVector<SidebarPin>& sidebarPins() const { return sidebarPins_; }
    QVector<SidebarPin>& sidebarPins() { return sidebarPins_; }
    int sidebarPinIndex(const QString& kind, const QString& path) const;

    const QHash<QString, QString>& driverMappings() const {
        return driverMappings_;
    }
    QHash<QString, QString>& driverMappings() { return driverMappings_; }

    const QHash<QString, QString>& trackAssignments() const {
        return trackAssignments_;
    }
    QHash<QString, QString>& trackAssignments() { return trackAssignments_; }

    const QHash<QString, QVariantMap>& recordingMetadata() const {
        return recordingMetadata_;
    }
    QHash<QString, QVariantMap>& recordingMetadata() {
        return recordingMetadata_;
    }

    const QStringList& channelOrder() const { return channelOrder_; }
    QStringList& channelOrder() { return channelOrder_; }

    const QHash<QString, bool>& channelVisible() const {
        return channelVisible_;
    }
    QHash<QString, bool>& channelVisible() { return channelVisible_; }

    const QHash<QString, QColor>& channelColors() const {
        return channelColors_;
    }
    QHash<QString, QColor>& channelColors() { return channelColors_; }

    const QHash<QString, double>& channelWeights() const {
        return channelWeights_;
    }
    QHash<QString, double>& channelWeights() { return channelWeights_; }

    const QVector<omatrack::LibraryLocation>& locations() const {
        return locations_;
    }
    QVector<omatrack::LibraryLocation>& locations() { return locations_; }

    int locationIndex(const QString& id) const;
    bool appendFolderLocation(const QString& dirPath,
                              bool requireExists = true);

    // ── selection keys (persisted, but read/written by the store) ───
    const QString& lastPrimaryKey() const { return lastPrimaryKey_; }
    QString& lastPrimaryKey() { return lastPrimaryKey_; }
    const QString& lastCompareKey() const { return lastCompareKey_; }
    QString& lastCompareKey() { return lastCompareKey_; }
    int lastPrimaryLap() const { return lastPrimaryLap_; }
    int& lastPrimaryLap() { return lastPrimaryLap_; }
    int lastCompareLap() const { return lastCompareLap_; }
    int& lastCompareLap() { return lastCompareLap_; }

signals:
    void operationError(const QString& title, const QString& message);

private:
    void migrateLegacyCornerCsvs();

    QTimer* prefsTimer_ = nullptr;
    AsyncJob<QString> saveJob_;

    QStringList recentFiles_;
    bool videoMuted_ = false;
    QString requestedComparisonSyncStrategy_ = QStringLiteral("gps-continuous");
    qint64 cacheLimitBytes_ = 0;
    QVector<SidebarPin> sidebarPins_;
    QHash<QString, QString> driverMappings_;
    QHash<QString, QString> trackAssignments_;
    QHash<QString, QVariantMap> recordingMetadata_;
    QStringList channelOrder_;
    QHash<QString, bool> channelVisible_;
    QHash<QString, QColor> channelColors_;
    QHash<QString, double> channelWeights_;
    QVector<omatrack::LibraryLocation> locations_;
    QString lastPrimaryKey_;
    QString lastCompareKey_;
    int lastPrimaryLap_ = -1;
    int lastCompareLap_ = -1;
};
