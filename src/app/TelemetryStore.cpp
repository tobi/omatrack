#include "TelemetryStore.h"

#include "core/TelemetryEngine.h"

#include <QDebug>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScopeGuard>
#include <QSettings>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
constexpr qint64 kTrackAtlasMaxAgeSeconds = 24 * 60 * 60;
constexpr int kTrackAtlasCheckIntervalMs = 6 * 60 * 60 * 1000;
const QUrl kTrackAtlasUrl(
    QStringLiteral("https://raw.githubusercontent.com/tobi/track-atlas/main/tracks.jsonl"));

QString normalizeAtlasName(QString value) {
    value = value.toLower();
    value.remove(QRegularExpression(QStringLiteral("[^a-z0-9]")));
    return value;
}

bool ignoredImportedCorner(const QString& name) {
    QString normalized = name.trimmed().toLower();
    normalized.remove(QRegularExpression(QStringLiteral("[^a-z0-9]")));
    return normalized == QStringLiteral("5") ||
           normalized == QStringLiteral("6") ||
           normalized == QStringLiteral("turn5") ||
           normalized == QStringLiteral("turn6");
}
QString driverNameForEventId(int id, const QString& fallback) {
    static const QHash<int, QString> names = {
        {1, QStringLiteral("Tobi Lütke")},
        {2, QStringLiteral("Mathias Beche")},
        {3, QStringLiteral("DHH")},
        {4, QStringLiteral("Charles Melesi")},
    };
    return names.value(id, fallback);
}


QColor defaultChannelColor(const QString& key) {
    static const QHash<QString, QColor> colors = {
        {"speed", QColor("#a7c080")},   {"throttle", QColor("#a7c080")},
        {"brake", QColor("#e67e80")},   {"steering", QColor("#dbbc7f")},
        {"gear", QColor("#d699b6")},    {"dampers", QColor("#7fbbb3")},
        {"g_long", QColor("#e09d7f")},  {"delta", QColor("#83c092")},
        {"clutch", QColor("#d3c6aa")},  {"driver_throttle", QColor("#9da9a0")},
        {"gps_lat", QColor("#83c092")}, {"gps_lon", QColor("#e09d7f")},
    };
    return colors.value(key, QColor("#9da9a0"));
}

QPair<QString, QString> channelMetadata(const QString& key) {
    static const QHash<QString, QPair<QString, QString>> metadata = {
        {"speed", {"Speed", "km/h"}}, {"throttle", {"Throttle", "%"}},
        {"brake", {"Brake", "bar"}}, {"steering", {"Steering", "deg"}},
        {"gear", {"Gear", ""}}, {"dampers", {"Dampers", "mm"}},
        {"g_long", {"G Long", "g"}}, {"delta", {"Δ Time", "s"}},
        {"clutch", {"Clutch", "%"}},
        {"driver_throttle", {"Driver throttle", "%"}},
        {"gps_lat", {"GPS latitude", "°"}},
        {"gps_lon", {"GPS longitude", "°"}},
    };
    return metadata.value(key, {key, {}});
}

}  // namespace

using namespace racecraft;

// ── SessionHandle ───────────────────────────────────────────────────

SessionHandle::SessionHandle(const QString& path) : path_(path) {
    // Resolve track + metadata eagerly from filename only (cheap, no parse).
    QFileInfo fi(path);
    SessionMeta meta = sessionMetaFromFilename(fi.completeBaseName().toStdString());
    venue_ = QString::fromStdString(meta.venue);
    driver_ = QString::fromStdString(meta.driverName);
    vehicle_ = QString::fromStdString(meta.vehicleId);
    time_ = QString::fromStdString(meta.time);
    driverId_ = QString::fromStdString(meta.driverTag);
    driver_ = QStringLiteral("Driver id %1").arg(driverId_);
    const QString stem = fi.completeBaseName();
    QRegularExpression carPattern(
        QStringLiteral("(?:^|[_ ])Car(\\d+)"),
        QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatch carMatch = carPattern.match(stem);
    if (carMatch.hasMatch()) {
        carNumber_ = carMatch.captured(1);
    } else {
        QRegularExpression hashPattern(QStringLiteral("#(\\d+)"));
        const QRegularExpressionMatch hashMatch = hashPattern.match(stem);
        if (hashMatch.hasMatch()) carNumber_ = hashMatch.captured(1);
    }
    QRegularExpression classPattern(
        QStringLiteral("(LMP\\d+|GT[0-9A-Z]+)"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch classMatch = classPattern.match(stem);
    carClass_ = classMatch.hasMatch()
                    ? classMatch.captured(1).toUpper()
                    : vehicle_;

    // Track: prefer bundled venue knowledge; fall back to a folder name token.
    track_ = venue_;
    if (track_.isEmpty()) {
        track_ = fi.dir().dirName();
    }
    date_ = QString::fromStdString(meta.date);
    if (date_.isEmpty()) {
        date_ = "Unknown";
    }
}

SessionHandle::~SessionHandle() = default;

QString SessionHandle::stem() const {
    QFileInfo fi(path_);
    return fi.completeBaseName();
}

QString SessionHandle::sessionKey() const {
    return path_;
}

void SessionHandle::populateLaps(const std::vector<Lap>& detected) {
    laps_.clear();
    double fastest = 1e18;
    int fastestId = -1;
    for (const Lap& lap : detected) {
        LapEntry entry;
        entry.lapId = lap.id;
        entry.startTime = lap.startTime;
        entry.endTime = lap.endTime;
        entry.timeMs = lap.timeMs;
        entry.isOutlap = lap.id == 0;
        entry.label = entry.isOutlap
                          ? QStringLiteral("Out")
                          : QStringLiteral("L%1").arg(lap.id);
        entry.timeText =
            QString::fromStdString(formatLapTime(lap.timeMs));
        if (!entry.isOutlap && lap.timeMs < fastest) {
            fastest = lap.timeMs;
            fastestId = lap.id;
        }
        laps_.append(entry);
    }
    for (LapEntry& entry : laps_)
        entry.isFastest =
            entry.lapId == fastestId && fastestId >= 0;
    summaryLoaded_ = true;
}

void SessionHandle::ensureLapSummary() {
    if (summaryLoaded_) return;
    int eventDriverId = 0;
    populateLaps(
        detectLapsLightweight(path_.toStdString(), &eventDriverId));
    if (eventDriverId > 0) {
        driverId_ = QString::number(eventDriverId);
        driver_ = QStringLiteral("Driver id %1").arg(driverId_);
    } else if (eventDriverId < 0) {
        driverId_ = QStringLiteral("?");
        driver_ = QStringLiteral("Unknown driver");
    }
}

void SessionHandle::ensureSource() {
    if (src_) return;
    src_ = TelemetrySource::open(path_.toStdString());
    if (!src_) return;
    if (!summaryLoaded_) populateLaps(src_->detectLaps());
    loaded_ = true;
}

double SessionHandle::bestLapMs() {
    ensureLapSummary();
    for (const LapEntry& entry : laps_)
        if (entry.isFastest) return entry.timeMs;
    return 0.0;
}

QString SessionHandle::bestLapTime() {
    const double milliseconds = bestLapMs();
    return milliseconds > 0.0
               ? QString::fromStdString(formatLapTime(milliseconds))
               : QStringLiteral("—");
}

const racecraft::TelemetrySource* SessionHandle::source() {
    ensureSource();
    return src_.get();
}

const QVector<LapEntry>& SessionHandle::laps() {
    ensureSource();
    return laps_;
}

std::shared_ptr<const racecraft::UnifiedLap> SessionHandle::unifiedLap(int lapId) {
    ensureSource();
    if (!src_) return nullptr;
    auto it = unifiedCache_.find(lapId);
    if (it != unifiedCache_.end()) return it.value();
    for (const auto& l : laps_) {
        if (l.lapId == lapId) {
            auto u = std::make_shared<UnifiedLap>(src_->unifyLap(l.startTime, l.endTime));
            if (u->size() == 0) return nullptr;
            unifiedCache_.insert(lapId, u);
            return u;
        }
    }
    return nullptr;
}

// ── TelemetryStore ──────────────────────────────────────────────────

TelemetryStore::TelemetryStore(QObject* parent) : QObject(parent) {
    loadPreferences();
    loadChannelsConfig();

    atlasNetwork_ = new QNetworkAccessManager(this);
    atlasTimer_ = new QTimer(this);
    atlasTimer_->setInterval(kTrackAtlasCheckIntervalMs);
    connect(atlasTimer_, &QTimer::timeout, this,
            [this]() { updateTrackAtlas(false); });
    atlasTimer_->start();
    loadTrackAtlasCache();
    QTimer::singleShot(0, this, [this]() { updateTrackAtlas(false); });

    if (!sessionDirs_.isEmpty()) scan();
}

TelemetryStore::~TelemetryStore() = default;

void TelemetryStore::loadPreferences() {
    QSettings s;
    const QStringList dirs = s.value("sessionDirs").toStringList();
    for (const QString& d : dirs) {
        if (!sessionDirs_.contains(d)) sessionDirs_.append(d);
    }
    channelHeight_ = s.value("channelHeight", 110).toInt();
    lastPrimaryKey_ = s.value("lastPrimaryKey").toString();
    lastPrimaryLap_ = s.value("lastPrimaryLap", -1).toInt();
    lastCompareKey_ = s.value("lastCompareKey").toString();
    lastCompareLap_ = s.value("lastCompareLap", -1).toInt();
    s.beginGroup("driverAliases");
    for (const QString& key : s.childKeys())
        driverAliases_.insert(key, s.value(key).toString());
    s.endGroup();
    s.beginGroup("driverMappings");
    for (const QString& key : s.childKeys())
        driverMappings_.insert(key, s.value(key).toString());
    s.endGroup();
}

void TelemetryStore::savePreferences() {
    QSettings s;
    s.setValue("sessionDirs", sessionDirs_);
    s.setValue("channelHeight", channelHeight_);
    s.setValue("lastPrimaryKey", lastPrimaryKey_);
    s.setValue("lastPrimaryLap", lastPrimaryLap_);
    s.setValue("lastCompareKey", lastCompareKey_);
    s.setValue("lastCompareLap", lastCompareLap_);

    s.beginGroup("driverAliases");
    s.remove(QString());
    for (auto it = driverAliases_.cbegin(); it != driverAliases_.cend(); ++it)
        s.setValue(it.key(), it.value());
    s.endGroup();

    s.beginGroup("driverMappings");
    s.remove(QString());
    for (auto it = driverMappings_.cbegin();
         it != driverMappings_.cend(); ++it)
        s.setValue(it.key(), it.value());
    s.endGroup();
    s.beginGroup("channels");
    s.setValue("_paletteSchema", 3);
    for (const QString& key : channelOrder_) {
        s.setValue(key + "/visible", channelVisible_.value(key, key != "delta"));
        s.setValue(key + "/color", channelColors_.value(key).name(QColor::HexRgb));
        s.setValue(key + "/weight", channelWeights_.value(key, 1.0));
    }
    s.endGroup();
}

void TelemetryStore::loadChannelsConfig() {
    // The dialog exposes every UnifiedLap channel; extras start hidden so
    // enabling them never changes the default overview.
    static const char* order[] = {
        "speed", "throttle", "brake", "steering", "gear",
        "dampers", "g_long", "clutch", "driver_throttle",
        "gps_lat", "gps_lon", "delta"};
    channelOrder_ =
        QStringList{order, order + sizeof(order) / sizeof(order[0])};
    QSettings s;
    s.beginGroup("channels");
    const int paletteSchema = s.value("_paletteSchema", 1).toInt();
    for (const QString& k : channelOrder_) {
        const bool defaultVisible =
            k != "delta" && k != "clutch" &&
            k != "driver_throttle" && k != "gps_lat" &&
            k != "gps_lon";
        channelVisible_[k] =
            s.value(k + "/visible", defaultVisible).toBool();
        QColor color = paletteSchema < 3
                           ? defaultChannelColor(k)
                           : QColor(s.value(k + "/color", defaultChannelColor(k).name()).toString());
        channelColors_[k] = color.isValid() ? color : defaultChannelColor(k);
        channelWeights_[k] = qBound(0.5, s.value(k + "/weight", 1.0).toDouble(), 2.0);
    }
    if (paletteSchema < 3) s.setValue("_paletteSchema", 3);
    s.endGroup();
}

QString TelemetryStore::trackAtlasCachePath() const {
    const QString directory =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
        QStringLiteral("/track-atlas");
    QDir().mkpath(directory);
    return directory + QStringLiteral("/tracks.jsonl");
}

bool TelemetryStore::parseTrackAtlas(const QByteArray& payload) {
    QHash<QString, QJsonObject> parsed;
    const QList<QByteArray> lines = payload.split('\n');
    for (const QByteArray& rawLine : lines) {
        const QByteArray line = rawLine.trimmed();
        if (line.isEmpty()) continue;
        QJsonParseError error;
        const QJsonDocument document = QJsonDocument::fromJson(line, &error);
        if (error.error != QJsonParseError::NoError || !document.isObject())
            continue;
        const QJsonObject track = document.object();
        const QString slug = track.value(QStringLiteral("slug")).toString();
        if (!slug.isEmpty()) parsed.insert(slug, track);
    }
    if (parsed.isEmpty()) return false;

    atlasTracks_ = std::move(parsed);
    trackAtlasStatus_ =
        QStringLiteral("%1 tracks cached").arg(atlasTracks_.size());
    emit trackAtlasChanged();
    if (primarySession_) {
        loadCornersForPrimary();
        emit cornersChanged();
    }
    return true;
}

void TelemetryStore::loadTrackAtlasCache() {
    QFile cache(trackAtlasCachePath());
    if (!cache.open(QIODevice::ReadOnly)) {
        trackAtlasStatus_ = QStringLiteral("No track-atlas cache");
        return;
    }
    if (!parseTrackAtlas(cache.readAll()))
        trackAtlasStatus_ = QStringLiteral("Invalid track-atlas cache");
}

void TelemetryStore::refreshTrackAtlas() {
    updateTrackAtlas(true);
}

void TelemetryStore::updateTrackAtlas(bool force) {
    const QFileInfo cache(trackAtlasCachePath());

    trackAtlasStatus_ = QStringLiteral("Updating track-atlas…");
    emit trackAtlasChanged();
    QNetworkRequest request(kTrackAtlasUrl);
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("racecraft-qt/0.1"));
    QNetworkReply* reply = atlasNetwork_->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        const auto cleanup = qScopeGuard([reply]() { reply->deleteLater(); });
        if (reply->error() != QNetworkReply::NoError) {
            trackAtlasStatus_ = atlasTracks_.isEmpty()
                                    ? QStringLiteral("Track-atlas unavailable")
                                    : QStringLiteral("Track-atlas cache in use");
            emit trackAtlasChanged();
            return;
        }
        const QByteArray payload = reply->readAll();
        if (!parseTrackAtlas(payload)) {
            trackAtlasStatus_ = QStringLiteral("Track-atlas update invalid");
            emit trackAtlasChanged();
            return;
        }
        QSaveFile output(trackAtlasCachePath());
        if (output.open(QIODevice::WriteOnly)) {
            output.write(payload);
            output.commit();
        }
    });
}


