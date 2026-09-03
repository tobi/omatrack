// Qt object model bridging the omatrack core engine to QML.
//
// Mirrors omatrack's TelemetryStore architecture: lazy per-file
// SessionHandles, Track→Date→Session→Laps grouping, primary/compare lap
// selection, cursor + viewport state, corner zones, and channel display
// configuration.

#pragma once

#include "AsyncJob.h"
#include "LibraryModel.h"
#include "StoreModels.h"
#include "UsbCopy.h"
// Extracted collaborators (plain QObjects owned by the store).
class PreferencesStore;
class TrackAtlasManager;
class OverlayManager;

#include <QByteArray>
#include <QColor>
#include "core/TelemetryEngine.h"
#include <QObject>
#include <QSet>
#include <QThreadPool>
#include <QStringList>
#include <QHash>
#include <QJsonObject>
#include <QPointF>
#include <QVector>
#include <QUrl>
#include <QtQml/qqmlregistration.h>
#include <QVariantList>
#include <QVariantMap>

#include <atomic>
#include <memory>
#include <cmath>
#include <limits>
#include <vector>
#include <vector>

class SessionHandle;
class TelemetryStore;
class QNetworkAccessManager;
namespace omatrack {
class PluginHost;
struct PluginSamplesResult;
}  // namespace omatrack
class QTimer;
class QFileSystemWatcher;
class QQmlEngine;
class QJSEngine;
struct SessionScanResult;
struct SessionLapLoadResult;
struct FileOpenResult;
struct FolderChannelSample;
struct SessionConfidenceLoadResult;
struct SidebarMetadataResult;
struct CornerConsistencyLoadResult;
struct TraceSnapshot;
enum class ComparisonAlignmentStrategy;

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

// Central trace envelope for the fastest half of the other representative
// laps in the active session. Samples are already mapped onto the primary
// lap's track-station grid, so the renderer only performs pixel decimation.
struct TraceConfidenceBand {
    std::vector<double> lower;
    std::vector<double> median;
    std::vector<double> upper;
    int lapCount = 0;

    bool valid() const {
        return lapCount >= 2 && lower.size() > 1 &&
               lower.size() == median.size() && lower.size() == upper.size();
    }
};

// ── corner focus ────────────────────────────────────────────────────

// One driver-facing event inside a focused corner, placed on the primary
// lap's fraction axis. `referenceFraction` is the reference lap's own event
// mapped back onto the primary distance axis, so both markers can be drawn
// against the same zoomed viewport; it is negative when there is no
// reference lap.
struct CornerMarker {
    QString key;
    QString label;
    double fraction = 0.0;
    double referenceFraction = -1.0;
};
struct CornerConsistencyState {
    QString key;
    bool loading = false;
    int lapCount = 0;
    int validLapCount = 0;
    int brakingLapCount = 0;
    double medianBrakePoint = std::numeric_limits<double>::quiet_NaN();
    double brakePointStdDev = std::numeric_limits<double>::quiet_NaN();
    double brakePointRange = std::numeric_limits<double>::quiet_NaN();
};

// ── corner zone ─────────────────────────────────────────────────────

struct CornerZone {
    QString name;
    double start = 0.0;  // 0-1 lap fraction
    double end = 0.0;
    double mid() const { return (start + end) / 2.0; }
    bool operator==(const CornerZone& other) const {
        return name == other.name && std::abs(start - other.start) < 1.0e-9 &&
               std::abs(end - other.end) < 1.0e-9;
    }
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
    /// Presentation-order frame at this lap's first telemetry sample.
    std::optional<std::uint64_t> firstVideoFrame;
    /// Representative timed racing lap: eligible for best-lap statistics.
    bool countsForBest() const { return isComplete && !isPitLap; }
};

enum class VideoIdentityStatus {
    NotChecked,
    ExactSource,
    VerifiedHash,
    TrustedRemoteObject,
    Unverified,
    Mismatch,
};

struct VideoIdentityResult {
    VideoIdentityStatus status = VideoIdentityStatus::NotChecked;
    std::optional<std::uint32_t> fileIndex;
    QString warning;

    bool trusted() const {
        return status == VideoIdentityStatus::ExactSource ||
               status == VideoIdentityStatus::VerifiedHash ||
               status == VideoIdentityStatus::TrustedRemoteObject;
    }
};

/// Outcome of a background TRACK.yml write plus the re-read inherited
/// metadata of the recordings beneath that folder.
struct FolderMetadataWrite {
    QString error;
    QHash<QString, QVariantMap> refreshed;
};

struct SidebarPin {
    QString kind;
    QString path;
};

/// Session-length HUD strip sampled from the native recording. Numeric
/// readouts may follow video time on this clock when no lap is loaded.
/// Compared pedal traces use the selected UnifiedLap and the track-station
/// map, not this series.
struct VideoHudSeries {
    std::vector<double> time;
    std::vector<double> speed;
    std::vector<double> throttle;
    std::vector<double> brake;
    std::vector<double> steering;
    std::vector<double> gear;
    std::vector<double> fuel;
    double duration = 0.0;
    bool empty() const { return time.size() < 2; }
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
                           const QJsonObject& cachedMetadata = {},
                           const QString& telemetryPath = {});
    ~SessionHandle();

    const QString& path() const { return path_; }
    const QString& telemetryPath() const { return telemetryPath_; }
    void setTelemetryPath(const QString& path) { telemetryPath_ = path; }
    QString stem() const;

    const QVector<LapEntry>& laps() const { return laps_; }
    std::shared_ptr<const omatrack::UnifiedLap> unifiedLap(int lapId) const;
    void clearUnifiedCache() { unifiedCache_.clear(); }
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
    bool loadChannelSummaryForIndex();
    bool loadSummaryForOpen(QString* errorString = nullptr);
    QJsonObject metadataForCache() const;
    void adoptLoadedLap(int lapId,
                        std::unique_ptr<omatrack::TelemetrySource> source,
                        std::shared_ptr<const omatrack::UnifiedLap> unified,
                        double driverId = 0.0, bool forceDriverId = false,
                        const VideoIdentityResult& videoIdentity = {});
    void setVideoClock(const omatrack::VideoClock& clock,
                       const VideoIdentityResult& identity = {});
    const omatrack::VideoClock& videoClock() const { return videoClock_; }
    const VideoIdentityResult& videoIdentity() const { return videoIdentity_; }
    std::optional<double> videoPresentationTime(double fileRelativeTime) const;
    std::optional<double> videoTelemetryTime(double presentationTime) const;
    const VideoHudSeries& videoHud() const { return videoHud_; }
    qint64 utcStartNs() const { return utcStartNs_; }
    /// Exclusive file-relative duration of the session window (laps / HUD).
    qint64 durationNs() const;
    qint64 startNs() const;

private:
    void captureVideoHud(const omatrack::TelemetrySource& source);
    void applyCachedMetadata(const QJsonObject& metadata);
    void ensureLapSummary();
    void applyEventDriverId(double eventDriverId, bool force = false);
    void captureGpsLocation(const omatrack::TelemetrySource& source);
    void captureSourceChannels(const omatrack::TelemetrySource& source);
    void populateLaps(const std::vector<omatrack::Lap>& detected);
    QString path_;
    QString telemetryPath_;
    omatrack::VideoClock videoClock_;
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
    VideoIdentityResult videoIdentity_;
    VideoHudSeries videoHud_;
    qint64 utcStartNs_ = -1;
};

