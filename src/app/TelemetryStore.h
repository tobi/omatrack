// Qt object model bridging the omatrack core engine to QML.
//
// Mirrors omatrack's TelemetryStore architecture: lazy per-file
// SessionHandles, Track→Date→Session→Laps grouping, primary/compare lap
// selection, cursor + viewport state, corner zones, and channel display
// configuration.

#pragma once

#include <QByteArray>
#include <QColor>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QHash>
#include <QJsonObject>
#include <QVector>
#include <QUrl>
#include <QtQml/qqmlregistration.h>
#include <QVariantList>
#include <QVariantMap>

#include <memory>
#include <cmath>
#include <limits>
#include <vector>

namespace omatrack {
class TelemetrySource;
struct UnifiedLap;
struct Lap;
}  // namespace omatrack

class SessionHandle;
class TelemetryStore;
class QNetworkAccessManager;
class QTimer;
class QQmlEngine;
class QJSEngine;
template <typename T>
class QFutureWatcher;
struct SessionScanResult;
struct SessionLapLoadResult;
struct FileOpenResult;

// ── damper alignment ────────────────────────────────────────────────

// Front-damper traces for the active and reference laps on one shared vertical
// scale. Used by the manual alignment tool when a session has no positional
// GPS; kept as plain vectors so the renderer walks them without boxing every
// sample into a QVariant.
struct DamperAlignment {
    std::vector<double> primary;
    std::vector<double> compare;
    double minimum = 0.0;
    double maximum = 1.0;

    bool valid() const { return primary.size() > 1 && compare.size() > 1; }
    double span() const { return std::max(1e-6, maximum - minimum); }
};

// ── corner graph payload ────────────────────────────────────────────

// Everything the corner graph renderer draws for one corner, resampled onto a
// fixed grid across the corner's context window. Kept as plain vectors: the
// previous QVariantList form boxed 800 doubles per lap per corner and the
// window rebuilds them on every selection change.
struct CornerGraphSeries {
    std::vector<double> speed;
    std::vector<double> throttle;
    std::vector<double> brake;
    std::vector<double> steering;
    double maxBrake = 1.0;

    bool valid() const { return speed.size() > 1; }
};

// Damper travel across a corner's approach window, used by the fallback
// alignment tool when a session has no positional GPS.
struct CornerDamperWindow {
    std::vector<double> primary;
    std::vector<double> compare;
    double windowMeters = 0.0;
    double cornerStartMeters = 0.0;

    bool valid() const { return primary.size() > 1 && compare.size() > 1; }
};

struct CornerGraph {
    CornerGraphSeries primary;
    CornerGraphSeries compare;
    CornerDamperWindow damper;

    // Positions are 0-1 across the context window, not the lap.
    double zoneStart = 0.0;
    double zoneEnd = 1.0;
    double turnIn = -1.0;
    double apex = -1.0;
    double pickup = -1.0;
    double compareTurnIn = -1.0;
    double compareApex = -1.0;
    double comparePickup = -1.0;

    double windowMeters = 0.0;
    double zoneMeters = 0.0;

    bool valid() const { return primary.valid(); }
    bool hasCompare() const { return compare.valid(); }
};

// ── corner zone ─────────────────────────────────────────────────────

struct CornerZone {
    QString name;
    double start = 0.0;  // 0-1 lap fraction
    double end = 0.0;
    double mid() const { return (start + end) / 2.0; }
};

// ── lap node (leaf of the sidebar tree) ─────────────────────────────

struct LapEntry {
    int lapId = 0;
    double startTime = 0.0;
    double endTime = 0.0;
    double timeMs = 0.0;
    QString label;
    QString timeText;
    bool isFastest = false;
    /// False for the leading/trailing recording fragment (out/in fragment).
    bool isComplete = true;
    /// Complete crossing-to-crossing lap whose time is a pit in/out outlier.
    bool isPitLap = false;
    /// Representative timed racing lap: eligible for best-lap statistics.
    bool countsForBest() const { return isComplete && !isPitLap; }
};