// ── scanning / grouping ─────────────────────────────────────────────

void TelemetryStore::scan() {
    closedTracks_.clear();
    scannedSessionPaths_.clear();
    scannedSessionIdentities_.clear();
    clearSessions();
    for (const QString& dir : sessionDirs_) scanDirectory(dir);
    if (!lastPrimaryKey_.isEmpty()) {
        SessionHandle* saved = findSession(lastPrimaryKey_);
        if (saved) {
            for (const LapEntry& lap : saved->laps()) {
                if (lap.lapId == lastPrimaryLap_) {
                    setPrimary(saved, lastPrimaryLap_);
                    break;
                }
            }
        }
    }
    if (!lastCompareKey_.isEmpty() && primarySession_) {
        SessionHandle* saved = findSession(lastCompareKey_);
        if (saved) {
            for (const LapEntry& lap : saved->laps()) {
                if (lap.lapId == lastCompareLap_) {
                    setCompare(saved, lastCompareLap_);
                    break;
                }
            }
        }
    }
    ready_ = true;
    emit readyChanged();
    emit sessionsChanged();
}

void TelemetryStore::scanDirectory(const QString& dir) {
    const QStringList filters{"*.pds", "*.PDS", "*.ld", "*.LD", "*.ldx", "*.LDX",
                              "*.vbo", "*.VBO"};
    QDirIterator it(dir, filters, QDir::Files, QDirIterator::Subdirectories);
    QStringList paths;
    while (it.hasNext()) {
        it.next();
        QString path = it.filePath();
        const QFileInfo info(path);
        if (info.suffix().compare("ldx", Qt::CaseInsensitive) == 0) {
            QString companion = path;
            companion.chop(3);
            companion += "ld";
            if (!QFileInfo::exists(companion)) continue;
            path = companion;
        }
        const QFileInfo resolved(path);
        const QString canonical = resolved.canonicalFilePath().isEmpty()
                                       ? resolved.absoluteFilePath()
                                       : resolved.canonicalFilePath();
        QString identity = resolved.completeBaseName().toLower();
        identity.remove(QRegularExpression(QStringLiteral("-\\d+$")));
        if (scannedSessionIdentities_.contains(identity)) continue;
        scannedSessionIdentities_.insert(identity);
        if (scannedSessionPaths_.contains(canonical)) continue;
        scannedSessionPaths_.insert(canonical);
        if (!paths.contains(canonical)) paths.append(canonical);
    }
    std::sort(paths.begin(), paths.end());
    for (const QString& path : paths) {
        auto h = std::make_unique<SessionHandle>(path);
        sessions_.push_back(std::move(h));
    }
}

void TelemetryStore::addSessionDirectory(const QString& dirPath) {
    if (dirPath.isEmpty() || !QFileInfo::exists(dirPath)) return;
    if (!sessionDirs_.contains(dirPath)) sessionDirs_.append(dirPath);
    savePreferences();
    scan();
}
void TelemetryStore::removeSessionDirectory(const QString& dirPath) {
    if (!sessionDirs_.removeAll(dirPath)) return;
    savePreferences();
    scan();
}

void TelemetryStore::openFile(const QString& filePath) {
    if (filePath.isEmpty() || !QFileInfo::exists(filePath)) return;
    QString telemetryPath = filePath;
    if (QFileInfo(filePath).suffix().compare("ldx", Qt::CaseInsensitive) == 0) {
        telemetryPath.chop(3);
        telemetryPath += "ld";
    }
    if (!QFileInfo::exists(telemetryPath)) return;
    auto h = std::make_unique<SessionHandle>(telemetryPath);
    SessionHandle* raw = h.get();
    sessions_.push_back(std::move(h));
    // select a fastest lap
    raw->laps();
    const auto& laps = raw->laps();
    int bestId = -1;
    double bestMs = 1e18;
    for (const auto& l : laps) {
        if (!l.isOutlap && l.timeMs < bestMs) {
            bestMs = l.timeMs;
            bestId = l.lapId;
        }
    }
    if (bestId >= 0) {
        setPrimary(raw, bestId);
        viewStart_ = 0.0;
        viewEnd_ = 1.0;
    }
    ready_ = true;
    emit readyChanged();
    emit sessionsChanged();
}

void TelemetryStore::clearSessions() {
    sessions_.clear();
    primarySession_ = nullptr;
    compareSession_ = nullptr;
    primaryLap_ = -1;
    compareLap_ = -1;
    deltaCacheValid_ = false;
    setReferenceAlignment(0.0);
    corners_.clear();
    emit selectionChanged();
    emit cornersChanged();
}

QStringList TelemetryStore::sessionDirectories() const {
    return sessionDirs_;
}