// MTX overlay: one sidecar file is one folder of extra traces / spans.

struct OverlayChrome {
    QString kind;
    QString text;
    QString label;
    QString value;
};

struct OverlaySpan {
    qint64 startHostNs = 0;
    qint64 endHostNs = 0;
    bool visible = true;
    QString name;
    QString title;
    QString subtitle;
    QColor color;
    QVariantList meta;
};

/// One Gantt lane: every span that shares `n` (a car, a sleep series, …).
struct OverlaySpanLane {
    QString key;
    QString name;
    bool visible = true;
};

struct OverlayChannel {
    QString key;
    QString name;
    QString unit;
    bool defaultVisible = true;
    qint64 t0HostNs = 0;
    qint64 periodNs = 0;
    std::shared_ptr<std::vector<double>> samples;
    /// Explicit host-relative sample instants (plugin series). When set, the
    /// channel is resampled from memory by interpolation instead of through
    /// the sidecar file.
    std::shared_ptr<std::vector<qint64>> times;
};

struct OverlayGroup {
    QString path;
    QString id;
    /// Non-empty for a Lua plugin group: `path` is then `plugin:<id>`.
    QString pluginId;
    QString name;
    QString timezone;
    bool expanded = true;
    QVector<OverlayChrome> chrome;
    QVector<OverlaySpan> spans;
    QVector<OverlaySpanLane> spanLanes;
    QVector<OverlayChannel> channels;
    qint64 utcStartNs = -1;
    qint64 durationNs = 0;
    qint64 shiftNs = 0;
};

// ── store (root model exposed to QML) ───────────────────────────────