struct SourceChannelSummary {
    QString name;
    QString unit;
    QStringList examples;
    double frequencyHz = 0.0;
    int recordingCount = 1;
};

// ── session handle: lazy parse + unified-lap cache per file ─────────

class SessionHandle {
public:
    explicit SessionHandle(const QString& path,
                           const QJsonObject& cachedMetadata = {});
    ~SessionHandle();

    const QString& path() const { return path_; }
    QString stem() const;

    const omatrack::TelemetrySource* source() const { return src_.get(); }
    const QVector<LapEntry>& laps() const { return laps_; }
    std::shared_ptr<const omatrack::UnifiedLap> unifiedLap(int lapId) const;
    QString sessionKey() const;
    bool isVideo() const;

    QString track() const { return track_; }
    QString sessionTime() const { return time_; }
    QString driverId() const { return driverId_; }
    QString bestLapTime();
    double bestLapMs();
    QString date() const { return date_; }
    QString driver() const { return driver_; }
    QString vehicle() const { return vehicle_; }
    QString venue() const { return venue_; }
    QString carNumber() const { return carNumber_; }
    QString carClass() const { return carClass_; }
    bool hasGpsLocation() const {
        return std::isfinite(gpsLatitude_) && std::isfinite(gpsLongitude_);
    }
    double gpsLatitude() const { return gpsLatitude_; }
    double gpsLongitude() const { return gpsLongitude_; }
    QString driverMappingKey() const {
        return carNumber_ + QStringLiteral("|") + carClass_ +
               QStringLiteral("|") + driverId_;
    }
    const QVector<SourceChannelSummary>& sourceChannels() const {
        return sourceChannels_;
    }
    const QHash<QString, QString>& automaticChannelMappings() const {
        return automaticChannelMappings_;
    }

    bool hasSummary() const { return summaryLoaded_; }
    bool loadSummaryForIndex();
    QJsonObject metadataForCache() const;
    void adoptLoadedLap(int lapId,
                        std::unique_ptr<omatrack::TelemetrySource> source,
                        std::shared_ptr<const omatrack::UnifiedLap> unified,
                        double driverId = 0.0, bool forceDriverId = false);
    void clearUnifiedCache() { unifiedCache_.clear(); }

private:
    void applyCachedMetadata(const QJsonObject& metadata);
    void ensureLapSummary();
    void applyEventDriverId(double eventDriverId, bool force = false);
    void captureGpsLocation(const omatrack::TelemetrySource& source);
    void captureSourceChannels(const omatrack::TelemetrySource& source);
    void populateLaps(const std::vector<omatrack::Lap>& detected);
    QString path_;
    std::unique_ptr<omatrack::TelemetrySource> src_;
    QVector<LapEntry> laps_;
    QHash<int, std::shared_ptr<const omatrack::UnifiedLap>> unifiedCache_;
    QString time_;
    QString driverId_;
    QString track_;
    QString date_;
    bool summaryLoaded_ = false;
    bool driverIdResolved_ = false;
    QString carNumber_;
    QString carClass_;
    QString driver_;
    QString vehicle_;
    QString venue_;
    QVector<SourceChannelSummary> sourceChannels_;
    QHash<QString, QString> automaticChannelMappings_;
    double gpsLatitude_ = std::numeric_limits<double>::quiet_NaN();
    double gpsLongitude_ = std::numeric_limits<double>::quiet_NaN();
};

// ── store (root model exposed to QML) ───────────────────────────────