SessionHandle* TelemetryStore::findSession(const QString& key) const {
    for (auto& s : sessions_)
        if (s->sessionKey() == key) return s.get();
    return nullptr;
}
void TelemetryStore::closeTrack(const QString& trackName) {
    if (trackName.isEmpty()) return;
    if (closedTracks_.contains(trackName)) return;
    closedTracks_.insert(trackName);
    emit sessionsChanged();
}

QString TelemetryStore::driverDisplay(const SessionHandle* session) const {
    if (!session) return QStringLiteral("Unknown driver");
    const QString mapped = driverMappings_.value(
        session->driverMappingKey()).trimmed();
    return mapped.isEmpty() ? session->driver() : mapped;
}

QVariantList TelemetryStore::trackGroups() const {
    QVariantList tracks;
    QStringList trackNames;
    QMap<QString, QHash<QString, QStringList>> dateSessions;

    for (const auto& session : sessions_) {
        const QString track =
            session->track().isEmpty() ? QStringLiteral("Unknown")
                                       : session->track();
        if (closedTracks_.contains(track)) continue;
        const QString date =
            session->date().isEmpty() ? QStringLiteral("Unknown")
                                      : session->date();
        if (!dateSessions.contains(track)) trackNames.append(track);
        dateSessions[track][date].append(
            session->stem() + "\n" + session->sessionKey());
    }

    std::sort(trackNames.begin(), trackNames.end());
    for (const QString& trackName : trackNames) {
        QVariantList dates;
        QStringList dateNames = dateSessions[trackName].keys();
        std::sort(dateNames.begin(), dateNames.end(),
                  [](const QString& a, const QString& b) {
                      const QDate da = QDate::fromString(a, "dd/MM/yyyy");
                      const QDate db = QDate::fromString(b, "dd/MM/yyyy");
                      if (da.isValid() && db.isValid() && da != db)
                          return da > db;
                      return a > b;
                  });
        for (const QString& dateName : dateNames) {
            QVector<QVariantMap> sessionMaps;
            for (const QString& pair : dateSessions[trackName][dateName]) {
                const int separator = pair.indexOf('\n');
                const QString stem = pair.left(separator);
                const QString key = pair.mid(separator + 1);
                SessionHandle* session = findSession(key);
                if (!session) continue;
                const double bestTimeMs = session->bestLapMs();
                sessionMaps.append(QVariantMap{
                    {"stem", stem},
                    {"key", key},
                    {"mappingKey", session->driverMappingKey()},
                    {"driver", driverDisplay(session)},
                    {"driverId", session->driverId()},
                    {"carNumber", session->carNumber()},
                    {"carClass", session->carClass()},
                    {"vehicle", session->vehicle()},
                    {"sessionTime", session->sessionTime()},
                    {"bestTime", session->bestLapTime()},
                    {"bestTimeMs", bestTimeMs}});
            }
            std::sort(sessionMaps.begin(), sessionMaps.end(),
                      [](const QVariantMap& a, const QVariantMap& b) {
                          const QTime at =
                              QTime::fromString(a.value("sessionTime").toString(),
                                                "HH:mm:ss");
                          const QTime bt =
                              QTime::fromString(b.value("sessionTime").toString(),
                                                "HH:mm:ss");
                          if (at.isValid() && bt.isValid() && at != bt)
                              return at < bt;
                          if (at.isValid() != bt.isValid()) return at.isValid();
                          return a.value("stem").toString() <
                                 b.value("stem").toString();
                      });
            QHash<QString, double> driverBest;
            double dayBest = std::numeric_limits<double>::max();
            for (const QVariantMap& row : sessionMaps) {
                const double best = row.value("bestTimeMs").toDouble();
                if (best <= 0.0) continue;
                driverBest[row.value("mappingKey").toString()] =
                    std::min(driverBest.value(
                                 row.value("mappingKey").toString(),
                                 std::numeric_limits<double>::max()),
                             best);
                dayBest = std::min(dayBest, best);
            }
            QVariantList sessions;
            for (QVariantMap row : sessionMaps) {
                const QString mappingKey = row.value("mappingKey").toString();
                const double best = row.value("bestTimeMs").toDouble();
                row.insert("isDriverBest",
                           best > 0.0 &&
                               qFuzzyCompare(best + 1.0,
                                             driverBest.value(mappingKey) + 1.0));
                row.insert("isDayBest",
                           best > 0.0 &&
                               qFuzzyCompare(best + 1.0, dayBest + 1.0));
                sessions.append(row);
            }
            dates.append(QVariantMap{{"date", dateName},
                                     {"sessions", sessions}});
        }
        tracks.append(QVariantMap{{"track", trackName},
                                  {"dates", dates}});
    }
    return tracks;
}

QVector<CornerZone> TelemetryStore::atlasCornersForPrimary() const {
    QVector<CornerZone> result;
    if (!primarySession_ || primaryLap_ < 0 || atlasTracks_.isEmpty())
        return result;

    const QString wanted = normalizeAtlasName(primarySession_->track());
    if (wanted.isEmpty()) return result;

    QJsonObject matchedTrack;
    int bestTrackScore = std::numeric_limits<int>::max();
    for (auto it = atlasTracks_.cbegin(); it != atlasTracks_.cend(); ++it) {
        const QJsonObject track = it.value();
        QStringList names{
            track.value(QStringLiteral("slug")).toString(),
            track.value(QStringLiteral("name")).toString(),
        };
        for (const QJsonValue& alias :
             track.value(QStringLiteral("aka")).toArray())
            names.append(alias.toString());
        const QJsonObject external =
            track.value(QStringLiteral("external_ids")).toObject();
        for (auto externalIt = external.begin();
             externalIt != external.end(); ++externalIt)
            names.append(externalIt.value().toString());

        for (const QString& name : names) {
            const QString candidate = normalizeAtlasName(name);
            if (candidate.isEmpty()) continue;
            int score = std::numeric_limits<int>::max();
            if (candidate == wanted) {
                score = 0;
            } else if (candidate.contains(wanted) ||
                       wanted.contains(candidate)) {
                score = 10 + std::abs(candidate.size() - wanted.size());
            }
            if (score < bestTrackScore) {
                bestTrackScore = score;
                matchedTrack = track;
            }
        }
    }
    if (matchedTrack.isEmpty() ||
        bestTrackScore == std::numeric_limits<int>::max())
        return result;

    const auto unified = primarySession_->unifiedLap(primaryLap_);
    const double lapLength =
        unified && !unified->distance.empty() ? unified->distance.back() : 0.0;
    const QString sessionStem = primarySession_->stem().toLower();

    QJsonObject matchedLayout;
    double bestLayoutScore = std::numeric_limits<double>::max();
    for (const QJsonValue& layoutValue :
         matchedTrack.value(QStringLiteral("layouts")).toArray()) {
        const QJsonObject layout = layoutValue.toObject();
        const double declaredLength =
            layout.value(QStringLiteral("length_m")).toDouble(0.0);
        double score =
            declaredLength > 0.0 && lapLength > 0.0
                ? std::fabs(declaredLength - lapLength)
                : 100000.0;
        for (const QJsonValue& seriesValue :
             layout.value(QStringLiteral("series")).toArray()) {
            const QString series = seriesValue.toString().toLower();
            if (!series.isEmpty() && sessionStem.contains(series))
                score -= 10000.0;
        }
        if (score < bestLayoutScore) {
            bestLayoutScore = score;
            matchedLayout = layout;
        }
    }
    if (matchedLayout.isEmpty()) return result;

    QHash<QString, QJsonObject> cornerPoints;
    for (const QJsonValue& layerValue :
         matchedLayout.value(QStringLiteral("point_layers")).toArray()) {
        const QJsonObject layer = layerValue.toObject();
        if (layer.value(QStringLiteral("id")).toString() !=
            QStringLiteral("corners"))
            continue;
        for (const QJsonValue& itemValue :
             layer.value(QStringLiteral("items")).toArray()) {
            const QJsonObject item = itemValue.toObject();
            cornerPoints.insert(item.value(QStringLiteral("id")).toString(),
                                item);
        }
    }

    QJsonArray ranges;
    for (const QJsonValue& layerValue :
         matchedLayout.value(QStringLiteral("range_layers")).toArray()) {
        const QJsonObject layer = layerValue.toObject();
        if (layer.value(QStringLiteral("id")).toString() ==
            QStringLiteral("corner_ranges")) {
            ranges = layer.value(QStringLiteral("items")).toArray();
            break;
        }
    }
    if (ranges.isEmpty()) return result;

    const QString labelDefault =
        matchedLayout.value(QStringLiteral("label_default"))
            .toString(QStringLiteral("numbered"));
    auto fractionAtDistance = [&](double normalizedDistance) {
        if (!unified || unified->distance.size() < 2 || lapLength <= 0.0)
            return qBound(0.0, normalizedDistance, 1.0);
        const double target =
            qBound(0.0, normalizedDistance, 1.0) * lapLength;
        const auto it = std::lower_bound(
            unified->distance.begin(), unified->distance.end(), target);
        const int hi = std::clamp(
            int(it - unified->distance.begin()), 0,
            int(unified->distance.size()) - 1);
        if (hi == 0) return 0.0;
        const int lo = hi - 1;
        const double span = unified->distance[hi] - unified->distance[lo];
        const double local =
            span > 0.0 ? (target - unified->distance[lo]) / span : 0.0;
        return (lo + local) / double(unified->distance.size() - 1);
    };

    for (const QJsonValue& rangeValue : ranges) {
        const QJsonObject range = rangeValue.toObject();
        const QString anchor =
            range.value(QStringLiteral("anchor")).toString();
        const QJsonObject point = cornerPoints.value(anchor);
        const QJsonObject labels =
            point.value(QStringLiteral("labels")).toObject();
        QString label = labels.value(labelDefault).toString();
        if (label.isEmpty())
            label = range.value(QStringLiteral("label")).toString();
        if (label.isEmpty())
            label = labels.value(QStringLiteral("numbered")).toString();
        if (label.isEmpty())
            label = point.value(QStringLiteral("label")).toString(anchor);

        CornerZone corner;
        corner.name = label;
        corner.start = fractionAtDistance(
            range.value(QStringLiteral("start")).toDouble());
        corner.end = fractionAtDistance(
            range.value(QStringLiteral("end")).toDouble());
        if (!corner.name.isEmpty() &&
            !ignoredImportedCorner(corner.name) &&
            corner.end > corner.start)
            result.append(corner);
    }
    std::sort(result.begin(), result.end(),
              [](const CornerZone& a, const CornerZone& b) {
                  return a.start < b.start;
              });
    return result;
}