class TelemetryStore : public QObject {
    Q_OBJECT
    QML_NAMED_ELEMENT(Store)
    QML_SINGLETON
    Q_PROPERTY(bool ready READ ready NOTIFY readyChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    Q_PROPERTY(bool fileOpenLoading READ fileOpenLoading NOTIFY fileOpenChanged)
    Q_PROPERTY(QString fileOpenPath READ fileOpenPath NOTIFY fileOpenChanged)
    Q_PROPERTY(bool lapLoading READ lapLoading NOTIFY lapLoadingChanged)
    Q_PROPERTY(bool traceConfidenceMode READ traceConfidenceMode WRITE
                   setTraceConfidenceMode NOTIFY traceConfidenceChanged)
    Q_PROPERTY(bool traceConfidenceLoading READ traceConfidenceLoading NOTIFY
                   traceConfidenceChanged)
    Q_PROPERTY(int traceConfidenceLapCount READ traceConfidenceLapCount NOTIFY
                   traceConfidenceChanged)
    Q_PROPERTY(bool comparing READ comparing NOTIFY selectionChanged)
    Q_PROPERTY(bool editingCorners READ editingCorners WRITE setEditingCorners
                   NOTIFY editingCornersChanged)
    Q_PROPERTY(int cornerCount READ cornerCount NOTIFY cornersChanged)
    Q_PROPERTY(int focusedCorner READ focusedCorner NOTIFY cornerFocusChanged)
    Q_PROPERTY(
        QString highlightedCornerMarker READ highlightedCornerMarker WRITE
            setHighlightedCornerMarker NOTIFY highlightedCornerMarkerChanged)
    Q_PROPERTY(double cursorFrac READ cursorFrac WRITE setCursorFrac NOTIFY
                   cursorFracChanged)
    Q_PROPERTY(bool hasGpsData READ hasGpsData NOTIFY selectionChanged)
    Q_PROPERTY(
        double viewStart READ viewStart WRITE setViewStart NOTIFY viewChanged)
    Q_PROPERTY(double viewEnd READ viewEnd WRITE setViewEnd NOTIFY viewChanged)
    Q_PROPERTY(QString primaryLabel READ primaryLabel NOTIFY selectionChanged)
    Q_PROPERTY(QString primaryDetail READ primaryDetail NOTIFY selectionChanged)
    Q_PROPERTY(QString primaryDriverName READ primaryDriverName NOTIFY
                   selectionChanged)
    Q_PROPERTY(QString primaryDriverMappingKey READ primaryDriverMappingKey
                   NOTIFY selectionChanged)
    Q_PROPERTY(QString primaryMetadataPath READ primaryMetadataPath NOTIFY
                   selectionChanged)
    Q_PROPERTY(bool primaryMetadataFolderScope READ primaryMetadataFolderScope
                   NOTIFY selectionChanged)
    Q_PROPERTY(QString compareLabel READ compareLabel NOTIFY selectionChanged)
    Q_PROPERTY(QString roomName READ roomName NOTIFY selectionChanged)
    Q_PROPERTY(QString primarySessionKey READ primarySessionKey NOTIFY
                   selectionChanged)
    Q_PROPERTY(int primaryLapIndex READ primaryLapIndex NOTIFY selectionChanged)
    Q_PROPERTY(
        int primaryLapOrdinal READ primaryLapOrdinal NOTIFY selectionChanged)
    Q_PROPERTY(int primaryLapTotal READ primaryLapTotal NOTIFY selectionChanged)
    Q_PROPERTY(
        QString primaryFuelLoad READ primaryFuelLoad NOTIFY cursorFracChanged)
    Q_PROPERTY(QString compareDriverName READ compareDriverName NOTIFY
                   selectionChanged)
    Q_PROPERTY(
        int compareLapOrdinal READ compareLapOrdinal NOTIFY selectionChanged)
    Q_PROPERTY(int compareLapTotal READ compareLapTotal NOTIFY selectionChanged)
    Q_PROPERTY(
        QString compareFuelLoad READ compareFuelLoad NOTIFY cursorFracChanged)
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
    Q_PROPERTY(QString primaryVideoSyncWarning READ primaryVideoSyncWarning
                   NOTIFY selectionChanged)
    Q_PROPERTY(QString compareVideoSyncWarning READ compareVideoSyncWarning
                   NOTIFY selectionChanged)
    /// One line about the recording being fetched for offline use, empty when
    /// none is. Gigabytes take minutes, so this is the only honest way to show
    /// that something is happening.
    Q_PROPERTY(QString videoDownloadStatus READ videoDownloadStatus NOTIFY
                   videoDownloadChanged)
    /// Local primary/reference pace ratio around the cursor. Playback uses
    /// `referencePlaybackRate()`, not this value.
    Q_PROPERTY(double comparisonVideoRate READ comparisonVideoRate NOTIFY
                   videoTimeChanged)
    Q_PROPERTY(QString comparisonAlignmentBasis READ comparisonAlignmentBasis
                   NOTIFY selectionChanged)
    Q_PROPERTY(QString comparisonAlignmentConfidence READ
                   comparisonAlignmentConfidence NOTIFY selectionChanged)
    Q_PROPERTY(int comparisonGpsAnchors READ comparisonGpsAnchors NOTIFY
                   selectionChanged)
    Q_PROPERTY(
        QString comparisonSyncStrategy READ comparisonSyncStrategy WRITE
            setComparisonSyncStrategy NOTIFY comparisonSyncStrategyChanged)
    Q_PROPERTY(
        QAbstractItemModel* comparisonSyncStrategies READ
            comparisonSyncStrategiesModel NOTIFY comparisonSyncStrategyChanged)
    Q_PROPERTY(
        LapListModel* primaryLaps READ primaryLapsModel NOTIFY selectionChanged)
    Q_PROPERTY(
        LapListModel* compareLaps READ compareLapsModel NOTIFY selectionChanged)
    Q_PROPERTY(FilmstripSessionListModel* filmstripSessions READ
                   filmstripSessionsModel NOTIFY selectionChanged)
    Q_PROPERTY(QAbstractItemModel* channels READ channelsModel NOTIFY
                   channelConfigChanged)
    Q_PROPERTY(
        QAbstractItemModel* corners READ cornersModel NOTIFY cornersChanged)
    Q_PROPERTY(QAbstractItemModel* driverMappings READ driverMappingsModel
                   NOTIFY driverMappingsChanged)
    Q_PROPERTY(QAbstractItemModel* library READ libraryModel CONSTANT)
    Q_PROPERTY(bool videoMuted READ videoMuted WRITE setVideoMuted NOTIFY
                   videoMutedChanged)
    Q_PROPERTY(
        QStringList recentFiles READ recentFiles NOTIFY recentFilesChanged)
    Q_PROPERTY(
        bool eventMode READ eventMode WRITE setEventMode NOTIFY eventChanged)
    Q_PROPERTY(QString eventTrack READ eventTrack WRITE setEventTrack NOTIFY
                   eventChanged)
    Q_PROPERTY(QString eventSession READ eventSession WRITE setEventSession
                   NOTIFY eventChanged)
    Q_PROPERTY(
        QString eventDate READ eventDate WRITE setEventDate NOTIFY eventChanged)
    Q_PROPERTY(bool usbPresent READ usbPresent NOTIFY usbChanged)
    Q_PROPERTY(QString usbLabel READ usbLabel NOTIFY usbChanged)
    Q_PROPERTY(bool usbCopyVisible READ usbCopyVisible NOTIFY usbChanged)
    Q_PROPERTY(QString usbCopyStatus READ usbCopyStatus NOTIFY usbChanged)
    Q_PROPERTY(double usbCopyProgress READ usbCopyProgress NOTIFY usbChanged)
    Q_PROPERTY(bool usbCopyBusy READ usbCopyBusy NOTIFY usbChanged)
    Q_PROPERTY(bool usbPreviewLoading READ usbPreviewLoading NOTIFY usbChanged)
    Q_PROPERTY(int usbCopyReadyCount READ usbCopyReadyCount NOTIFY usbChanged)
    Q_PROPERTY(
        int usbCopyInvalidCount READ usbCopyInvalidCount NOTIFY usbChanged)
    Q_PROPERTY(QString usbCopySummary READ usbCopySummary NOTIFY usbChanged)
    Q_PROPERTY(QString usbCopyTarget READ usbCopyTarget NOTIFY usbChanged)
    Q_PROPERTY(QString manualUsbSource READ manualUsbSource NOTIFY usbChanged)
    Q_PROPERTY(UsbCopyListModel* usbCopyModel READ usbCopyModel CONSTANT)
    Q_PROPERTY(QString usbDest READ usbDest WRITE setUsbDest NOTIFY usbChanged)
    Q_PROPERTY(
        QString usbFormat READ usbFormat WRITE setUsbFormat NOTIFY usbChanged)
    Q_PROPERTY(QString usbRenameScript READ usbRenameScript WRITE
                   setUsbRenameScript NOTIFY usbChanged)
    Q_PROPERTY(QString overlayRefColor READ overlayRefColor WRITE
                   setOverlayRefColor NOTIFY overlayStyleChanged)
    Q_PROPERTY(QString overlayRefStyle READ overlayRefStyle WRITE
                   setOverlayRefStyle NOTIFY overlayStyleChanged)
    Q_PROPERTY(bool overlayRefWhite READ overlayRefWhite WRITE
                   setOverlayRefWhite NOTIFY overlayStyleChanged)
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
    bool eventMode() const;
    QString eventTrack() const;
    QString eventSession() const;
    QString eventDate() const;
    Q_INVOKABLE void setEventMode(bool enabled);
    Q_INVOKABLE void setEventTrack(const QString& track);
    Q_INVOKABLE void setEventSession(const QString& session);
    Q_INVOKABLE void setEventDate(const QString& date);
    /// "track · date · session" from the non-empty event fields, or empty
    /// when nothing is configured. Shared by the title-bar and sidebar
    /// event state so the two cannot describe it differently.
    Q_INVOKABLE QString eventSummary() const;
    bool usbPresent() const { return usbPresent_; }
    QString usbLabel() const { return usbLabel_; }
    QString manualUsbSource() const { return manualUsbSource_; }
    bool usbCopyVisible() const { return usbCopyVisible_; }
    QString usbCopyStatus() const { return usbCopyStatus_; }
    double usbCopyProgress() const { return usbCopyProgress_; }
    bool usbCopyBusy() const { return usbCopyJob_.running(); }
    bool usbPreviewLoading() const { return usbCopyPlanJob_.running(); }
    int usbCopyReadyCount() const { return usbCopyPlan_.ready; }
    int usbCopyInvalidCount() const { return usbCopyPlan_.invalid; }
    QString usbCopySummary() const;
    QString usbCopyTarget() const { return usbCopyPlan_.options.destination; }
    UsbCopyListModel* usbCopyModel() const { return usbCopyModel_.get(); }
    QString usbDest() const;
    QString usbFormat() const;
    QString usbRenameScript() const;
    Q_INVOKABLE void setUsbDest(const QString& dest);
    Q_INVOKABLE void setUsbFormat(const QString& format);
    Q_INVOKABLE void setUsbRenameScript(const QString& script);
    Q_INVOKABLE void showUsbCopy();
    Q_INVOKABLE void hideUsbCopy();
    /// Manual copy source for machines without detectable USB volumes
    /// (non-Linux, unusual mounts): scanned and watched like a USB volume,
    /// never added to library locations. Empty when unset.
    Q_INVOKABLE void setManualUsbSource(const QString& dirPath);
    Q_INVOKABLE void clearManualUsbSource();
    Q_INVOKABLE void copyUsbFiles();
    Q_INVOKABLE void cancelUsbCopy();
    Q_INVOKABLE QString luaRenameExample() const;
    QString overlayRefColor() const;
    QString overlayRefStyle() const;
    bool overlayRefWhite() const;
    Q_INVOKABLE void setOverlayRefColor(const QString& color);
    Q_INVOKABLE void setOverlayRefStyle(const QString& style);
    Q_INVOKABLE void setOverlayRefWhite(bool enabled);
    Q_INVOKABLE void addSessionDirectory(const QString& dirPath);
    /// Queue one telemetry source for indexing and lap loading. Ordinary video
    /// is reported through standaloneVideoRequested after background probing.
    Q_INVOKABLE void openFile(const QString& filePath);
    /// Cancels the current background file open and drops queued opens. The
    /// parser worker is cooperative where possible; if a vendor decoder is
    /// already inside an uninterruptible read, its result is discarded when
    /// it returns and the UI remains usable.
    Q_INVOKABLE void cancelFileOpen();
    /// True when `path` is an MTX sidecar name (`.ext.jsonl` / `.mtx.jsonl`).
    Q_INVOKABLE bool isMtxSidecarPath(const QString& filePath) const;
    /// Drop or File > Open of an MTX sidecar: overlap-join onto the open
    /// lap / video / traces, then append as a collapsible folder.
    Q_INVOKABLE void attachSidecar(const QString& filePath);
    Q_INVOKABLE void removeOverlay(const QString& id);
    Q_INVOKABLE void setOverlayExpanded(const QString& id, bool expanded);
    Q_INVOKABLE bool overlayExpanded(const QString& id) const;
    /// Sidecars discovered in configured folders that overlap the active
    /// session's time window. Attachments are session state, not preferences.
    Q_INVOKABLE QVariantList sidecarLibrary() const;
    /// Lua trace-group plugins (PluginHost). Rows: id, name, version,
    /// directory, status, error, channelCount, enabled, loading.
    Q_INVOKABLE QVariantList pluginLibrary() const;
    Q_INVOKABLE void setPluginEnabled(const QString& id, bool enabled);
    Q_INVOKABLE void reloadPlugins();
    Q_INVOKABLE QString pluginDirectory() const;
    /// Example plugins bundled as resources (`:/plugins/<id>/plugin.lua`).
    Q_INVOKABLE QStringList examplePlugins() const;
    /// Copy a bundled example into the plugin folder; never overwrites an
    /// existing plugin.lua. Returns a user-facing status line and rescans.
    Q_INVOKABLE QString installExamplePlugin(const QString& id);
    Q_INVOKABLE void openPluginDirectory();

    const QVector<OverlayGroup>& overlayGroups() const;
    Q_INVOKABLE bool directoryExists(const QString& dirPath) const;
    /// Native path for a `file:` URL from a dialog or a drop. Platform
    /// dialogs hand back `file:///C:/Users/…` on Windows, which a string
    /// strip of `file://` turns into `/C:/Users/…`; `QUrl::toLocalFile()`
    /// is the only correct decoder. Takes a string so a typed `C:\…` path
    /// is never parsed as a URL with scheme `c`; anything that is not a
    /// `file:` URL passes through unchanged.
    Q_INVOKABLE QString localPathFromUrl(const QString& value) const;
    /// `defaultTelemetryDirectory()` as a `file:` URL for dialog `folder`.
    Q_INVOKABLE QUrl defaultTelemetryDirectoryUrl() const;
    /// The default telemetry library folder: the platform's Documents
    /// location plus `/Telemetry` (honors Windows OneDrive redirection),
    /// created if it does not exist so a fresh install never lands in a
    /// missing directory.
    Q_INVOKABLE QString defaultTelemetryDirectory() const;
    Q_INVOKABLE QString configFilePath() const;
    QStringList recentFiles() const;
    Q_INVOKABLE void removeSessionDirectory(const QString& dirPath);
    /// Enabled local folders, including the cache directories that stand in
    /// for connected servers. This is the scan input.
    Q_INVOKABLE QStringList sessionDirectories() const;
    /// Build the enriched file-source tree for the library model (internal).
    QVariantList buildFileSourceTree() const;

    // ── Library locations ────────────────────────────────────────────────
    // One ordered list of folders and server connections. Every row carries
    // `type`, `name`, `target`, `enabled`, plus the live `status`/`detail`
    // and `fileCount` from the last scan.
    Q_INVOKABLE QVariantList libraryLocations() const;
    /// The connection kinds offered by the "Connect" menu. Each row carries
    /// {type, label, placeholder, needsCredentials, detail}, and optionally
    /// the credential-field labels and an `extraFields` list of protocol
    /// settings — which is what keeps ConnectionDialog free of protocols.
    Q_INVOKABLE QVariantList connectionTypes() const;
    /// What every connection has downloaded, as {bytes, text} for what the
    /// limit governs and {videoBytes, videoText} for the recordings kept for
    /// offline use, which it does not. Measured by walking the cache, not by
    /// adding up index entries, so it also counts what a removed location or
    /// an interrupted download left behind.
    Q_INVOKABLE QVariantMap cacheUsage() const;
    /// Whether `path` is a recording on a server and, if so, whether it is
    /// being kept locally: {remote, offline, busy}. What the context menu on a
    /// file needs to know to offer the right thing.
    Q_INVOKABLE QVariantMap videoOffline(const QString& path) const;
    /// Keeps `path` on this machine, or gives the space back. Downloading
    /// happens in the background and is reported through videoDownloadStatus;
    /// asking for one already queued cancels nothing and starts nothing.
    Q_INVOKABLE void setVideoOffline(const QString& path, bool offline);
    /// Abandons the download in progress and everything queued behind it.
    Q_INVOKABLE void cancelVideoDownloads();
    /// A freshly signed address for a recording whose last one stopped
    /// working — the answer to MpvVideoItem's sourceExpired(). Empty when the
    /// player is holding something this store never produced.
    Q_INVOKABLE QUrl refreshedVideoSource(const QUrl& source) const;
    /// Deletes every downloaded file. Nothing is lost that the servers cannot
    /// send again, but everything still configured has to be fetched afresh.
    Q_INVOKABLE void clearCache();
    /// Saves a new connection or updates an existing one when `id` is set.
    /// Returns an empty string on success, otherwise the reason it failed.
    Q_INVOKABLE QString saveConnection(const QVariantMap& fields);
    Q_INVOKABLE void removeLocation(const QString& id);
    Q_INVOKABLE void setLocationEnabled(const QString& id, bool enabled);
    Q_INVOKABLE void setLocationName(const QString& id, const QString& name);
    /// Moves a location within the list so users can order their library.
    Q_INVOKABLE void moveLocation(const QString& id, int delta);
    Q_INVOKABLE void requestSidebarMetadata(const QString& path, bool visible);
    Q_INVOKABLE void copyFilePath(const QString& path) const;
    /// Connection id whose cache contains `path`, or empty for a local file.
    Q_INVOKABLE QString locationIdForPath(const QString& path) const;
    Q_INVOKABLE void openContainingFolder(const QString& path) const;
    Q_INVOKABLE bool filePinned(const QString& role, const QString& path) const;
    Q_INVOKABLE void setFilePinned(const QString& role, const QString& path,
                                   bool pinned);
    Q_INVOKABLE SessionInfoRow sessionInfo(const QString& sessionKey) const;
    /// First session key in track→date→session order, or empty.
    Q_INVOKABLE QString firstSessionKey() const;
    /// All session keys in track→date→session order (for AutotestHarness).
    QStringList sessionKeys() const;
    /// All file paths in the library tree (for AutotestHarness).
    QStringList libraryFilePaths() const;
    Q_INVOKABLE void clearSessions();
    Q_INVOKABLE void refreshTrackAtlas();
    Q_INVOKABLE QVariantList trackAtlasChoices() const;
    Q_INVOKABLE QString
    detectedTrackForSession(const QString& sessionKey) const;
    Q_INVOKABLE QString
    assignedTrackForSession(const QString& sessionKey) const;
    Q_INVOKABLE void setSessionTrackAssignment(const QString& sessionKey,
                                               const QString& atlasSlug);
    Q_INVOKABLE QVariantMap folderMetadata(const QString& folderPath) const;
    Q_INVOKABLE void sampleFolderChannels(const QString& folderPath);
    Q_INVOKABLE bool saveFolderMetadata(const QString& folderPath,
                                        const QVariantMap& metadata);
    Q_INVOKABLE QVariantMap videoMetadata(const QString& videoPath) const;
    Q_INVOKABLE bool saveVideoMetadata(const QString& videoPath,
                                       const QVariantMap& metadata);

    // ── selection ──────────────────────────────────────────────────
    Q_INVOKABLE void selectSession(const QString& sessionKey, bool compare);
    Q_INVOKABLE void selectLap(const QString& sessionKey, int lapId);
    Q_INVOKABLE void compareLap(const QString& sessionKey, int lapId);
    /// Transient hover preview of a filmstrip lap. Traces, delta, and cursor
    /// data render the peeked lap at the current cursor while the committed
    /// selection, viewport, and alignment tuning stay untouched. The peeked
    /// lap loads in the background when it is not cached yet.
    Q_INVOKABLE void peekLap(const QString& sessionKey, int lapId,
                             bool compare);
    Q_INVOKABLE void clearPeek();
    Q_INVOKABLE void clearCompare();
    Q_INVOKABLE void clearPrimary();
    /// Swap primary and reference analysis roles. No-op without a reference.
    Q_INVOKABLE bool swapPrimaryWithReference();
    /// Next lap in recording order after the primary, or -1 at the last lap.
    Q_INVOKABLE int nextPrimaryLapId() const;
    Q_INVOKABLE QString lapLabel(const QString& sessionKey, int lapId) const;
    Q_INVOKABLE QString lapTimeText(const QString& sessionKey, int lapId) const;
    /// Load a lap into the session cache without changing the selection.
    Q_INVOKABLE void prefetchLap(const QString& sessionKey, int lapId);
    Q_INVOKABLE int bestLapIdForSession(const QString& sessionKey) const;
    Q_INVOKABLE ActiveSessionRoles activeSessionRoles() const;
    QVector<LapRow> lapRowsForSession(const QString& sessionKey) const;
    Q_INVOKABLE bool traceConfidenceIncludesLap(const QString& sessionKey,
                                                int lapId) const;

    // ── navigation ─────────────────────────────────────────────────
    Q_INVOKABLE void zoomAt(double anchorFrac, double factor);
    Q_INVOKABLE void pan(double deltaFrac);
    Q_INVOKABLE void moveCursorSteps(int steps);
    Q_INVOKABLE void seekCursorSeconds(double seconds);
    Q_INVOKABLE void setCursorFromVideoTime(double mediaTime);
    Q_INVOKABLE void jumpToFraction(double frac);
    /// Primary-relative fraction mapped onto the reference lap through the
    /// shared comparison alignment. Feeds the filmstrip's reference-row
    /// playhead marker.
    Q_INVOKABLE double compareFractionForPrimaryFraction(double fraction) const;
    /// Full-lap viewport; also leaves corner focus.
    Q_INVOKABLE void resetView();
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
    Q_INVOKABLE void setCornerName(int index, const QString& name);
    Q_INVOKABLE QString cornerName(int index) const;
    Q_INVOKABLE double cornerStart(int index) const;
    Q_INVOKABLE double cornerEnd(int index) const;
    int cornerCount() const { return corners_.size(); }
    // Corner focus: the workspace zooms onto one corner instead of opening a
    // second window. focusCorner() remembers the viewport it replaced,
    // clearCornerFocus() puts it back.
    Q_INVOKABLE void focusCorner(int index);
    Q_INVOKABLE void focusCornerAtCursor();
    Q_INVOKABLE void clearCornerFocus();
    Q_INVOKABLE CornerFocusSummary cornerFocusSummary() const;
    int focusedCorner() const { return focusedCorner_; }
    QString highlightedCornerMarker() const { return highlightedCornerMarker_; }
    Q_INVOKABLE void setHighlightedCornerMarker(const QString& key);
    /// Unified lap immediately before (-1) or after (+1) the active lap in the
    /// same session. Corner focus can frame a corner near start/finish so the
    /// viewport runs past the lap bounds; this is what fills that space.
    /// Never parses on the calling thread: returns nullptr while the
    /// background prefetch is still running, and stays null when there is no
    /// such lap.
    const omatrack::UnifiedLap* neighbourUnified(int offset) const;
    /// "lap 6" for an available neighbour, empty when there is nothing there.
    Q_INVOKABLE QString neighbourLabel(int offset) const;
    /// Brake / turn-in / apex / throttle-pickup events of the focused corner,
    /// on the primary lap's fraction axis. Empty when nothing is focused.
    const QVector<CornerMarker>& cornerMarkers() const { return markers_; }
    Q_INVOKABLE void updateCorner(int index, double start, double end);
    Q_INVOKABLE int addCorner(double start, double end);
    Q_INVOKABLE void deleteCorner(int index);
    Q_INVOKABLE void setEditingCorners(bool editing);
    Q_INVOKABLE void beginCornerEdit();
    Q_INVOKABLE void commitCornerEdit();
    Q_INVOKABLE void cancelCornerEdit();

    // ── channel config ─────────────────────────────────────────────
    Q_INVOKABLE bool channelVisible(const QString& key) const;
    Q_INVOKABLE void setChannelVisible(const QString& key, bool visible);

    Q_INVOKABLE QString channelColor(const QString& key) const;
    Q_INVOKABLE void setChannelColor(const QString& key, const QString& color);
    Q_INVOKABLE QString channelExample(const QString& key);
    Q_INVOKABLE double channelWeight(const QString& key) const;
    Q_INVOKABLE void setChannelWeight(const QString& key, double weight);

    Q_INVOKABLE QStringList channelOrder() const;

    // ── driver mappings / user preferences ──────────────────────────

    Q_INVOKABLE void setDriverMapping(const QString& key,
                                      const QString& display);
    Q_INVOKABLE QString driverDisplayName(const QString& sessionKey) const;

    // ── data access for the trace canvas ───────────────────────────
    const omatrack::UnifiedLap* primaryUnified() const;
    const omatrack::UnifiedLap* compareUnified() const;
    const VideoHudSeries* primaryVideoHud() const;
    const SessionHandle* primarySession() const { return primarySession_; }
    const SessionHandle* compareSession() const { return compareSession_; }
    int primaryLapIndex() const { return primaryLap_; }
    int primaryLapOrdinal() const;
    int primaryLapTotal() const;
    QString primaryFuelLoad() const;
    QString compareDriverName() const;
    int compareLapOrdinal() const;
    int compareLapTotal() const;
    QString compareFuelLoad() const;
    double primaryFractionForVideoTime(double mediaTime) const;
    int compareLapIndex() const { return compareLap_; }
    const QVector<CornerZone>& corners() const { return corners_; }
    QString cornerNameAt(double frac) const;
    Q_INVOKABLE CursorReadout cursorReadout() const;
    /// Header / delta-bar refresh. Video playback emits this from a queued
    /// deadline job, not from every cursorFracChanged. Pointer/keyboard
    /// cursor motion still emits it immediately from setCursorFrac().
    void notifyCursorReadout() { emit cursorReadoutChanged(); }
    /// Channels window playhead samples. No-op in QML when the window is
    /// hidden; the video path only enqueues this at a low-priority deadline.
    void notifyChannelsCursorTick() { emit channelsCursorTick(); }
    /// Accumulated primary−reference time at the cursor station. NaN when
    /// there is no compare lap. Negative is ahead.
    Q_INVOKABLE double cursorTimeDelta() const;
    /// Primary−reference speed at the same station. NaN when either lap
    /// has no speed. Positive means the active car is faster right now.
    Q_INVOKABLE double cursorSpeedDelta() const;
    Q_INVOKABLE double sessionStartUnixTime() const;
    Q_INVOKABLE bool hasGlobalTime() const;
    // Per-sample Δ-time (primary vs compare, alignment-mapped; empty when no
    // compare). Shared by the trace canvas and cursor readout so the numeric Δ
    const QVector<double>& deltaTrace() const;
    /// Immutable per-frame view of the telemetry state the renderers need.
    /// Publishes the private alignment/clip helpers that TraceView and
    /// VideoTelemetryHud used to reach through `friend` declarations.
    TraceSnapshot traceSnapshot() const;
    const std::vector<double>* overlayChannelData(const QString& key) const;
    const std::vector<double>* extraChannelData(const QString& key,
                                                bool reference);
    const TraceConfidenceBand* traceConfidenceBand(const QString& field) const;
    const std::vector<double>& traceConsistency() const {
        return traceConsistency_;
    }
    bool trackAtlasReady() const;
    QString trackAtlasStatus() const;

    bool ready() const { return ready_; }
    bool loading() const { return loading_; }
    bool fileOpenLoading() const { return fileOpenLoading_; }
    QString fileOpenPath() const { return fileOpenPath_; }
    bool lapLoading() const {
        return primaryLapLoading_ || compareLapLoading_ || fileOpenLoading_;
    }
    bool comparing() const { return compareSession_ != nullptr; }
    bool traceConfidenceMode() const { return traceConfidenceMode_; }
    void setTraceConfidenceMode(bool enabled);
    bool traceConfidenceLoading() const { return traceConfidenceLoading_; }
    int traceConfidenceLapCount() const { return traceConfidenceLapCount_; }
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
    QString primaryLabel() const;
    QString primaryDetail() const;
    QString primaryDriverName() const;
    QString primaryDriverMappingKey() const;
    QString primaryMetadataPath() const;
    bool primaryMetadataFolderScope() const;
    bool hasGpsData() const;
    QString compareLabel() const;
    QString roomName() const;
    QString primarySessionKey() const;
    QString compareSessionKey() const;
    QUrl primaryVideoSource() const;
    /// Cache path for a player URL, or empty. Streamed sources are signed
    /// URLs; the library and metadata dialogs need the local stub path.
    Q_INVOKABLE QString localPathForVideoSource(const QUrl& source) const;
    double primaryVideoTime() const;
    // Reference-lap recording uses the same selected primary→reference
    // alignment as traces and delta, so every comparison view shares one
    // coordinate.
    QUrl compareVideoSource() const;
    double compareVideoTime() const;
    QString primaryVideoSyncWarning() const;
    QString compareVideoSyncWarning() const;
    double comparisonVideoRate() const;
    /// True when the primary cursor sits inside a corner range. The reference
    /// video holds 1× here so a turn is never time-warped.
    Q_INVOKABLE bool cursorInCorner() const;
    /// Playback speed for the reference recording. On a straight this is the
    /// rate that lands both videos at the next turn-in together; inside a
    /// corner it is 1. `refMediaTime` is the reference player's current
    /// position in seconds.
    Q_INVOKABLE double referencePlaybackRate(double refMediaTime) const;
    QString videoDownloadStatus() const { return videoDownloadStatus_; }
    QString comparisonAlignmentBasis() const;
    QString comparisonAlignmentConfidence() const;
    int comparisonGpsAnchors() const;
    QString comparisonSyncStrategy() const {
        return effectiveComparisonSyncStrategy_;
    }
    void setComparisonSyncStrategy(const QString& strategy);
    Q_INVOKABLE QString comparisonSyncStrategyField(const QString& field) const;
    QAbstractItemModel* comparisonSyncStrategiesModel() const {
        return syncStrategyModel_.get();
    }
    LapListModel* primaryLapsModel() const { return primaryLapsModel_.get(); }
    LapListModel* compareLapsModel() const { return compareLapsModel_.get(); }
    FilmstripSessionListModel* filmstripSessionsModel() const {
        return filmstripSessionsModel_.get();
    }
    QAbstractItemModel* channelsModel() const { return channelsModel_.get(); }
    QAbstractItemModel* cornersModel() const { return cornersModel_.get(); }
    QAbstractItemModel* driverMappingsModel() const {
        return driverMappingsModel_.get();
    }
    QAbstractItemModel* libraryModel() const { return libraryModel_.get(); }
    bool videoMuted() const;
    void setVideoMuted(bool muted);
    /// `--mute`: silence playback for this process without touching
    /// `video.muted` in omatrack.yml. The first click on the speaker button
    /// ends the override and persists as usual.
    void overrideVideoMuted(bool muted);
    QStringList sessionDirectoriesList() const { return sessionDirectories(); }

signals:
    void readyChanged();
    void loadingChanged();
    void fileOpenChanged();
    void lapLoadingChanged();
    /// Video playback has just reached the end of the primary lap.
    void primaryLapPlaybackEnded();
    void selectionChanged();
    /// The hover-peek lap changed. Renderers repaint from the same snapshot
    /// as a selection change, without the selection side effects (video sync
    /// resets, plugin sessions, model refreshes) that selectionChanged owns.
    void peekChanged();
    void editingCornersChanged();
    void cursorFracChanged();
    void cursorReadoutChanged();
    void channelsCursorTick();
    void viewChanged();
    void sessionsChanged();
    void cornersChanged();
    void cornerFocusChanged();
    void highlightedCornerMarkerChanged();
    void cornerConsistencyChanged();
    void traceConfidenceChanged();
    void driverMappingsChanged();
    void locationsChanged();
    void channelConfigChanged();
    void referenceAlignmentChanged();
    void comparisonSyncStrategyChanged();
    void trackAtlasChanged();
    void videoTimeChanged();
    void videoMutedChanged();
    void videoDownloadChanged();
    void videoMetadataChanged(const QString& videoPath);
    void folderChannelSampleReady(const QVariantMap& metadata);
    void filePinsChanged();
    void sidebarMetadataChanged(const QString& path,
                                const QVariantMap& details);
    void recentFilesChanged();
    void eventChanged();
    void usbChanged();
    void overlayStyleChanged();
    void standaloneVideoRequested(const QUrl& source);
    void operationError(const QString& title, const QString& message);
    void overlaysChanged();
    void sidecarLibraryChanged();
    void pluginsChanged();

private:
    enum class FileOpenRole { Automatic, Primary, Compare };

    SessionHandle* findSession(const QString& key) const;
    /// Records that a cached file was opened, which is what keeps it from
    /// being the next one evicted when the cache runs over its limit.
    void markRecentlyUsed(const QString& path) const;
    /// What to hand the player for a discovered file. Usually the local file
    /// itself; for a video inside a connection's cache, a streaming URL,
    /// because those are never downloaded. The URL carries the credential, so
    /// it goes to the player and nowhere else — see RemoteCache's
    /// streamSource().
    QUrl videoSourceFor(const QString& path) const;
    /// `renew` throws away the signature already held for this file, which is
    /// what recovers from one the server has stopped accepting.
    QUrl videoSourceFor(const QString& path, bool renew) const;
    /// The connection whose cache holds `path`, or null when it is an
    /// ordinary local file.
    const omatrack::LibraryLocation* connectionHolding(
        const QString& path) const;
    void startNextVideoDownload();
    void setVideoDownloadStatus(const QString& status);
    void cancelSelectionLoads();
    void resetPrimarySessionOverlays();
    void setPrimary(SessionHandle* session, int lapId);
    void setCompare(SessionHandle* session, int lapId);
    void requestLapLoad(SessionHandle* session, int lapId, bool compare);
    void queueFileOpen(const QString& filePath, FileOpenRole role,
                       int lapId = -1);
    void restoreLastSelection();
    void pauseSidebarMetadataQueue();
    void resumeSidebarMetadataQueue();
    void setPrimaryLapLoading(bool loading);
    void setCompareLapLoading(bool loading);
    void startSessionScan();
    void finishSessionScan(std::shared_ptr<SessionScanResult> result);
    void setupLibraryWatch();
    void rebuildLibraryWatch();
    void startUsbScan(bool force = true);
    void finishUsbScan(std::shared_ptr<SessionScanResult> result);
    void refreshTrackMetadata(const QStringList& paths);
    void scheduleUsbCopyPlanRefresh();
    void refreshUsbCopyPlan(bool clearStatus = true);
    void updateUsbCopyProgress();
    int sidebarPinIndex(const QString& kind, const QString& path) const;
    void rememberRecentFile(const QString& filePath);
    QString driverDisplay(const SessionHandle* session) const;
    QVariantMap sidebarFileDetails(const QString& path) const;
    QString assignedTrackSlug(const SessionHandle* session) const;
    QString detectedAtlasSlug(const SessionHandle* session) const;
    QString resolvedTrackSlug(const SessionHandle* session) const;
    /// The one metadata precedence rule (see AGENTS.md "Recording metadata
    /// precedence"): `recording_metadata[path]` from omatrack.yml over the
    /// root-to-leaf TRACK.yml chain. Served from the discovery snapshot
    /// (`fileMetadata_`), read once and memoized for a path discovery has
    /// not seen yet; never re-read per row or per frame.
    QVariantMap effectiveMetadata(const QString& path) const;
    QString displayTrack(const SessionHandle* session) const;
    static QString trackAssignmentKey(const SessionHandle* session);
    void schedulePreferencesSave();
    void flushPreferences();
    int locationIndex(const QString& id) const;
    bool appendFolderLocation(const QString& dirPath,
                              bool requireExists = true);
    bool hostWindowNs(qint64* startNs, qint64* endNs, qint64* utcNs) const;
    /// File-relative window of the open primary video, or the reference
    /// video when the primary has no recording clock.
    bool videoClipWindowNs(qint64* startNs, qint64* endNs) const;

    void loadCornersForPrimary();
    void rebuildCornerMarkers();
    void requestCornerConsistency();
    void prefetchNeighbourLaps();
    void invalidateTraceConfidence();
    void requestTraceConfidence();
    int neighbourLapId(int offset) const;
    void invalidateComparisonAlignment();
    /// Rebuilds the one cached primary→reference map. Peek passes
    /// notify=false so a transient hover preview never resets committed
    /// alignment tuning and never emits the strategy signal that hard-seeks
    /// reference video.
    void rebuildComparisonAlignment(bool notify = true);
    void invalidateExtraChannelCache();
    double compareTimeForPrimaryFraction(double fraction) const;
    QString effectiveComparisonSyncStrategy() const;
    static ComparisonAlignmentStrategy comparisonAlignmentStrategy(
        const QString& strategy);
    double comparisonPrimaryFraction(double fraction) const;
    double primaryFractionForCompareFraction(double fraction) const;
    double primaryTimeAtFraction(double fraction) const;
    double compareVideoTimeAtFraction(double fraction) const;
    double nextCornerStartFraction() const;
    // ── typed model refresh ────────────────────────────────────────
    void refreshLapModels();
    void refreshFilmstripModel();
    void refreshChannelsModel();
    void refreshCornersModel();
    void refreshDriverMappingsModel();
    void refreshSyncStrategyModel();
    void refreshLibraryModel();
    QVector<LapRow> buildLapRows(SessionHandle* session,
                                 int selectedLapId) const;
    QVector<ChannelRow> buildChannelRows() const;
    QVector<CornerRow> buildCornerRows() const;
    QVector<DriverMappingRow> buildDriverMappingRows() const;
    QVector<SyncStrategyRow> buildSyncStrategyRows() const;
    /// Per-location scan outcome, keyed by location id: the status line shown
    /// in preferences and the number of telemetry files discovered.
    QHash<QString, QString> locationStatuses_;
    QHash<QString, int> locationFileCounts_;
    QVariantList fileSources_;
    QStringList discoveredFilePaths_;
    QSet<QString> sidebarMetadataQueued_;
    QSet<QString> sidebarMetadataLoaded_;
    QThreadPool sidebarMetadataPool_;
    QHash<QString, QString> folderDisplayNames_;
    QHash<QString, std::shared_ptr<const FolderChannelSample>>
        folderChannelSamples_;
    QSet<QString> folderChannelSampleRequests_;
    /// Root-to-leaf TRACK.yml merge per recording path, filled by discovery
    /// and the sidebar metadata pass, refreshed by the app's own TRACK.yml
    /// writes, memoized on demand for undiscovered paths.
    mutable QHash<QString, QVariantMap> fileMetadata_;
    /// TRACK.yml writer: saveFolderMetadata() serialises on the GUI thread and
    /// writes on a worker; the completion applies state and reports failure
    /// through operationError.
    AsyncJob<FolderMetadataWrite> folderMetadataJob_;
    AsyncJob<std::shared_ptr<SessionScanResult>> scanJob_;
    AsyncJob<QHash<QString, QVariantMap>> trackMetadataRefreshJob_;
    AsyncJob<std::shared_ptr<SessionScanResult>> usbScanJob_;
    AsyncJob<QStringList> libraryWatchJob_;
    AsyncJob<omatrack::UsbCopyPlan> usbCopyPlanJob_;
    AsyncJob<omatrack::UsbCopyResult> usbCopyJob_;
    omatrack::UsbCopyPlan usbCopyPlan_;
    std::shared_ptr<std::atomic<qint64>> usbCopyBytes_;
    SerialJobQueue<std::shared_ptr<FileOpenResult>> fileOpenQueue_;
    QString fileOpenPath_;
    QSet<QString> transientSessionPaths_;
    /// Offline downloads, one at a time: two concurrent transfers of tens of
    /// gigabytes finish no sooner and make the progress meaningless.
    QStringList videoDownloadQueue_;
    QString videoDownloadPath_;
    QString videoDownloadStatus_;
    QString videoDownloadName_;
    std::shared_ptr<std::atomic<qint64>> videoDownloadReceived_;
    std::shared_ptr<std::atomic<qint64>> videoDownloadTotal_;
    QTimer* videoDownloadTicker_ = nullptr;
    /// The signature in force for each streamed recording, and when it was
    /// made. Reused rather than remade so the URL a player is holding stays
    /// equal to itself.
    struct StreamUrl {
        QUrl url;
        qint64 signedAtMs = 0;
    };
    mutable QHash<QString, StreamUrl> streamUrls_;
    /// Which cached file each streaming URL was built for, so that one the
    /// server has stopped honouring can be signed again. Keyed without the
    /// query string, which is the part a signature lives in.
    mutable QHash<QString, QString> streamedPaths_;
    QStringList trackMetadataPaths_;
    QHash<QString, qint64> trackMetadataStamps_;
    std::vector<std::unique_ptr<SessionHandle>> sessions_;
    SessionHandle* primarySession_ = nullptr;
    AsyncJob<std::shared_ptr<SessionLapLoadResult>> primaryLapJob_;
    AsyncJob<std::shared_ptr<SessionLapLoadResult>> compareLapJob_;
    SerialJobQueue<std::shared_ptr<SessionLapLoadResult>> lapPrefetchQueue_;
    SerialJobQueue<std::shared_ptr<SidebarMetadataResult>>
        sidebarMetadataQueue_;
    AsyncJob<std::shared_ptr<FolderChannelSample>> folderChannelSampleJob_;
    AsyncJob<std::shared_ptr<SessionConfidenceLoadResult>> traceConfidenceJob_;
    AsyncJob<std::shared_ptr<CornerConsistencyLoadResult>>
        cornerConsistencyJob_;
    AsyncJob<QString> videoDownloadJob_;
    AsyncJob<qint64> clearCacheJob_;
    SessionHandle* compareSession_ = nullptr;
    int primaryLap_ = -1;
    int compareLap_ = -1;
    // Transient filmstrip hover preview, keyed by session so a rebuilt
    // registry cannot dangle it. The committed selection above is untouched
    // while these are set; primaryUnified()/compareUnified() prefer them.
    bool hasPeek_ = false;
    QString peekSessionKey_;
    int peekLap_ = -1;
    bool peekCompare_ = false;
    void applyPeek();
    void resetPeekState();

    // Lap selection keeps the cursor, so a fresh store starts at the lap
    // start instead of inheriting a mid-lap fraction on the first load.
    double cursorFrac_ = 0.0;
    double viewStart_ = 0.0;
    double viewEnd_ = 1.0;
    bool editingCorners_ = false;
    QVector<CornerZone> cornerEditBaseline_;
    bool ready_ = false;
    std::optional<bool> videoMutedOverride_;
    bool loading_ = false;
    bool rescanPending_ = false;
    bool primaryLapLoading_ = false;
    bool compareLapLoading_ = false;
    bool fileOpenLoading_ = false;
    QString effectiveComparisonSyncStrategy_ = QStringLiteral("lap-percentage");
    double referenceAlignment_ = 0.0;
    QSet<QString> closedTracks_;

    QSet<QString> neighbourPrefetch_;
    QVector<CornerZone> corners_;
    QVector<CornerMarker> markers_;
    CornerConsistencyState cornerConsistency_;
    QHash<QString, TraceConfidenceBand> traceConfidenceBands_;
    QSet<int> traceConfidenceLapIds_;
    std::vector<double> traceConsistency_;
    QString traceConfidenceKey_;
    int traceConfidenceLapCount_ = 0;
    bool traceConfidenceMode_ = false;
    bool traceConfidenceLoading_ = false;
    bool traceConfidenceReady_ = false;
    int focusedCorner_ = -1;
    QString highlightedCornerMarker_;
    double focusReturnStart_ = 0.0;
    double focusReturnEnd_ = 1.0;
    mutable QHash<QString, std::shared_ptr<std::vector<double>>>
        extraChannelCache_;
    mutable QSet<QString> extraChannelLoading_;
    mutable QHash<QString, AsyncJob<std::shared_ptr<std::vector<double>>>*>
        extraChannelJobs_;
    mutable QVector<double> deltaCache_;
    mutable DamperAlignment damperAlignment_;
    mutable bool damperAlignmentValid_ = false;
    mutable bool deltaCacheValid_ = false;
    QVector<double> comparisonAlignmentTime_;
    QVector<double> comparisonAlignmentFraction_;
    QString comparisonAlignmentBasis_;
    int comparisonGpsAnchors_ = 0;
    // Extracted collaborators — plain QObjects owned by the store.
    PreferencesStore* prefs_ = nullptr;
    TrackAtlasManager* atlas_ = nullptr;
    OverlayManager* overlays_ = nullptr;
    omatrack::PluginHost* plugins_ = nullptr;
    void updatePluginSession();
    // Typed list models backing the Q_PROPERTYs that replace QVariantList
    // builders. Owned by the store; refreshed on the matching store signal.
    std::unique_ptr<LapListModel> primaryLapsModel_;
    std::unique_ptr<LapListModel> compareLapsModel_;
    std::unique_ptr<FilmstripSessionListModel> filmstripSessionsModel_;
    std::unique_ptr<UsbCopyListModel> usbCopyModel_;
    std::unique_ptr<ChannelListModel> channelsModel_;
    std::unique_ptr<CornerListModel> cornersModel_;
    std::unique_ptr<DriverMappingModel> driverMappingsModel_;
    std::unique_ptr<SyncStrategyModel> syncStrategyModel_;
    std::unique_ptr<LibraryModel> libraryModel_;
    QFileSystemWatcher* libraryWatch_ = nullptr;
    QTimer* usbPollTimer_ = nullptr;
    QTimer* usbDebounce_ = nullptr;
    QTimer* usbPlanDebounce_ = nullptr;
    QTimer* usbCopyProgressTimer_ = nullptr;
    QStringList usbRoots_;
    QVariantList usbFileSources_;
    QString manualUsbSource_;
    bool usbPresent_ = false;
    bool usbCopyVisible_ = false;
    bool usbRescanOnly_ = false;
    QString usbLabel_;
    QString usbCopyStatus_;
    double usbCopyProgress_ = 0.0;
};