class TelemetryStore : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(Store)
    QML_SINGLETON
    Q_PROPERTY(bool ready READ ready NOTIFY readyChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    Q_PROPERTY(bool lapLoading READ lapLoading NOTIFY lapLoadingChanged)
    Q_PROPERTY(bool comparing READ comparing NOTIFY selectionChanged)
    Q_PROPERTY(bool editingCorners READ editingCorners WRITE setEditingCorners
                   NOTIFY editingCornersChanged)
    Q_PROPERTY(double cursorFrac READ cursorFrac WRITE setCursorFrac NOTIFY
                   cursorFracChanged)
    Q_PROPERTY(bool hasGpsData READ hasGpsData NOTIFY selectionChanged)
    Q_PROPERTY(
        double viewStart READ viewStart WRITE setViewStart NOTIFY viewChanged)
    Q_PROPERTY(double viewEnd READ viewEnd WRITE setViewEnd NOTIFY viewChanged)
    Q_PROPERTY(int channelHeight READ channelHeight WRITE setChannelHeight
                   NOTIFY channelHeightChanged)
    Q_PROPERTY(QString primaryLabel READ primaryLabel NOTIFY selectionChanged)
    Q_PROPERTY(QString primaryDetail READ primaryDetail NOTIFY selectionChanged)
    Q_PROPERTY(QString primaryDriverName READ primaryDriverName NOTIFY
                   selectionChanged)
    Q_PROPERTY(QString primaryDriverMappingKey READ primaryDriverMappingKey
                   NOTIFY selectionChanged)
    Q_PROPERTY(QString compareLabel READ compareLabel NOTIFY selectionChanged)
    Q_PROPERTY(QString roomName READ roomName NOTIFY selectionChanged)
    Q_PROPERTY(QString primarySessionKey READ primarySessionKey NOTIFY
                   selectionChanged)
    Q_PROPERTY(int primaryLapIndex READ primaryLapIndex NOTIFY selectionChanged)
    Q_PROPERTY(QString compareSessionKey READ compareSessionKey NOTIFY
                   selectionChanged)
    Q_PROPERTY(int compareLapIndex READ compareLapIndex NOTIFY selectionChanged)
    Q_PROPERTY(double referenceAlignment READ referenceAlignment WRITE
                   setReferenceAlignment NOTIFY referenceAlignmentChanged)
    Q_PROPERTY(
        bool trackAtlasReady READ trackAtlasReady NOTIFY trackAtlasChanged)
    Q_PROPERTY(
        QString trackAtlasStatus READ trackAtlasStatus NOTIFY trackAtlasChanged)
    Q_PROPERTY(
        QUrl primaryVideoSource READ primaryVideoSource NOTIFY selectionChanged)
    Q_PROPERTY(
        double primaryVideoTime READ primaryVideoTime NOTIFY videoTimeChanged)
    Q_PROPERTY(
        QUrl compareVideoSource READ compareVideoSource NOTIFY selectionChanged)
    Q_PROPERTY(
        double compareVideoTime READ compareVideoTime NOTIFY videoTimeChanged)
    Q_PROPERTY(double comparisonVideoRate READ comparisonVideoRate NOTIFY
                   videoTimeChanged)
    Q_PROPERTY(QString comparisonAlignmentBasis READ comparisonAlignmentBasis
                   NOTIFY selectionChanged)
    Q_PROPERTY(QString comparisonAlignmentConfidence READ
                   comparisonAlignmentConfidence NOTIFY selectionChanged)
    Q_PROPERTY(int comparisonGpsAnchors READ comparisonGpsAnchors NOTIFY
                   selectionChanged)
    Q_PROPERTY(bool videoMuted READ videoMuted WRITE setVideoMuted NOTIFY
                   videoMutedChanged)
public:
    // Registered as the `Store` singleton of the Omatrack QML module. The
    // QML engine owns the single instance and default-constructs it while
    // loading Main.qml; C++ reaches it with
    // QQmlEngine::singletonInstance<TelemetryStore*>("Omatrack", "Store").
    // Never construct a second one: it would scan sessions and write
    // omatrack.yml behind the UI's back.

    explicit TelemetryStore(QObject* parent = nullptr);
    ~TelemetryStore() override;

    Q_INVOKABLE void closeTrack(const QString& trackName);

    Q_INVOKABLE void scan();
    Q_INVOKABLE void addSessionDirectory(const QString& dirPath);
    /// Queue one telemetry source for indexing and lap loading. Ordinary video
    /// is reported through standaloneVideoRequested after background probing.
    Q_INVOKABLE void openFile(const QString& filePath);
    Q_INVOKABLE bool directoryExists(const QString& dirPath) const;
    Q_INVOKABLE QString configFilePath() const;
    Q_INVOKABLE void removeSessionDirectory(const QString& dirPath);
    Q_INVOKABLE QStringList sessionDirectories() const;
    Q_INVOKABLE QVariantList fileSources() const;
    Q_INVOKABLE void clearSessions();
    Q_INVOKABLE QVariantList
    trackGroups() const;  // nested: track → dates → sessions → laps
    Q_INVOKABLE void refreshTrackAtlas();
    Q_INVOKABLE QVariantList trackAtlasChoices() const;
    Q_INVOKABLE QString
    detectedTrackForSession(const QString& sessionKey) const;
    Q_INVOKABLE QString
    assignedTrackForSession(const QString& sessionKey) const;
    Q_INVOKABLE void setSessionTrackAssignment(const QString& sessionKey,
                                               const QString& atlasSlug);
    Q_INVOKABLE QVariantMap folderMetadata(const QString& folderPath) const;
    Q_INVOKABLE bool saveFolderMetadata(const QString& folderPath,
                                        const QVariantMap& metadata);
    Q_INVOKABLE QVariantMap videoMetadata(const QString& videoPath) const;
    Q_INVOKABLE bool saveVideoMetadata(const QString& videoPath,
                                       const QVariantMap& metadata);

    // ── selection ──────────────────────────────────────────────────
    Q_INVOKABLE void selectLap(const QString& sessionKey, int lapId);
    Q_INVOKABLE void compareLap(const QString& sessionKey, int lapId);
    Q_INVOKABLE void clearCompare();
    Q_INVOKABLE void clearPrimary();
    Q_INVOKABLE QVariantList lapsForSession(const QString& sessionKey) const;

    // ── navigation ─────────────────────────────────────────────────
    Q_INVOKABLE void zoomAt(double anchorFrac, double factor);
    Q_INVOKABLE void pan(double deltaFrac);
    Q_INVOKABLE void moveCursorSteps(int steps);
    Q_INVOKABLE void seekCursorSeconds(double seconds);
    Q_INVOKABLE void setCursorFromVideoTime(double mediaTime);
    Q_INVOKABLE void jumpToFraction(double frac);
    // Front-damper traces for the manual reference-alignment tool. The typed
    // accessor feeds the C++ strip renderer; QML only needs to know whether
    // there is anything worth showing.
    Q_INVOKABLE bool hasDamperAlignment() const;
    const DamperAlignment& damperAlignment() const;
    Q_INVOKABLE double referenceAlignmentSeconds() const;
    Q_INVOKABLE void resetReferenceAlignment();

    // ── corners ────────────────────────────────────────────────────
    Q_INVOKABLE void autoGenerateCorners();
    Q_INVOKABLE void saveCorners();
    static QStringList cornerConfigPath(const QString& track);
    static void migrateLegacyCorners(const QString& track);
    Q_INVOKABLE QVariantList cornerList() const;
    Q_INVOKABLE void setCornerName(int index, const QString& name);
    Q_INVOKABLE QVariantList cornerComparison() const;
    // Typed payload for the corner graph renderer, indexed like
    // cornerComparison(). Populated as a side effect of that call, which the
    // corner window already makes whenever the selection or corners change.
    const CornerGraph* cornerGraph(int index) const;
    Q_INVOKABLE void updateCorner(int index, double start, double end);
    Q_INVOKABLE int addCorner(double start, double end);
    Q_INVOKABLE void deleteCorner(int index);
    Q_INVOKABLE void setEditingCorners(bool editing);

    // ── channel config ─────────────────────────────────────────────
    Q_INVOKABLE bool channelVisible(const QString& key) const;
    Q_INVOKABLE void setChannelVisible(const QString& key, bool visible);
    Q_INVOKABLE QVariantList channelSettings() const;
    Q_INVOKABLE QString channelColor(const QString& key) const;
    Q_INVOKABLE void setChannelColor(const QString& key, const QString& color);
    Q_INVOKABLE double channelWeight(const QString& key) const;
    Q_INVOKABLE void setChannelWeight(const QString& key, double weight);
    Q_INVOKABLE QStringList channelOrder() const;

    // ── driver aliases / user preferences ─────────────────────────
    Q_INVOKABLE QVariantList driverAliases() const;
    Q_INVOKABLE void setDriverAlias(const QString& detected,
                                    const QString& display);
    Q_INVOKABLE QVariantList driverMappings() const;
    Q_INVOKABLE void setDriverMapping(const QString& key,
                                      const QString& display);
    Q_INVOKABLE QString driverDisplayName(const QString& sessionKey) const;

    // ── data access for the trace canvas ───────────────────────────
    const omatrack::UnifiedLap* primaryUnified() const;
    const omatrack::UnifiedLap* compareUnified() const;
    const SessionHandle* primarySession() const { return primarySession_; }
    const SessionHandle* compareSession() const { return compareSession_; }
    int primaryLapIndex() const { return primaryLap_; }
    int compareLapIndex() const { return compareLap_; }
    const QVector<CornerZone>& corners() const { return corners_; }
    QString cornerNameAt(double frac) const;
    Q_INVOKABLE QVariantMap cursorReadout() const;
    Q_INVOKABLE double sessionStartUnixTime() const;
    Q_INVOKABLE bool hasGlobalTime() const;
    // Per-sample Δ-time (primary vs compare, distance-aligned; empty when no
    // compare). Shared by the trace canvas and the cursor readout so the
    // numeric Δ and the Δ trace never disagree. Cached until selection changes.
    const QVector<double>& deltaTrace() const;
    const std::vector<double>* extraChannelData(const QString& key,
                                                bool reference) const;
    bool trackAtlasReady() const { return !atlasTracks_.isEmpty(); }
    QString trackAtlasStatus() const { return trackAtlasStatus_; }

    bool ready() const { return ready_; }
    bool loading() const { return loading_; }
    bool lapLoading() const {
        return primaryLapLoading_ || compareLapLoading_ || fileOpenLoading_;
    }
    bool comparing() const { return compareSession_ != nullptr; }
    bool editingCorners() const { return editingCorners_; }
    double cursorFrac() const { return cursorFrac_; }
    void setCursorFrac(double v);
    double viewStart() const { return viewStart_; }
    void setViewStart(double v);
    double viewEnd() const { return viewEnd_; }
    void setViewEnd(double v);
    double viewSpan() const {
        return qBound(0.001, viewEnd_ - viewStart_, 1.0);
    }
    double referenceAlignment() const { return referenceAlignment_; }
    void setReferenceAlignment(double fraction);
    int channelHeight() const { return channelHeight_; }
    void setChannelHeight(int v);
    QString primaryLabel() const;
    QString primaryDetail() const;
    QString primaryDriverName() const;
    QString primaryDriverMappingKey() const;
    bool hasGpsData() const;
    QString compareLabel() const;
    QString roomName() const;
    QString primarySessionKey() const;
    QString compareSessionKey() const;
    QUrl primaryVideoSource() const;
    double primaryVideoTime() const;
    // Reference-lap recording uses the same GPS/speed track-station alignment
    // as traces and delta, so every comparison view shares one coordinate.
    QUrl compareVideoSource() const;
    double compareVideoTime() const;
    double comparisonVideoRate() const;
    QString comparisonAlignmentBasis() const;
    QString comparisonAlignmentConfidence() const;
    int comparisonGpsAnchors() const;
    bool videoMuted() const { return videoMuted_; }
    void setVideoMuted(bool muted);
    QStringList sessionDirectoriesList() const { return sessionDirs_; }

signals:
    void readyChanged();
    void loadingChanged();
    void lapLoadingChanged();
    void selectionChanged();
    void editingCornersChanged();
    void cursorFracChanged();
    void viewChanged();
    void sessionsChanged();
    void cornersChanged();
    void driverMappingsChanged();
    void channelHeightChanged();
    void channelConfigChanged();
    void referenceAlignmentChanged();
    void trackAtlasChanged();
    void videoTimeChanged();
    void videoMutedChanged();
    void videoMetadataChanged(const QString& videoPath);
    void standaloneVideoRequested(const QUrl& source);

private:
    SessionHandle* findSession(const QString& key) const;
    void setPrimary(SessionHandle* session, int lapId);
    void setCompare(SessionHandle* session, int lapId);
    void requestLapLoad(SessionHandle* session, int lapId, bool compare);
    void startNextFileOpen();
    void setPrimaryLapLoading(bool loading);
    void setCompareLapLoading(bool loading);
    QString sessionIndexCachePath() const;
    void startSessionScan();
    void finishSessionScan();
    void loadPreferences();
    QString driverDisplay(const SessionHandle* session) const;
    QString assignedTrackSlug(const SessionHandle* session) const;
    QString detectedAtlasSlug(const SessionHandle* session) const;
    QString resolvedTrackSlug(const SessionHandle* session) const;
    QString displayTrack(const SessionHandle* session) const;
    static QString trackAssignmentKey(const SessionHandle* session);
    void savePreferences();
    void loadChannelsConfig();

    void loadCornersForPrimary();
    QVector<CornerZone> atlasCornersForPrimary() const;
    bool parseTrackAtlas(const QByteArray& payload);
    void loadTrackAtlasCache();
    void updateTrackAtlas(bool force);
    QString trackAtlasCachePath() const;
    void invalidateComparisonAlignment();
    void rebuildComparisonAlignment();
    double compareTimeForPrimaryFraction(double fraction) const;
    double compareFractionForPrimaryFraction(double fraction) const;
    QStringList sessionDirs_;
    QVariantList fileSources_;
    QHash<QString, QVariantMap> fileMetadata_;
    QFutureWatcher<std::shared_ptr<SessionScanResult>>* scanWatcher_ = nullptr;
    QSet<QString> scanExtraPaths_;
    QHash<QString, QString> driverMappings_;
    QHash<QString, QString> trackAssignments_;
    QHash<QString, QVariantMap> recordingMetadata_;
    QStringList trackMetadataPaths_;
    QString lastPrimaryKey_;
    QString lastCompareKey_;
    int lastPrimaryLap_ = -1;
    int lastCompareLap_ = -1;
    std::vector<std::unique_ptr<SessionHandle>> sessions_;
    SessionHandle* primarySession_ = nullptr;
    QStringList pendingFileOpens_;
    quint64 primaryLoadGeneration_ = 0;
    quint64 compareLoadGeneration_ = 0;
    SessionHandle* compareSession_ = nullptr;
    int primaryLap_ = -1;
    int compareLap_ = -1;

    double cursorFrac_ = 0.5;
    double viewStart_ = 0.0;
    double viewEnd_ = 1.0;
    int channelHeight_ = 110;
    bool editingCorners_ = false;
    bool ready_ = false;
    bool loading_ = false;
    bool rescanPending_ = false;
    bool primaryLapLoading_ = false;
    bool compareLapLoading_ = false;
    bool fileOpenLoading_ = false;
    bool videoMuted_ = false;
    double referenceAlignment_ = 0.0;
    QSet<QString> closedTracks_;

    QHash<QString, QJsonObject> atlasTracks_;
    QString trackAtlasStatus_;
    QNetworkAccessManager* atlasNetwork_ = nullptr;
    QTimer* atlasTimer_ = nullptr;
    QVector<CornerZone> corners_;
    mutable QHash<QString, bool> channelVisible_;
    mutable QHash<QString, QColor> channelColors_;
    mutable QHash<QString, double> channelWeights_;
    mutable QHash<QString, std::shared_ptr<std::vector<double>>>
        extraChannelCache_;
    QHash<QString, QString> driverAliases_;
    mutable std::vector<CornerGraph> cornerGraphs_;
    QStringList channelOrder_;
    mutable QVector<double> deltaCache_;
    mutable DamperAlignment damperAlignment_;
    mutable bool damperAlignmentValid_ = false;
    mutable bool deltaCacheValid_ = false;
    QVector<double> comparisonAlignmentTime_;
    QVector<double> comparisonAlignmentFraction_;
    QString comparisonAlignmentBasis_;
    int comparisonGpsAnchors_ = 0;

    friend class TraceView;
    friend class VideoTelemetryHud;
};