void TelemetryStore::loadCornersForPrimary() {
    corners_.clear();
    if (!primarySession_) return;

    corners_ = atlasCornersForPrimary();
    QString trackKey = primarySession_->track().toLower();
    trackKey.remove(QRegularExpression(QStringLiteral("[^a-z0-9]")));
    if (corners_.isEmpty()) {
        for (const QString& bundled :
             {QStringLiteral("lilski_sebring"),
              QStringLiteral("ier_daytona"),
              QStringLiteral("daytona")}) {
            if (!trackKey.contains(bundled) &&
                !bundled.contains(trackKey))
                continue;
            QFile file(QStringLiteral(":/corners/") + bundled +
                       QStringLiteral(".csv"));
            if (file.open(QIODevice::ReadOnly)) {
                const auto lines =
                    QString::fromUtf8(file.readAll())
                        .split('\n', Qt::SkipEmptyParts);
                for (int i = 1; i < lines.size(); ++i) {
                    const auto parts = lines[i].split(',');
                    if (parts.size() < 3) continue;
                    CornerZone corner;
                    corner.name = parts[0].trimmed();
                    if (ignoredImportedCorner(corner.name)) continue;
                    corner.start = parts[1].trimmed().toDouble();
                    corner.end = parts[2].trimmed().toDouble();
                    corners_.append(corner);
                }
            }
            break;
        }
    }

    QString safeName = primarySession_->track().toLower();
    safeName.replace(' ', '_').replace('-', '_');
    QFile custom(
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
        QStringLiteral("/") + safeName + QStringLiteral(".csv"));
    if (custom.open(QIODevice::ReadOnly)) {
        QVector<CornerZone> loaded;
        const auto lines =
            QString::fromUtf8(custom.readAll())
                .split('\n', Qt::SkipEmptyParts);
        for (int i = 1; i < lines.size(); ++i) {
            const auto parts = lines[i].split(',');
            if (parts.size() < 3) continue;
            CornerZone corner;
            corner.name = parts[0].trimmed();
            if (ignoredImportedCorner(corner.name)) continue;
            corner.start =
                qBound(0.0, parts[1].trimmed().toDouble(), 1.0);
            corner.end =
                qBound(corner.start, parts[2].trimmed().toDouble(), 1.0);
            loaded.append(corner);
        }
        if (!loaded.isEmpty()) corners_ = loaded;
    }
}

// ── selection ───────────────────────────────────────────────────────

void TelemetryStore::setPrimary(SessionHandle* session, int lapId) {
    const bool sessionChanged = primarySession_ != session;
    primarySession_ = session;
    primaryLap_ = lapId;
    deltaCacheValid_ = false;
    lastPrimaryKey_ = session ? session->sessionKey() : QString();
    lastPrimaryLap_ = session ? lapId : -1;
    savePreferences();
    extraChannelCache_.clear();
    if (sessionChanged) setReferenceAlignment(0.0);
    cursorFrac_ = 0.0;
    loadCornersForPrimary();
    emit selectionChanged();
    emit cornersChanged();
}

void TelemetryStore::setCompare(SessionHandle* session, int lapId) {
    const bool sessionChanged = compareSession_ != session;
    lastCompareKey_ = session ? session->sessionKey() : QString();
    lastCompareLap_ = session ? lapId : -1;
    savePreferences();
    compareSession_ = session;
    compareLap_ = lapId;
    deltaCacheValid_ = false;
    if (sessionChanged) setReferenceAlignment(0.0);
    emit selectionChanged();
}

void TelemetryStore::selectLap(const QString& sessionKey, int lapId) {
    SessionHandle* s = findSession(sessionKey);
    if (!s) return;
    if (primarySession_ == s && primaryLap_ == lapId) return;
    setPrimary(s, lapId);
}

void TelemetryStore::compareLap(const QString& sessionKey, int lapId) {
    SessionHandle* s = findSession(sessionKey);
    if (!s) return;
    if (primarySession_ == s && primaryLap_ == lapId) return;
    setCompare(s, lapId);
}

void TelemetryStore::clearCompare() {
    compareSession_ = nullptr;
    compareLap_ = -1;
    lastCompareKey_.clear();
    lastCompareLap_ = -1;
    deltaCacheValid_ = false;
    setReferenceAlignment(0.0);
    savePreferences();
    emit selectionChanged();
}

void TelemetryStore::clearPrimary() {
    primarySession_ = nullptr;
    primaryLap_ = -1;
    compareSession_ = nullptr;
    compareLap_ = -1;
    lastPrimaryKey_.clear();
    lastPrimaryLap_ = -1;
    lastCompareKey_.clear();
    lastCompareLap_ = -1;
    deltaCacheValid_ = false;
    corners_.clear();
    savePreferences();
    emit selectionChanged();
    emit cornersChanged();
}

QVariantList TelemetryStore::lapsForSession(const QString& sessionKey) const {
    QVariantList out;
    SessionHandle* s = const_cast<TelemetryStore*>(this)->findSession(sessionKey);
    if (!s) return out;
    for (const LapEntry& l : s->laps()) {
        out.append(QVariantMap{{QStringLiteral("lapId"), l.lapId},
                               {QStringLiteral("label"), l.label},
                               {QStringLiteral("timeText"), l.timeText},
                               {QStringLiteral("timeMs"), l.timeMs},
                               {QStringLiteral("startTime"), l.startTime},
                               {QStringLiteral("isFastest"), l.isFastest},
                               {QStringLiteral("isOutlap"), l.isOutlap}});
    }
    return out;
}

// ── navigation ──────────────────────────────────────────────────────

void TelemetryStore::setCursorFrac(double v) {
    v = qBound(0.0, v, 1.0);
    if (qFuzzyCompare(v, cursorFrac_)) return;
    cursorFrac_ = v;
    emit cursorFracChanged();
}

void TelemetryStore::setChannelHeight(int v) {
    v = qBound(40, v, 400);
    if (v == channelHeight_) return;
    channelHeight_ = v;
    savePreferences();
    emit channelHeightChanged();
}

void TelemetryStore::setViewStart(double v) { viewStart_ = qBound(0.0, v, 1.0); emit viewChanged(); }
void TelemetryStore::setViewEnd(double v) { viewEnd_ = qBound(0.0, v, 1.0); emit viewChanged(); }

void TelemetryStore::zoomAt(double anchorFrac, double factor) {
    double span = viewSpan();
    double newSpan = qBound(0.002, span * factor, 1.0);
    double anchor = qBound(0.0, anchorFrac, 1.0);
    double fracOfView = (anchor - viewStart_) / span;
    double ns = anchor - fracOfView * newSpan;
    double ne = ns + newSpan;
    if (ns < 0) { ne -= ns; ns = 0; }
    if (ne > 1) { ns -= (ne - 1); ne = 1; ns = qBound(0.0, ns, 1.0); }
    viewStart_ = ns;
    viewEnd_ = ne;
    emit viewChanged();
}

void TelemetryStore::pan(double deltaFrac) {
    double span = viewSpan();
    double shift = deltaFrac * span;
    double ns = qBound(0.0, viewStart_ + shift, 1.0 - span);
    viewStart_ = ns;
    viewEnd_ = ns + span;
    emit viewChanged();
}

void TelemetryStore::moveCursorSteps(int steps) {
    const UnifiedLap* u = primaryUnified();
    if (!u || u->size() < 2) return;
    double stepFrac = 1.0 / double(u->size() - 1);
    setCursorFrac(cursorFrac_ + steps * stepFrac);
}

void TelemetryStore::jumpToFraction(double frac) {
    setCursorFrac(frac);
}

void TelemetryStore::setReferenceAlignment(double fraction) {
    fraction = qBound(-0.15, fraction, 0.15);
    if (qFuzzyCompare(referenceAlignment_ + 1.0, fraction + 1.0)) return;
    referenceAlignment_ = fraction;
    emit referenceAlignmentChanged();
}

void TelemetryStore::resetReferenceAlignment() {
    setReferenceAlignment(0.0);
}

double TelemetryStore::referenceAlignmentSeconds() const {
    const UnifiedLap* primary = primaryUnified();
    if (!primary || primary->time.empty()) return 0.0;
    return referenceAlignment_ * primary->time.back();
}

QVariantMap TelemetryStore::alignmentData(int points) const {
    QVariantMap result;
    const UnifiedLap* primary = primaryUnified();
    const UnifiedLap* compare = compareUnified();
    if (!primary || !compare) return result;
    points = qBound(64, points, 1200);

    auto frontDamper = [](const UnifiedLap& lap) {
        std::vector<double> values;
        const size_t both = std::min(lap.damperFL.size(), lap.damperFR.size());
        if (both > 0) {
            values.reserve(both);
            for (size_t i = 0; i < both; ++i)
                values.push_back((lap.damperFL[i] + lap.damperFR[i]) * 0.5);
        } else if (!lap.damperFL.empty()) {
            values = lap.damperFL;
        } else {
            values = lap.damperFR;
        }
        return values;
    };
    const std::vector<double> primaryDamper = frontDamper(*primary);
    const std::vector<double> compareDamper = frontDamper(*compare);
    if (primaryDamper.size() < 2 || compareDamper.size() < 2) return result;

    QVariantList primarySamples;
    QVariantList compareSamples;
    primarySamples.reserve(points);
    compareSamples.reserve(points);
    double minimum = 1e18;
    double maximum = -1e18;
    auto sample = [&](const std::vector<double>& values, int index) {
        const double position =
            double(index) / double(points - 1) * double(values.size() - 1);
        const int lo = std::clamp(int(std::floor(position)), 0,
                                  int(values.size()) - 1);
        const int hi = std::min(lo + 1, int(values.size()) - 1);
        return values[lo] + (values[hi] - values[lo]) * (position - lo);
    };
    for (int i = 0; i < points; ++i) {
        const double p = sample(primaryDamper, i);
        const double c = sample(compareDamper, i);
        primarySamples.append(p);
        compareSamples.append(c);
        minimum = std::min({minimum, p, c});
        maximum = std::max({maximum, p, c});
    }
    if (!(maximum > minimum)) {
        minimum -= 1.0;
        maximum += 1.0;
    }
    result.insert(QStringLiteral("primary"), primarySamples);
    result.insert(QStringLiteral("compare"), compareSamples);
    result.insert(QStringLiteral("min"), minimum);
    result.insert(QStringLiteral("max"), maximum);
    return result;
}

