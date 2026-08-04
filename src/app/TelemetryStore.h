// Qt object model bridging the racecraft core engine to QML.
//
// Mirrors racecraft's TelemetryStore architecture: lazy per-file SessionHandles,
// Track→Date→Session→Laps grouping, primary/compare lap selection, cursor +
// viewport state, corner zones, and channel display configuration.

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
#include <QVariantList>

#include <memory>
#include <vector>

namespace racecraft {
class TelemetrySource;
struct UnifiedLap;
struct Lap;
}

class SessionHandle;
class TelemetryStore;
class QNetworkAccessManager;
class QTimer;

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
    bool isOutlap = false;
};

// ── session handle: lazy parse + unified-lap cache per file ─────────

class SessionHandle {
public:
    explicit SessionHandle(const QString& path);
    ~SessionHandle();

    const QString& path() const { return path_; }
    QString stem() const;

    const racecraft::TelemetrySource* source();
    const QVector<LapEntry>& laps();
    std::shared_ptr<const racecraft::UnifiedLap> unifiedLap(int lapId);
    QString sessionKey() const;
    bool isVideo() const {
        return path_.endsWith(QStringLiteral(".mp4"),
                              Qt::CaseInsensitive);
    }

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
    QString driverMappingKey() const {
        return carNumber_ + QStringLiteral("|") +
               carClass_ + QStringLiteral("|") + driverId_;
    }

private:
    void ensureSource();
    void ensureLapSummary();
    void applyEventDriverId(int eventDriverId);
    void populateLaps(const std::vector<racecraft::Lap>& detected);
    QString path_;
    std::unique_ptr<racecraft::TelemetrySource> src_;
    QVector<LapEntry> laps_;
    QHash<int, std::shared_ptr<const racecraft::UnifiedLap>> unifiedCache_;
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
    bool loaded_ = false;
};

// ── store (root model exposed to QML) ───────────────────────────────

class TelemetryStore : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool ready READ ready NOTIFY readyChanged)
    Q_PROPERTY(bool comparing READ comparing NOTIFY selectionChanged)
    Q_PROPERTY(bool editingCorners READ editingCorners WRITE setEditingCorners NOTIFY editingCornersChanged)
    Q_PROPERTY(double cursorFrac READ cursorFrac WRITE setCursorFrac NOTIFY cursorFracChanged)
    Q_PROPERTY(bool hasGpsData READ hasGpsData NOTIFY selectionChanged)
    Q_PROPERTY(double viewStart READ viewStart WRITE setViewStart NOTIFY viewChanged)
    Q_PROPERTY(double viewEnd READ viewEnd WRITE setViewEnd NOTIFY viewChanged)
    Q_PROPERTY(int channelHeight READ channelHeight WRITE setChannelHeight NOTIFY channelHeightChanged)
    Q_PROPERTY(QString primaryLabel READ primaryLabel NOTIFY selectionChanged)
    Q_PROPERTY(QString primaryDetail READ primaryDetail NOTIFY selectionChanged)
    Q_PROPERTY(QString primaryDriverName READ primaryDriverName NOTIFY selectionChanged)
    Q_PROPERTY(QString primaryDriverMappingKey READ primaryDriverMappingKey NOTIFY selectionChanged)
    Q_PROPERTY(QString compareLabel READ compareLabel NOTIFY selectionChanged)
    Q_PROPERTY(QString roomName READ roomName NOTIFY selectionChanged)
    Q_PROPERTY(QString primarySessionKey READ primarySessionKey NOTIFY selectionChanged)
    Q_PROPERTY(int primaryLapIndex READ primaryLapIndex NOTIFY selectionChanged)
    Q_PROPERTY(QString compareSessionKey READ compareSessionKey NOTIFY selectionChanged)
    Q_PROPERTY(int compareLapIndex READ compareLapIndex NOTIFY selectionChanged)
    Q_PROPERTY(double referenceAlignment READ referenceAlignment WRITE setReferenceAlignment NOTIFY referenceAlignmentChanged)
    Q_PROPERTY(bool trackAtlasReady READ trackAtlasReady NOTIFY trackAtlasChanged)
    Q_PROPERTY(QString trackAtlasStatus READ trackAtlasStatus NOTIFY trackAtlasChanged)
    Q_PROPERTY(QUrl primaryVideoSource READ primaryVideoSource NOTIFY selectionChanged)
    Q_PROPERTY(double primaryVideoTime READ primaryVideoTime NOTIFY videoTimeChanged)
