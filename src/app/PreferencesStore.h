// Owns omatrack.yml serialisation, the debounced writer, and the typed
// in-memory preference fields. TelemetryStore reads and writes through this
// collaborator; schedulePreferencesSave() on the store remains the single
// entry point and forwards here.
#pragma once

#include "AsyncJob.h"
#include "ChannelAppearance.h"

#include "TelemetryStore.h"  // SidebarPin, cornerConfigPath, SessionHandle fwd

#include "LibraryLocation.h"
#include "YamlConfig.h"

#include <QColor>
#include <QPointF>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QVariantMap>
#include <QVector>

#include <QtConcurrent/QtConcurrentRun>
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

    QPointF videoHudPosition() const { return videoHudPosition_; }
    void setVideoHudPosition(QPointF point) { videoHudPosition_ = point; }
    bool videoMuted() const { return videoMuted_; }
    void setVideoMuted(bool muted) { videoMuted_ = muted; }
    bool imageTelemetryEnabled() const { return imageTelemetryEnabled_; }
    void setImageTelemetryEnabled(bool enabled) {
        imageTelemetryEnabled_ = enabled;
    }
    const QString& imageTelemetryModel() const { return imageTelemetryModel_; }
    void setImageTelemetryModel(const QString& path) {
        imageTelemetryModel_ = path;
    }
    bool imageModelManaged() const { return imageModelManaged_; }
    void setImageModelManaged(bool managed) { imageModelManaged_ = managed; }
    bool imageModelUpdates() const { return imageModelUpdates_; }
    void setImageModelUpdates(bool updates) { imageModelUpdates_ = updates; }

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

    const QHash<QString, ChannelAppearance>& channelAppearance() const {
        return channelAppearance_;
    }
    QHash<QString, ChannelAppearance>& channelAppearance() {
        return channelAppearance_;
    }

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

    bool eventMode() const { return eventMode_; }
    void setEventMode(bool enabled) { eventMode_ = enabled; }
    const QString& eventTrack() const { return eventTrack_; }
    void setEventTrack(const QString& track) { eventTrack_ = track; }
    const QString& eventSession() const { return eventSession_; }
    void setEventSession(const QString& session) { eventSession_ = session; }
    const QString& eventDate() const { return eventDate_; }
    void setEventDate(const QString& date) { eventDate_ = date; }

    const QString& usbDest() const { return usbDest_; }
    void setUsbDest(const QString& dest) { usbDest_ = dest; }
    const QString& usbFormat() const { return usbFormat_; }
    void setUsbFormat(const QString& format) { usbFormat_ = format; }
    const QString& usbRenameScript() const { return usbRenameScript_; }
    void setUsbRenameScript(const QString& script) {
        usbRenameScript_ = script;
    }

    const QString& overlayRefColor() const { return overlayRefColor_; }
    void setOverlayRefColor(const QString& color) { overlayRefColor_ = color; }
    const QString& overlayRefStyle() const { return overlayRefStyle_; }
    void setOverlayRefStyle(const QString& style) { overlayRefStyle_ = style; }
    bool overlayRefWhite() const { return overlayRefWhite_; }
    void setOverlayRefWhite(bool enabled) { overlayRefWhite_ = enabled; }
    QStringList enabledPlugins() const { return enabledPlugins_; }
    void setEnabledPlugins(const QStringList& ids) { enabledPlugins_ = ids; }

signals:
    void operationError(const QString& title, const QString& message);

private:
    void migrateLegacyCornerCsvs();

    QTimer* prefsTimer_ = nullptr;
    AsyncJob<QString> saveJob_;

    QStringList recentFiles_;
    bool videoMuted_ = false;
    bool imageTelemetryEnabled_ = false;
    QString imageTelemetryModel_;
    bool imageModelManaged_ = false;
    bool imageModelUpdates_ = true;
    QPointF videoHudPosition_{-1.0, -1.0};
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
    QHash<QString, ChannelAppearance> channelAppearance_;
    QVector<omatrack::LibraryLocation> locations_;
    QString lastPrimaryKey_;
    QString lastCompareKey_;
    int lastPrimaryLap_ = -1;
    int lastCompareLap_ = -1;
    bool eventMode_ = false;
    QString eventTrack_;
    QString eventSession_;
    QString eventDate_;
    QString usbDest_;
    QString usbFormat_ = QStringLiteral("{track}/{date}/{session}/{original}");
    QString usbRenameScript_;
    QString overlayRefColor_ = QStringLiteral("#e09d7f");
    QString overlayRefStyle_ = QStringLiteral("dashed");
    bool overlayRefWhite_ = false;
    QStringList enabledPlugins_;
};