// ── corners ─────────────────────────────────────────────────────────

void TelemetryStore::autoGenerateCorners() {
    const UnifiedLap* u = primaryUnified();
    if (!u || (int)u->brake.size() < 100) return;
    double peak = 0;
    for (double b : u->brake) peak = std::max(peak, b);
    if (peak <= 2.0) return;
    double threshold = peak * 0.15;
    int n = (int)u->brake.size();
    QVector<QPair<int, int>> zones;
    bool inZone = false;
    int zoneStart = 0;
    for (int i = 0; i < n; ++i) {
        if (!inZone && u->brake[i] > threshold) {
            inZone = true;
            zoneStart = i;
        } else if (inZone && u->brake[i] < threshold * 0.5) {
            inZone = false;
            if (i - zoneStart > 5) zones.append({zoneStart, i});
        }
    }
    if (inZone && n - zoneStart > 5) zones.append({zoneStart, n - 1});
    QVector<QPair<int, int>> merged;
    for (auto& z : zones) {
        if (!merged.isEmpty() && z.first - merged.last().second < 50) {
            merged.last().second = z.second;
        } else {
            merged.append(z);
        }
    }
    corners_.clear();
    for (int i = 0; i < merged.size(); ++i) {
        int width = merged[i].second - merged[i].first;
        int approach = int(width * 0.3);
        int exitExt = int(width * 0.6);
        int start = std::max(0, merged[i].first - approach);
        int end = std::min(n - 1, merged[i].second + exitExt);
        CornerZone z;
        z.name = QStringLiteral("Turn %1").arg(i + 1);
        z.start = double(start) / double(n - 1);
        z.end = double(end) / double(n - 1);
        corners_.append(z);
    }
    emit cornersChanged();
}

void TelemetryStore::saveCorners() {
    if (!primarySession_) return;
    if (corners_.isEmpty()) return;
    QString track = primarySession_->track();
    QString safeName = track.toLower();
    safeName.replace(' ', '_').replace('-', '_');
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dir);
    QString path = dir + "/" + safeName + ".csv";
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    QTextStream out(&f);
    out << "name,start,end\n";
    auto sorted = corners_;
    std::sort(sorted.begin(), sorted.end(),
              [](const CornerZone& a, const CornerZone& b) { return a.start < b.start; });
    for (const auto& c : sorted)
        out << c.name << "," << QString::number(c.start, 'f', 6) << ","
            << QString::number(c.end, 'f', 6) << "\n";
}