public:
    explicit TelemetryStore(QObject* parent = nullptr);
    ~TelemetryStore() override;


    Q_INVOKABLE void closeTrack(const QString& trackName);

    Q_INVOKABLE void scan();
    Q_INVOKABLE void addSessionDirectory(const QString& dirPath);
    Q_INVOKABLE void openFile(const QString& filePath);
    Q_INVOKABLE bool directoryExists(const QString& dirPath) const;
    Q_INVOKABLE QString configFilePath() const;
    Q_INVOKABLE void removeSessionDirectory(const QString& dirPath);
    Q_INVOKABLE QStringList sessionDirectories() const;
    Q_INVOKABLE void clearSessions();
    Q_INVOKABLE QVariantList trackGroups() const;  // nested: track → dates → sessions → laps
    Q_INVOKABLE void refreshTrackAtlas();

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
    Q_INVOKABLE QVariantMap alignmentData(int points = 360) const;
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
    Q_INVOKABLE void setDriverAlias(const QString& detected, const QString& display);
    Q_INVOKABLE QVariantList driverMappings() const;
    Q_INVOKABLE void setDriverMapping(const QString& key,
                                      const QString& display);
    Q_INVOKABLE QString driverDisplayName(
        const QString& sessionKey) const;

    // ── data access for the trace canvas ───────────────────────────
    const racecraft::UnifiedLap* primaryUnified() const;
    const racecraft::UnifiedLap* compareUnified() const;
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
    const std::vector<double>* extraChannelData(
        const QString& key, bool reference) const;
    bool trackAtlasReady() const { return !atlasTracks_.isEmpty(); }
    QString trackAtlasStatus() const { return trackAtlasStatus_; }

    bool ready() const { return ready_; }
    bool comparing() const { return compareSession_ != nullptr; }
    bool editingCorners() const { return editingCorners_; }
    double cursorFrac() const { return cursorFrac_; }
    void setCursorFrac(double v);
    double viewStart() const { return viewStart_; }
    void setViewStart(double v);
    double viewEnd() const { return viewEnd_; }
    void setViewEnd(double v);
    double viewSpan() const { return qBound(0.001, viewEnd_ - viewStart_, 1.0); }
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
    QStringList sessionDirectoriesList() const { return sessionDirs_; }

signals:
    void readyChanged();
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

private:
    SessionHandle* findSession(const QString& key) const;
    void setPrimary(SessionHandle* session, int lapId);
    void setCompare(SessionHandle* session, int lapId);
    void scanDirectory(const QString& dir);
    void loadPreferences();
    QString driverDisplay(const SessionHandle* session) const;
    void savePreferences();
    void loadChannelsConfig();

    void loadCornersForPrimary();
    QVector<CornerZone> atlasCornersForPrimary() const;
    bool parseTrackAtlas(const QByteArray& payload);
    void loadTrackAtlasCache();
    void updateTrackAtlas(bool force);
    QString trackAtlasCachePath() const;
    QStringList sessionDirs_;
    QSet<QString> scannedSessionPaths_;
    QSet<QString> scannedSessionIdentities_;
    QHash<QString, QString> driverMappings_;
    QString lastPrimaryKey_;
    QString lastCompareKey_;
    int lastPrimaryLap_ = -1;
    int lastCompareLap_ = -1;
    std::vector<std::unique_ptr<SessionHandle>> sessions_;
    SessionHandle* primarySession_ = nullptr;
    SessionHandle* compareSession_ = nullptr;
    int primaryLap_ = -1;
    int compareLap_ = -1;

    double cursorFrac_ = 0.5;
    double viewStart_ = 0.0;
    double viewEnd_ = 1.0;
    int channelHeight_ = 110;
    bool editingCorners_ = false;
    bool ready_ = false;
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
    QStringList channelOrder_;
    mutable QVector<double> deltaCache_;
    mutable bool deltaCacheValid_ = false;

    friend class TraceView;
};