QVariantList TelemetryStore::cornerList() const {
    QVariantList out;
    for (const auto& c : corners_) {
        out.append(QVariantMap{{"name", c.name}, {"start", c.start}, {"end", c.end}});
    }
    return out;
}
QVariantList TelemetryStore::cornerComparison() const {
    QVariantList out;
    const UnifiedLap* primary = primaryUnified();
    if (!primary || primary->size() < 2 ||
        primary->distance.size() < 2 || primary->time.size() < 2)
        return out;

    auto sample = [](const std::vector<double>& values, double fraction) {
        if (values.empty()) return 0.0;
        const double position =
            qBound(0.0, fraction, 1.0) * (values.size() - 1);
        const int lo = std::clamp(int(std::floor(position)), 0,
                                  int(values.size()) - 1);
        const int hi = std::min(lo + 1, int(values.size()) - 1);
        return values[lo] +
               (values[hi] - values[lo]) * (position - lo);
    };
    auto frontDamper = [](const UnifiedLap& lap) {
        const size_t count = std::min(lap.damperFL.size(),
                                      lap.damperFR.size());
        std::vector<double> values;
        if (count > 0) {
            values.reserve(count);
            for (size_t i = 0; i < count; ++i)
                values.push_back((lap.damperFL[i] + lap.damperFR[i]) * 0.5);
        } else if (!lap.damperFL.empty()) {
            values = lap.damperFL;
        } else {
            values = lap.damperFR;
        }
        return values;
    };

    auto stats = [&](const UnifiedLap& lap, const CornerZone& corner) {
        const int last = int(lap.size()) - 1;
        const int first =
            std::clamp(int(std::floor(corner.start * last)), 0, last);
        const int finish =
            std::clamp(int(std::ceil(corner.end * last)), first, last);
        double apex = 1e18;
        int apexIndex = first;
        double maxSteering = 0.0;
        double maxBrake = 0.0;
        double minThrottle = 1.0;
        int minGear = 99;
        double brakePoint = -1.0;
        double liftPoint = -1.0;
        const double startDistance =
            first < int(lap.distance.size()) ? lap.distance[first] : 0.0;

        for (int i = first; i <= finish; ++i) {
            if (i < int(lap.speed.size()) && lap.speed[i] < apex) {
                apex = lap.speed[i];
                apexIndex = i;
            }
            if (i < int(lap.steering.size()))
                maxSteering =
                    std::max(maxSteering, std::fabs(lap.steering[i]));
            if (i < int(lap.gear.size()))
                minGear = std::min(minGear, lap.gear[i]);
            if (i < int(lap.brake.size())) {
                maxBrake = std::max(maxBrake, lap.brake[i]);
                if (brakePoint < 0.0 && lap.brake[i] > 2.0 &&
                    i < int(lap.distance.size()))
                    brakePoint = lap.distance[i] - startDistance;
            }
            if (i < int(lap.throttle.size())) {
                minThrottle = std::min(minThrottle, lap.throttle[i]);
                if (liftPoint < 0.0 && lap.throttle[i] < 0.9 &&
                    i < int(lap.distance.size()))
                    liftPoint = lap.distance[i] - startDistance;
            }
        }
        if (apex == 1e18) apex = 0.0;
        if (minGear == 99) minGear = 0;
        if (brakePoint < 0.0) brakePoint = 0.0;
        if (liftPoint < 0.0) liftPoint = 0.0;

        int turnInIndex = first;
        if (apexIndex > first && !lap.steering.empty()) {
            const int approachSamples =
                std::max(3, (apexIndex - first) / 6);
            double baseline = 0.0;
            int baselineCount = 0;
            for (int i = first;
                 i < std::min(apexIndex, first + approachSamples); ++i) {
                if (i >= int(lap.steering.size())) break;
                baseline += std::fabs(lap.steering[i]);
                ++baselineCount;
            }
            if (baselineCount > 0) baseline /= baselineCount;
            const double threshold =
                baseline + std::max(1.0, (maxSteering - baseline) * 0.07);
            for (int i = first + approachSamples;
                 i <= apexIndex && i + 2 < int(lap.steering.size()); ++i) {
                if (std::fabs(lap.steering[i]) > threshold &&
                    std::fabs(lap.steering[i + 1]) > threshold &&
                    std::fabs(lap.steering[i + 2]) > threshold) {
                    turnInIndex = i;
                    break;
                }
            }
        }

        int throttleIndex = finish;
        const double noiseFloor =
            qBound(0.015, minThrottle + 0.015, 0.08);
        int pickupStart = first;
        for (int i = first; i <= finish &&
                             i < int(lap.throttle.size()); ++i) {
            if (lap.throttle[i] <= noiseFloor) {
                pickupStart = i;
                break;
            }
        }
        const double applicationTarget =
            qBound(0.10, minThrottle + 0.08, 0.30);
        int targetIndex = -1;
        for (int i = pickupStart;
             i + 3 <= finish && i + 3 < int(lap.throttle.size()); ++i) {
            bool sustained = true;
            for (int j = 0; j < 4; ++j)
                sustained = sustained &&
                             lap.throttle[i + j] >= applicationTarget;
            if (sustained) {
                targetIndex = i;
                break;
            }
        }
        if (targetIndex >= 0) {
            // The marker is the beginning of the rising application, not
            // the later point where the pedal reaches a useful target.
            throttleIndex = targetIndex;
            while (throttleIndex > pickupStart &&
                   lap.throttle[throttleIndex - 1] > noiseFloor &&
                   lap.throttle[throttleIndex] >=
                       lap.throttle[throttleIndex - 1] - 0.02)
                --throttleIndex;
        } else {
            for (int i = pickupStart + 1; i <= finish &&
                                          i < int(lap.throttle.size()); ++i) {
                if (lap.throttle[i] > noiseFloor &&
                    lap.throttle[i] > lap.throttle[i - 1] + 0.005) {
                    throttleIndex = i;
                    break;
                }
            }
        }
        const double sampleSpan = std::max(1, finish - first);
        const double turnInPosition =
            double(turnInIndex - first) / sampleSpan;
        const double apexPosition =
            double(apexIndex - first) / sampleSpan;
        const double throttlePosition =
            double(throttleIndex - first) / sampleSpan;
        auto distanceFromStart = [&](int index) {
            return index < int(lap.distance.size())
                       ? lap.distance[index] - startDistance
                       : 0.0;
        };
        const double turnInPoint = distanceFromStart(turnInIndex);
        const double apexPoint = distanceFromStart(apexIndex);
        const double throttlePoint = distanceFromStart(throttleIndex);

        constexpr int seriesPoints = 120;
        QVariantList speeds;
        QVariantList throttles;
        QVariantList brakes;
        QVariantList steerings;
        speeds.reserve(seriesPoints);
        throttles.reserve(seriesPoints);
        brakes.reserve(seriesPoints);
        steerings.reserve(seriesPoints);
        for (int i = 0; i < seriesPoints; ++i) {
            const double fraction =
                corner.start +
                (corner.end - corner.start) *
                    double(i) / double(seriesPoints - 1);
            speeds.append(sample(lap.speed, fraction));
            throttles.append(sample(lap.throttle, fraction));
            brakes.append(sample(lap.brake, fraction));
            steerings.append(sample(lap.steering, fraction));
        }

        const double entrySpeed = sample(lap.speed, corner.start);
        const double exitSpeed = sample(lap.speed, corner.end);
        return QVariantMap{
            {"entrySpeed", entrySpeed},
            {"apexSpeed", apex},
            {"exitSpeed", exitSpeed},
            {"speedDrop", entrySpeed - apex},
            {"speedGain", exitSpeed - apex},
            {"time", sample(lap.time, corner.end) -
                         sample(lap.time, corner.start)},
            {"minGear", minGear},
            {"maxSteering", maxSteering},
            {"maxBrake", maxBrake},
            {"minThrottle", minThrottle},
            {"brakePoint", brakePoint},
            {"liftPoint", liftPoint},
            {"turnInPosition", turnInPosition},
            {"apexPosition", apexPosition},
            {"throttlePosition", throttlePosition},
            {"turnInPoint", turnInPoint},
            {"apexPoint", apexPoint},
            {"throttlePoint", throttlePoint},
            {"speedSeries", speeds},
            {"throttleSeries", throttles},
            {"brakeSeries", brakes},
            {"steeringSeries", steerings},
        };
    };
    auto fractionAtDistance = [](const UnifiedLap& lap, double distance) {
        if (lap.distance.size() < 2) return 0.0;
        if (distance <= lap.distance.front()) return 0.0;
        if (distance >= lap.distance.back()) return 1.0;
        const auto it =
            std::lower_bound(lap.distance.begin(), lap.distance.end(), distance);
        const int hi = int(it - lap.distance.begin());
        const int lo = hi - 1;
        const double span = lap.distance[hi] - lap.distance[lo];
        const double local =
            span > 0.0 ? (distance - lap.distance[lo]) / span : 0.0;
        return (lo + local) / double(lap.distance.size() - 1);
    };

    const UnifiedLap* compare = compareUnified();
    if (compare &&
        (compare->distance.size() < 2 || compare->time.size() < 2))
        compare = nullptr;

    const std::vector<double> primaryDamper = frontDamper(*primary);
    const std::vector<double> compareDamper =
        compare ? frontDamper(*compare) : std::vector<double>();
    auto damperWindow = [](const UnifiedLap& lap,
                           const std::vector<double>& damper,
                           const CornerZone& corner) {
        QVariantMap result;
        const int last = std::min(int(lap.size()),
                                  int(lap.distance.size())) - 1;
        if (last < 2 || damper.size() < 2) return result;
        const int cornerStart = std::clamp(
            int(std::floor(corner.start * last)), 0, last);
        const int cornerEnd = std::clamp(
            int(std::ceil(corner.end * last)), cornerStart, last);
        const double startDistance = lap.distance[cornerStart];
        const double windowStart =
            std::max(lap.distance.front(), startDistance - 300.0);
        const double windowEnd = lap.distance[cornerEnd];
        const auto lowerIndex = [&](double distance) {
            const auto it = std::lower_bound(lap.distance.begin(),
                                             lap.distance.begin() + last + 1,
                                             distance);
            return std::clamp(int(it - lap.distance.begin()), 0, last);
        };
        const int first = lowerIndex(windowStart);
        const int finish = std::max(first, lowerIndex(windowEnd));
        if (finish - first < 2) return result;

        std::vector<double> baseline;
        baseline.reserve(finish - first + 1);
        for (int i = first; i <= finish; i += 4) {
            if (i < int(damper.size()) && std::isfinite(damper[i]))
                baseline.push_back(damper[i]);
        }
        if (baseline.empty()) return result;
        std::sort(baseline.begin(), baseline.end());
        const double median = baseline[baseline.size() / 2];

        int peak = first;
        double peakScore = -1.0;
        auto scoreAt = [&](int i) {
            return i < int(damper.size()) && std::isfinite(damper[i])
                       ? std::fabs(damper[i] - median)
                       : -1.0;
        };
        for (int i = first + 1; i < finish && i < int(damper.size()) - 1;
             ++i) {
            const double score = scoreAt(i);
            if (score >= scoreAt(i - 1) &&
                score >= scoreAt(i + 1) &&
                score > peakScore) {
                peak = i;
                peakScore = score;
            }
        }
        if (peakScore < 0.0) {
            for (int i = first; i <= finish && i < int(damper.size()); ++i) {
                const double score = scoreAt(i);
                if (score > peakScore) {
                    peak = i;
                    peakScore = score;
                }
            }
        }

        auto valueAtDistance = [&](double distance) {
            const int hi = lowerIndex(distance);
            if (hi <= 0) return damper[0];
            const int lo = hi - 1;
            const double span = lap.distance[hi] - lap.distance[lo];
            const double amount =
                span > 0.0 ? (distance - lap.distance[lo]) / span : 0.0;
            const double left = damper[std::min(lo, int(damper.size()) - 1)];
            const double right = damper[std::min(hi, int(damper.size()) - 1)];
            return left + (right - left) * qBound(0.0, amount, 1.0);
        };

        constexpr int points = 180;
        QVariantList samples;
        samples.reserve(points);
        const double span = std::max(1.0, windowEnd - windowStart);
        for (int i = 0; i < points; ++i) {
            const double distance =
                windowStart + span * double(i) / double(points - 1);
            samples.append(valueAtDistance(distance));
        }
        result.insert(QStringLiteral("series"), samples);
        result.insert(QStringLiteral("windowMeters"), span);
        result.insert(QStringLiteral("cornerStartMeters"),
                      startDistance - windowStart);
        result.insert(QStringLiteral("peakDistance"), lap.distance[peak]);
        result.insert(QStringLiteral("peakScore"), peakScore);
        return result;
    };

    for (const CornerZone& corner : corners_) {
        const QVariantMap primaryStats = stats(*primary, corner);
        const QVariantMap primaryDamperData =
            damperWindow(*primary, primaryDamper, corner);
        const bool hasPrimaryDamper =
            primaryDamperData.contains(QStringLiteral("series"));
        QVariantMap row{{"name", corner.name},
                        {"start", corner.start},
                        {"end", corner.end},
                        {"entrySpeed", primaryStats.value("entrySpeed")},
                        {"apexSpeed", primaryStats.value("apexSpeed")},
                        {"exitSpeed", primaryStats.value("exitSpeed")},
                        {"speedDrop", primaryStats.value("speedDrop")},
                        {"speedGain", primaryStats.value("speedGain")},
                        {"time", primaryStats.value("time")},
                        {"minGear", primaryStats.value("minGear")},
                        {"maxSteering", primaryStats.value("maxSteering")},
                        {"maxBrake", primaryStats.value("maxBrake")},
                        {"minThrottle", primaryStats.value("minThrottle")},
                        {"brakePoint", primaryStats.value("brakePoint")},
                        {"liftPoint", primaryStats.value("liftPoint")},
                        {"turnInPosition", primaryStats.value("turnInPosition")},
                        {"apexPosition", primaryStats.value("apexPosition")},
                        {"throttlePosition", primaryStats.value("throttlePosition")},
                        {"turnInPoint", primaryStats.value("turnInPoint")},
                        {"apexPoint", primaryStats.value("apexPoint")},
                        {"throttlePoint", primaryStats.value("throttlePoint")},
                        {"speedSeries", primaryStats.value("speedSeries")},
                        {"throttleSeries", primaryStats.value("throttleSeries")},
                        {"brakeSeries", primaryStats.value("brakeSeries")},
                        {"steeringSeries", primaryStats.value("steeringSeries")},
                        {"damperPrimarySeries",
                         primaryDamperData.value("series")},
                        {"damperWindowMeters",
                         primaryDamperData.value("windowMeters")},
                        {"damperCornerStartMeters",
                         primaryDamperData.value("cornerStartMeters")},
                        {"damperPeakPrimary",
                         primaryDamperData.value("peakDistance")},
                        {"damperCompareSeries", QVariantList{}},
                        {"damperAlignment", 0.0},
                        {"damperAlignmentValid", false},
                        {"hasCompare", compare != nullptr}};

        if (compare) {
            const double startDistance =
                sample(primary->distance, corner.start);
            const double endDistance =
                sample(primary->distance, corner.end);
            CornerZone compareCorner = corner;
            compareCorner.start =
                fractionAtDistance(*compare, startDistance);
            compareCorner.end =
                fractionAtDistance(*compare, endDistance);
            const QVariantMap compareStats =
                stats(*compare, compareCorner);
            const QVariantMap compareDamperData =
                damperWindow(*compare, compareDamper, compareCorner);
            double damperAlignment = 0.0;
            bool damperAlignmentValid = false;
            if (hasPrimaryDamper &&
                compareDamperData.contains(QStringLiteral("peakDistance"))) {
                damperAlignment =
                    primaryDamperData.value("peakDistance").toDouble() -
                    compareDamperData.value("peakDistance").toDouble();
                damperAlignmentValid = std::fabs(damperAlignment) < 50.0;
                if (!damperAlignmentValid) damperAlignment = 0.0;
            }
            row.insert(QStringLiteral("damperCompareSeries"),
                       compareDamperData.value("series"));
            row.insert(QStringLiteral("damperAlignment"),
                       damperAlignment);
            row.insert(QStringLiteral("damperAlignmentValid"),
                       damperAlignmentValid);
            row.insert(QStringLiteral("damperPeakCompare"),
                       compareDamperData.value("peakDistance"));


            const double timeDelta =
                primaryStats.value("time").toDouble() -
                compareStats.value("time").toDouble();
            const double entryDelta =
                primaryStats.value("entrySpeed").toDouble() -
                compareStats.value("entrySpeed").toDouble();
            const double apexDelta =
                primaryStats.value("apexSpeed").toDouble() -
                compareStats.value("apexSpeed").toDouble();
            const double exitDelta =
                primaryStats.value("exitSpeed").toDouble() -
                compareStats.value("exitSpeed").toDouble();
            const double brakePointDelta =
                primaryStats.value("brakePoint").toDouble() -
                compareStats.value("brakePoint").toDouble();
            const double liftPointDelta =
                primaryStats.value("liftPoint").toDouble() -
                compareStats.value("liftPoint").toDouble();
            const double turnInDelta =
                primaryStats.value("turnInPoint").toDouble() -
                compareStats.value("turnInPoint").toDouble();
            const double apexPointDelta =
                primaryStats.value("apexPoint").toDouble() -
                compareStats.value("apexPoint").toDouble();
            const double throttlePointDelta =
                primaryStats.value("throttlePoint").toDouble() -
                compareStats.value("throttlePoint").toDouble();
            const double score = qBound(
                0.0,
                50.0 - timeDelta * 40.0 +
                    exitDelta * 0.8 + apexDelta * 0.35,
                100.0);

            QStringList notes;
            if (timeDelta > 0.03)
                notes << QStringLiteral("Reference gains %1s through the corner")
                             .arg(timeDelta, 0, 'f', 3);
            else if (timeDelta < -0.03)
                notes << QStringLiteral("Primary gains %1s through the corner")
                             .arg(-timeDelta, 0, 'f', 3);
            if (exitDelta < -2.0)
                notes << QStringLiteral("Reference exits %1 km/h faster")
                             .arg(-exitDelta, 0, 'f', 1);
            if (apexDelta < -2.0)
                notes << QStringLiteral("Reference carries %1 km/h more at apex")
                             .arg(-apexDelta, 0, 'f', 1);
            if (primaryStats.value("maxSteering").toDouble() >
                compareStats.value("maxSteering").toDouble() + 12.0)
                notes << QStringLiteral("Primary uses more steering input");
            if (brakePointDelta < -5.0)
                notes << QStringLiteral("Primary brakes earlier");
            else if (brakePointDelta > 5.0)
                notes << QStringLiteral("Primary brakes later");
            if (std::fabs(turnInDelta) >= 10.0)
                notes << QStringLiteral("Turn-in %1m %2 than reference")
                             .arg(std::fabs(turnInDelta), 0, 'f', 0)
                             .arg(turnInDelta > 0.0
                                      ? QStringLiteral("later")
                                      : QStringLiteral("earlier"));
            if (std::fabs(throttlePointDelta) >= 10.0)
                notes << QStringLiteral("Throttle %1m %2 than reference")
                             .arg(std::fabs(throttlePointDelta), 0, 'f', 0)
                             .arg(throttlePointDelta > 0.0
                                      ? QStringLiteral("later")
                                      : QStringLiteral("earlier"));
            if (notes.isEmpty()) notes << QStringLiteral("Closely matched");

            row.insert("compareEntrySpeed",
                       compareStats.value("entrySpeed"));
            row.insert("compareApexSpeed",
                       compareStats.value("apexSpeed"));
            row.insert("compareExitSpeed",
                       compareStats.value("exitSpeed"));
            row.insert("compareTime", compareStats.value("time"));
            row.insert("compareMinGear", compareStats.value("minGear"));
            row.insert("compareMaxSteering",
                       compareStats.value("maxSteering"));
            row.insert("compareMaxBrake",
                       compareStats.value("maxBrake"));
            row.insert("compareMinThrottle",
                       compareStats.value("minThrottle"));
            row.insert("compareBrakePoint",
                       compareStats.value("brakePoint"));
            row.insert("compareLiftPoint",
                       compareStats.value("liftPoint"));
            row.insert("compareTurnInPosition",
                       compareStats.value("turnInPosition"));
            row.insert("compareApexPosition",
                       compareStats.value("apexPosition"));
            row.insert("compareThrottlePosition",
                       compareStats.value("throttlePosition"));
            row.insert("compareTurnInPoint",
                       compareStats.value("turnInPoint"));
            row.insert("compareApexPoint",
                       compareStats.value("apexPoint"));
            row.insert("compareThrottlePoint",
                       compareStats.value("throttlePoint"));
            row.insert("compareSpeedSeries",
                       compareStats.value("speedSeries"));
            row.insert("compareThrottleSeries",
                       compareStats.value("throttleSeries"));
            row.insert("compareBrakeSeries",
                       compareStats.value("brakeSeries"));
            row.insert("compareSteeringSeries",
                       compareStats.value("steeringSeries"));
            row.insert("delta", timeDelta);
            row.insert("entryDelta", entryDelta);
            row.insert("apexDelta", apexDelta);
            row.insert("exitDelta", exitDelta);
            row.insert("brakePointDelta", brakePointDelta);
            row.insert("liftPointDelta", liftPointDelta);
            row.insert("turnInDelta", turnInDelta);
            row.insert("apexPointDelta", apexPointDelta);
            row.insert("throttlePointDelta", throttlePointDelta);
            row.insert("score", score);
            row.insert("note", notes.join(QStringLiteral(" · ")));
        }
        out.append(row);
    }
    return out;
}


void TelemetryStore::updateCorner(int index, double start, double end) {
    if (index < 0 || index >= corners_.size()) return;
    corners_[index].start = qBound(0.0, start, 1.0);
    corners_[index].end = qBound(corners_[index].start, end, 1.0);
    emit cornersChanged();
}

void TelemetryStore::setEditingCorners(bool editing) {
    if (editingCorners_ == editing) return;
    editingCorners_ = editing;
    emit editingCornersChanged();
}

QString TelemetryStore::cornerNameAt(double frac) const {
    for (const auto& c : corners_)
        if (c.start <= frac && frac <= c.end) return c.name;
    return QString();
}

// ── channel config ──────────────────────────────────────────────────

bool TelemetryStore::channelVisible(const QString& key) const {
    if (channelVisible_.contains(key))
        return channelVisible_.value(key);
    if (key.startsWith(QStringLiteral("raw:"))) {
        QSettings s;
        s.beginGroup(QStringLiteral("channels"));
        const bool visible =
            s.value(key + QStringLiteral("/visible"), false).toBool();
        s.endGroup();
        channelVisible_.insert(key, visible);
        return visible;
    }
    return false;
}

void TelemetryStore::setChannelVisible(const QString& key, bool visible) {
    if (channelVisible(key) == visible) return;
    channelVisible_[key] = visible;
    if (key.startsWith(QStringLiteral("raw:"))) {
        QSettings s;
        s.beginGroup(QStringLiteral("channels"));
        s.setValue(key + QStringLiteral("/visible"), visible);
        s.endGroup();
    } else {
        savePreferences();
    }
    emit channelConfigChanged();
}
const std::vector<double>* TelemetryStore::extraChannelData(
    const QString& key, bool reference) const {
    if (!key.startsWith(QStringLiteral("raw:"))) return nullptr;
    SessionHandle* session =
        reference ? compareSession_ : primarySession_;
    const int lapId = reference ? compareLap_ : primaryLap_;
    if (!session || lapId < 0) return nullptr;

    const QString cacheKey = session->sessionKey() + QStringLiteral("|") +
                             QString::number(lapId) + QStringLiteral("|") +
                             key;
    auto cached = extraChannelCache_.constFind(cacheKey);
    if (cached != extraChannelCache_.cend())
        return cached.value().get();

    const racecraft::TelemetrySource* source = session->source();
    if (!source) return nullptr;
    const QString rawName = key.mid(4);
    const racecraft::RawChannel* channel = nullptr;
    for (const racecraft::RawChannel& candidate : source->channels()) {
        if (QString::fromStdString(candidate.name) == rawName) {
            channel = &candidate;
            break;
        }
    }
    if (!channel || channel->samples.size() < 2) return nullptr;

    const LapEntry* lap = nullptr;
    for (const LapEntry& candidate : session->laps()) {
        if (candidate.lapId == lapId) {
            lap = &candidate;
            break;
        }
    }
    if (!lap || lap->endTime <= lap->startTime) return nullptr;
    const double frequency = channel->frequencyHz > 0.0
                                  ? channel->frequencyHz : 50.0;
    const int first = std::clamp(
        int(std::lround(lap->startTime * frequency)), 0,
        int(channel->samples.size()) - 1);
    const int last = std::clamp(
        int(std::lround(lap->endTime * frequency)), first,
        int(channel->samples.size()) - 1);
    std::vector<double> slice(
        channel->samples.begin() + first,
        channel->samples.begin() + last + 1);
    auto values = std::make_shared<std::vector<double>>(
        racecraft::resample(slice, frequency, 50.0,
                            lap->endTime - lap->startTime));
    auto inserted = extraChannelCache_.insert(cacheKey, std::move(values));
    return inserted.value().get();
}


QVariantList TelemetryStore::channelSettings() const {
    QVariantList out;
    for (const QString& key : channelOrder_) {
        const auto meta = channelMetadata(key);
        out.append(QVariantMap{{"key", key},
                               {"title", meta.first},
                               {"unit", meta.second},
                               {"visible", channelVisible(key)},
                               {"color", channelColor(key)},
                               {"weight", channelWeight(key)}});
    }
    if (primarySession_) {
        const racecraft::TelemetrySource* source = primarySession_->source();
        if (source) {
            for (const racecraft::RawChannel& channel : source->channels()) {
                if (channel.samples.size() < 2) continue;
                const QString key =
                    QStringLiteral("raw:") +
                    QString::fromStdString(channel.name);
                out.append(QVariantMap{
                    {"key", key},
                    {"title", QString::fromStdString(channel.name)},
                    {"unit", QString::fromStdString(channel.unit)},
                    {"visible", channelVisible(key)},
                    {"color", channelColor(key)},
                    {"weight", channelWeight(key)},
                    {"source", true}});
            }
        }
    }
    return out;
}

QString TelemetryStore::channelColor(const QString& key) const {
    if (channelColors_.contains(key))
        return channelColors_.value(key).name(QColor::HexRgb);
    if (key.startsWith(QStringLiteral("raw:"))) {
        QSettings s;
        s.beginGroup(QStringLiteral("channels"));
        const QString value = s.value(
            key + QStringLiteral("/color"),
            defaultChannelColor(key).name(QColor::HexRgb)).toString();
        s.endGroup();
        const QColor parsed(value);
        const QColor result = parsed.isValid() ? parsed
                                               : defaultChannelColor(key);
        channelColors_.insert(key, result);
        return result.name(QColor::HexRgb);
    }
    return defaultChannelColor(key).name(QColor::HexRgb);
}

void TelemetryStore::setChannelColor(const QString& key, const QString& color) {
    QColor parsed(color);
    if (!parsed.isValid() || channelColor(key) == parsed.name(QColor::HexRgb))
        return;
    channelColors_[key] = parsed;
    if (key.startsWith(QStringLiteral("raw:"))) {
        QSettings s;
        s.beginGroup(QStringLiteral("channels"));
        s.setValue(key + QStringLiteral("/color"),
                   parsed.name(QColor::HexRgb));
        s.endGroup();
    } else {
        savePreferences();
    }
    emit channelConfigChanged();
}

double TelemetryStore::channelWeight(const QString& key) const {
    if (channelWeights_.contains(key))
        return channelWeights_.value(key);
    if (key.startsWith(QStringLiteral("raw:"))) {
        QSettings s;
        s.beginGroup(QStringLiteral("channels"));
        const double value = qBound(
            0.5, s.value(key + QStringLiteral("/weight"), 1.0).toDouble(),
            2.0);
        s.endGroup();
        channelWeights_.insert(key, value);
        return value;
    }
    return 1.0;
}

void TelemetryStore::setChannelWeight(const QString& key, double weight) {
    weight = qBound(0.5, weight, 2.0);
    if (qFuzzyCompare(channelWeight(key), weight)) return;
    channelWeights_[key] = weight;
    if (key.startsWith(QStringLiteral("raw:"))) {
        QSettings s;
        s.beginGroup(QStringLiteral("channels"));
        s.setValue(key + QStringLiteral("/weight"), weight);
        s.endGroup();
    } else {
        savePreferences();
    }
    emit channelConfigChanged();
}

QStringList TelemetryStore::channelOrder() const { return channelOrder_; }

QVariantList TelemetryStore::driverMappings() const {
    QVariantList out;
    QHash<QString, QString> all = driverMappings_;
    for (const auto& session : sessions_) {
        session->bestLapMs();
        const QString key = session->driverMappingKey();
        if (!key.isEmpty() && !all.contains(key))
            all.insert(key, session->driver());
    }
    QStringList keys = all.keys();
    std::sort(keys.begin(), keys.end());
    for (const QString& key : keys) {
        const QStringList parts = key.split('|');
        out.append(QVariantMap{
            {"key", key},
            {"carNumber", parts.value(0)},
            {"carClass", parts.value(1)},
            {"driverId", parts.value(2)},
            {"display", all.value(key)}});
    }
    return out;
}

void TelemetryStore::setDriverMapping(const QString& key,
                                      const QString& display) {
    const QString cleanKey = key.trimmed();
    if (cleanKey.isEmpty()) return;
    if (display.trimmed().isEmpty())
        driverMappings_.remove(cleanKey);
    else
        driverMappings_[cleanKey] = display.trimmed();
    savePreferences();
    emit driverMappingsChanged();
    emit sessionsChanged();
}

QString TelemetryStore::driverDisplayName(const QString& sessionKey) const {
    SessionHandle* session = findSession(sessionKey);
    if (!session) return QString();
    session->bestLapMs();
    return driverDisplay(session);
}

QVariantList TelemetryStore::driverAliases() const {
    QVariantList out;
    for (auto it = driverAliases_.cbegin(); it != driverAliases_.cend(); ++it)
        out.append(QVariantMap{{"detected", it.key()}, {"display", it.value()}});
    return out;
}

void TelemetryStore::setDriverAlias(const QString& detected, const QString& display) {
    const QString key = detected.trimmed();
    if (key.isEmpty()) return;
    if (display.trimmed().isEmpty())
        driverAliases_.remove(key);
    else
        driverAliases_[key] = display.trimmed();
    savePreferences();
    emit selectionChanged();
}

// ── unified access ──────────────────────────────────────────────────

const racecraft::UnifiedLap* TelemetryStore::primaryUnified() const {
    if (!primarySession_ || primaryLap_ < 0) return nullptr;
    auto u = primarySession_->unifiedLap(primaryLap_);
    return u ? u.get() : nullptr;
}

const racecraft::UnifiedLap* TelemetryStore::compareUnified() const {
    if (!compareSession_ || compareLap_ < 0) return nullptr;
    auto u = compareSession_->unifiedLap(compareLap_);
    return u ? u.get() : nullptr;
}

QVariantMap TelemetryStore::cursorReadout() const {
    QVariantMap out;
    const UnifiedLap* u = primaryUnified();
    if (!u || u->size() < 1) {
        out["dist"] = 0.0; out["time"] = 0.0; out["speed"] = 0.0; out["gear"] = 0;
        out["corner"] = QString();
        return out;
    }
    auto sampleAt = [&](const std::vector<double>& arr, double frac) {
        if (arr.empty()) return 0.0;
        double pos = qBound(0.0, frac, 1.0) * (arr.size() - 1);
        int lo = (int)std::floor(pos);
        int hi = std::min(lo + 1, (int)arr.size() - 1);
        return arr[lo] + (arr[hi] - arr[lo]) * (pos - lo);
    };
    auto sampleQtAt = [&](const QVector<double>& arr, double frac) {
        if (arr.isEmpty()) return 0.0;
        double pos = qBound(0.0, frac, 1.0) * (arr.size() - 1);
        int lo = (int)std::floor(pos);
        int hi = std::min<int>(lo + 1, int(arr.size()) - 1);
        return arr[lo] + (arr[hi] - arr[lo]) * (pos - lo);
    };
    double frac = cursorFrac_;
    out["dist"] = sampleAt(u->distance, frac);
    out["time"] = sampleAt(u->time, frac);
    out["speed"] = sampleAt(u->speed, frac);
    out["gear"] = u->gear.empty() ? 0 : u->gear[qBound(0.0, frac, 1.0) * (u->gear.size() - 1)];
    out["corner"] = cornerNameAt(frac);
    // Δ vs compare lap (array from the shared cached deltaTrace())
    const QVector<double>& d = deltaTrace();
    if (!d.isEmpty())
        out["delta"] = sampleQtAt(d, frac);
    return out;
}

const QVector<double>& TelemetryStore::deltaTrace() const {
    if (deltaCacheValid_) return deltaCache_;
    deltaCache_.clear();
    deltaCacheValid_ = true;

    const UnifiedLap* a = primaryUnified();
    const UnifiedLap* b = compareUnified();
    if (!a || !b || a->size() < 3 || b->size() < 3 ||
        a->distance.size() < 2 || b->distance.size() < 2 ||
        a->time.size() < 2 || b->time.size() < 2)
        return deltaCache_;

    // Distance-aligned cumulative Δ (racecraft semantics): for each primary
    // sample, reference time at the same cumulative distance; offset so the
    // trace starts at zero.
    const int n = int(a->size());
    const int m = int(b->size());
    deltaCache_.resize(n);

    int j = 0;
    double base = 0.0;
    for (int i = 0; i < n; ++i) {
        const double distance = a->distance[i];
        double referenceTime = 0.0;
        if (distance <= b->distance.front()) {
            referenceTime = b->time.front();
        } else if (distance >= b->distance.back()) {
            referenceTime = b->time.back();
        } else {
            while (j + 1 < m && b->distance[j + 1] < distance) ++j;
            if (b->distance[j + 1] > b->distance[j]) {
                const double local = (distance - b->distance[j]) /
                                     (b->distance[j + 1] - b->distance[j]);
                referenceTime = b->time[j] +
                                local * (b->time[j + 1] - b->time[j]);
            } else {
                referenceTime = b->time[j];
            }
        }
        const double raw = a->time[i] - referenceTime;
        if (i == 0) base = raw;
        deltaCache_[i] = raw - base;
    }
    return deltaCache_;
}

// ── labels ──────────────────────────────────────────────────────────

QString TelemetryStore::primaryLabel() const {
    if (!primarySession_) return QString();
    QString s = primarySession_->track();
    if (s.isEmpty()) s = primarySession_->stem();
    return s;
}

QString TelemetryStore::primaryDetail() const {
    if (!primarySession_) return QString();
    QString s = driverDisplay(primarySession_);
    if (primaryLap_ >= 0) {
        for (const auto& l : primarySession_->laps())
            if (l.lapId == primaryLap_) {
                s += " · " + l.label + " " + l.timeText;
                break;
            }
    }
    return s;
}

QString TelemetryStore::compareLabel() const {
    if (!compareSession_) return QString();
    QString s = driverDisplay(compareSession_);
    for (const auto& l : compareSession_->laps())
        if (l.lapId == compareLap_) {
            s += " " + l.timeText;
            break;
        }
    return s;
}

QString TelemetryStore::roomName() const {
    return primaryLabel();
}

QString TelemetryStore::primarySessionKey() const {
    return primarySession_ ? primarySession_->sessionKey() : QString();
}

QString TelemetryStore::compareSessionKey() const {
    return compareSession_ ? compareSession_->sessionKey() : QString();
}
double TelemetryStore::sessionStartUnixTime() const {
    return 0.0;
}

bool TelemetryStore::hasGlobalTime() const {
    return false;
}

