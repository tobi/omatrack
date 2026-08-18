#include "TrackAtlasManager.h"

#include "TrackAtlasSpatial.h"
#include "core/TelemetryEngine.h"
#include "core/MonotonicSeries.h"
#include "RemoteCache.h"
#include "VerboseLog.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

constexpr qint64 kTrackAtlasMaxAgeSeconds = 24 * 60 * 60;
constexpr int kTrackAtlasCheckIntervalMs = 6 * 60 * 60 * 1000;
const QUrl kTrackAtlasUrl(QStringLiteral(
    "https://raw.githubusercontent.com/tobi/track-atlas/main/tracks.jsonl"));
const QString kTrackAtlasRawBase = QStringLiteral(
    "https://raw.githubusercontent.com/tobi/track-atlas/main/tracks/");

QString legacyAppDataPath() {
    return QStandardPaths::writableLocation(
               QStandardPaths::GenericDataLocation) +
           QStringLiteral("/racecraft/racecraft-qt");
}

QString normalizeAtlasName(QString value) {
    value = value.toLower();
    value.remove(QRegularExpression(QStringLiteral("[^a-z0-9]")));
    return value;
}

QString atlasGeometryKey(const QString& trackSlug, const QString& layoutId) {
    return trackSlug + QLatin1Char('/') + layoutId;
}

}  // namespace

using namespace omatrack;

TrackAtlasManager::TrackAtlasManager(QObject* parent)
    : QObject(parent),
      atlasRefreshJob_(this),
      atlasCenterlineQueue_(this),
      atlasCacheQueue_(this) {}

void TrackAtlasManager::startup() {
    atlasTimer_ = new QTimer(this);
    atlasTimer_->setInterval(kTrackAtlasCheckIntervalMs);
    connect(atlasTimer_, &QTimer::timeout, this,
            [this]() { updateTrackAtlas(false); });
    atlasTimer_->start();
    loadTrackAtlasCache();
    QTimer::singleShot(0, this, [this]() { updateTrackAtlas(false); });
}

QString TrackAtlasManager::trackAtlasCachePath() const {
    if (!atlasCachePathReady_) {
        const QString directory =
            QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
            QStringLiteral("/track-atlas");
        // The directory and a one-time legacy cache copy are ensured once at
        // startup (see loadTrackAtlasCache); this accessor only computes
        // the path so it never touches the filesystem on a QML-click path.
        atlasCachePath_ = directory + QStringLiteral("/tracks.jsonl");
        atlasCachePathReady_ = true;
    }
    return atlasCachePath_;
}

QString TrackAtlasManager::trackAtlasGeometryCachePath(
    const QString& trackSlug, const QString& layoutId) const {
    // No mkpath here: the geometry directory is created inside the
    // QtConcurrent job that writes the file, never on the GUI thread.
    const QString directory = QFileInfo(trackAtlasCachePath()).absolutePath() +
                              QStringLiteral("/geometry");
    auto safeName = [](QString value) {
        value.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]")),
                      QStringLiteral("_"));
        return value;
    };
    return directory + QLatin1Char('/') + safeName(trackSlug) +
           QStringLiteral("__") + safeName(layoutId) +
           QStringLiteral(".geojson");
}

bool TrackAtlasManager::ensureAtlasCenterline(const QString& trackSlug,
                                              const QJsonObject& layout) {
    const QString layoutId = layout.value(QStringLiteral("id")).toString();
    if (trackSlug.isEmpty() || layoutId.isEmpty()) return false;
    const QString key = atlasGeometryKey(trackSlug, layoutId);
    if (atlasCenterlines_.contains(key)) return true;
    // The cache read (file open + parse + stat) runs off the GUI thread; this
    // returns false immediately and the result arrives through the watcher.
    if (atlasGeometryRequests_.contains(key)) return false;
    loadAtlasCenterlineFromCache(trackSlug, layout);
    return false;
}

void TrackAtlasManager::loadAtlasCenterlineFromCache(
    const QString& trackSlug, const QJsonObject& layout) {
    const QString layoutId = layout.value(QStringLiteral("id")).toString();
    const QString key = atlasGeometryKey(trackSlug, layoutId);
    const QString cachePath = trackAtlasGeometryCachePath(trackSlug, layoutId);
    atlasGeometryRequests_.insert(key);
    atlasCacheQueue_.enqueue(
        key,
        [cachePath](IoCancel) {
            std::pair<QVector<QPointF>, bool> out{{}, true};
            const QFileInfo info(cachePath);
            if (info.exists()) {
                QFile cache(cachePath);
                if (cache.open(QIODevice::ReadOnly))
                    out.first =
                        omatrack::trackatlas::parseCenterline(cache.readAll());
                const qint64 ageSeconds =
                    info.lastModified().secsTo(QDateTime::currentDateTimeUtc());
                out.second =
                    ageSeconds < 0 || ageSeconds >= kTrackAtlasMaxAgeSeconds;
            }
            return out;
        },
        [this, key, trackSlug,
         layout](std::pair<QVector<QPointF>, bool> result) {
            atlasGeometryRequests_.remove(key);
            if (!result.first.isEmpty()) {
                atlasCenterlines_.insert(key, result.first);
                atlasSpatialMappings_.clear();
                emit cornersNeedReload();
            }
            if (result.first.isEmpty() || result.second)
                requestAtlasCenterline(trackSlug, layout);
        });
}

void TrackAtlasManager::requestAtlasCenterline(const QString& trackSlug,
                                               const QJsonObject& layout) {
    const QString layoutId = layout.value(QStringLiteral("id")).toString();
    const QJsonObject geometry =
        layout.value(QStringLiteral("geometry")).toObject();
    const QString crs = geometry.value(QStringLiteral("crs")).toString();
    QString relativePath =
        geometry.value(QStringLiteral("centerline")).toString();
    relativePath = QDir::cleanPath(relativePath);
    const QString key = atlasGeometryKey(trackSlug, layoutId);
    static const QRegularExpression safeSlug(
        QStringLiteral("^[A-Za-z0-9._-]+$"));
    if (!safeSlug.match(trackSlug).hasMatch() || layoutId.isEmpty() ||
        relativePath.isEmpty() || relativePath == QStringLiteral(".") ||
        relativePath.startsWith(QStringLiteral("../")) ||
        relativePath.startsWith(QLatin1Char('/')) ||
        (!crs.isEmpty() && crs != QStringLiteral("EPSG:4326")) ||
        atlasGeometryRequests_.contains(key))
        return;

    atlasGeometryRequests_.insert(key);
    if (!atlasCenterlines_.contains(key)) {
        trackAtlasStatus_ = QStringLiteral("Updating track-atlas geometry…");
        emit changed();
    }

    const QUrl url(kTrackAtlasRawBase + trackSlug + QStringLiteral("/raw/") +
                   relativePath);
    const QString version = QCoreApplication::applicationVersion();
    const QString cachePath = trackAtlasGeometryCachePath(trackSlug, layoutId);
    atlasCenterlineQueue_.enqueue(
        key,
        [url, version, cachePath](IoCancel cancel) {
            const RequestFactory build = [version](const QUrl& hop) {
                QNetworkRequest request = makeRequest(hop);
                request.setHeader(QNetworkRequest::UserAgentHeader,
                                  QStringLiteral("Omatrack/") + version);
                return request;
            };
            QDir().mkpath(QFileInfo(cachePath).absolutePath());
            const FileDownload download =
                downloadFile(url, build, cachePath, {}, cancel);
            if (ioCancelled(cancel) || download.status != 200)
                return QByteArray();
            QFile cached(cachePath);
            if (!cached.open(QIODevice::ReadOnly)) return QByteArray();
            return cached.readAll();
        },
        [this, key](QByteArray payload) {
            atlasGeometryRequests_.remove(key);
            if (payload.isEmpty()) {
                if (!atlasCenterlines_.contains(key)) {
                    trackAtlasStatus_ = QStringLiteral(
                        "Track-atlas layout geometry unavailable");
                    emit changed();
                }
                return;
            }
            QVector<QPointF> centerline =
                omatrack::trackatlas::parseCenterline(payload);
            if (centerline.isEmpty()) {
                if (!atlasCenterlines_.contains(key)) {
                    trackAtlasStatus_ =
                        QStringLiteral("Track-atlas layout geometry invalid");
                    emit changed();
                }
                return;
            }
            atlasCenterlines_.insert(key, std::move(centerline));
            atlasSpatialMappings_.clear();
            emit cornersNeedReload();
        });
}

bool TrackAtlasManager::parseTrackAtlas(const QByteArray& payload) {
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
    emit changed();
    emit cornersNeedReload();
    return true;
}

void TrackAtlasManager::loadTrackAtlasCache() {
    // Ensure the cache directory exists and migrate the pre-rename cache once.
    // This runs only at startup; trackAtlasCachePath() itself never does I/O.
    const QString currentPath = trackAtlasCachePath();
    const QString directory = QFileInfo(currentPath).absolutePath();
    QDir().mkpath(directory);
    if (!QFile::exists(currentPath)) {
        const QString legacy =
            legacyAppDataPath() + QStringLiteral("/track-atlas/tracks.jsonl");
        if (QFile::exists(legacy) && !QFile::copy(legacy, currentPath))
            qWarning() << "Unable to migrate Track Atlas cache" << legacy;
    }
    QFile cache(currentPath);
    if (!cache.open(QIODevice::ReadOnly)) {
        cache.setFileName(legacyAppDataPath() +
                          QStringLiteral("/track-atlas/tracks.jsonl"));
        if (!cache.open(QIODevice::ReadOnly)) {
            qCInfo(lcIo).noquote() << "track-atlas cache miss none";
            trackAtlasStatus_ = QStringLiteral("No track-atlas cache");
            emit changed();
            return;
        }
    }
    const QByteArray payload = cache.readAll();
    qCInfo(lcIo).noquote() << "track-atlas cache hit"
                           << omatrack::displayPath(cache.fileName())
                           << omatrack::formatBytes(payload.size());
    if (!parseTrackAtlas(payload))
        trackAtlasStatus_ = QStringLiteral("Invalid track-atlas cache");
}

void TrackAtlasManager::refreshTrackAtlas() { updateTrackAtlas(true); }

void TrackAtlasManager::updateTrackAtlas(bool force) {
    const QFileInfo cache(trackAtlasCachePath());
    if (!force && cache.exists()) {
        const qint64 ageSeconds =
            cache.lastModified().secsTo(QDateTime::currentDateTimeUtc());
        if (ageSeconds >= 0 && ageSeconds < kTrackAtlasMaxAgeSeconds) {
            qCInfo(lcIo).noquote()
                << "track-atlas skip fetch age" << ageSeconds << "s"
                << omatrack::displayPath(cache.absoluteFilePath());
            return;
        }
    }

    // A refresh supersedes any pending centerline geometry fetches.
    atlasCenterlineQueue_.clear();
    const QString version = QCoreApplication::applicationVersion();
    const QString cachePath = trackAtlasCachePath();
    trackAtlasStatus_ = QStringLiteral("Updating track-atlas…");
    emit changed();
    atlasRefreshJob_.start(
        [version, cachePath](IoCancel cancel) {
            const RequestFactory build = [version](const QUrl& hop) {
                QNetworkRequest request = makeRequest(hop);
                request.setHeader(QNetworkRequest::UserAgentHeader,
                                  QStringLiteral("Omatrack/") + version);
                return request;
            };
            // The shared downloadFile/QSaveFile helper streams straight to
            // the cache; the body is read back so parseTrackAtlas can run on
            // the GUI thread without re-opening the file.
            const FileDownload download =
                downloadFile(kTrackAtlasUrl, build, cachePath, {}, cancel);
            if (ioCancelled(cancel) || download.status != 200) {
                qCInfo(lcIo).noquote() << "track-atlas fetch failed status"
                                       << download.status << download.error;
                return QByteArray();
            }
            QFile cached(cachePath);
            if (!cached.open(QIODevice::ReadOnly)) {
                qCInfo(lcIo).noquote() << "write track-atlas failed"
                                       << omatrack::displayPath(cachePath);
                return QByteArray();
            }
            const QByteArray payload = cached.readAll();
            qCInfo(lcIo).noquote()
                << "write track-atlas" << omatrack::displayPath(cachePath)
                << omatrack::formatBytes(payload.size());
            return payload;
        },
        [this](QByteArray payload) {
            if (payload.isEmpty()) {
                trackAtlasStatus_ =
                    atlasTracks_.isEmpty()
                        ? QStringLiteral("Track-atlas unavailable")
                        : QStringLiteral("Track-atlas cache in use");
                emit changed();
                return;
            }
            if (!parseTrackAtlas(payload)) {
                trackAtlasStatus_ =
                    QStringLiteral("Track-atlas update invalid");
                emit changed();
            }
        });
}

QVector<CornerZone> TrackAtlasManager::cornersForPrimary(
    SessionHandle* session, int lapId, const QString& resolvedSlug) {
    QVector<CornerZone> result;
    if (!session || lapId < 0 || atlasTracks_.isEmpty()) return result;

    const QString assignedSlug = resolvedSlug;
    const QString wanted = normalizeAtlasName(
        assignedSlug.isEmpty() ? session->track() : assignedSlug);
    if (wanted.isEmpty()) return result;

    QJsonObject matchedTrack;
    QString matchedTrackSlug;
    int bestTrackScore = std::numeric_limits<int>::max();
    for (auto it = atlasTracks_.cbegin(); it != atlasTracks_.cend(); ++it) {
        const QJsonObject track = it.value();
        if (!assignedSlug.isEmpty() && it.key() != assignedSlug) continue;
        QStringList names{
            track.value(QStringLiteral("slug")).toString(),
            track.value(QStringLiteral("name")).toString(),
        };
        for (const QJsonValue& alias :
             track.value(QStringLiteral("aka")).toArray())
            names.append(alias.toString());
        const QJsonObject external =
            track.value(QStringLiteral("external_ids")).toObject();
        for (auto externalIt = external.begin(); externalIt != external.end();
             ++externalIt)
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
                matchedTrackSlug = it.key();
            }
        }
    }
    if (matchedTrack.isEmpty() ||
        bestTrackScore == std::numeric_limits<int>::max())
        return result;

    const auto unified = session->unifiedLap(lapId);
    const double lapLength =
        unified && !unified->distance.empty() ? unified->distance.back() : 0.0;
    const QString sessionStem = session->stem().toLower();

    QJsonObject matchedLayout;
    double bestLayoutScore = std::numeric_limits<double>::max();
    for (const QJsonValue& layoutValue :
         matchedTrack.value(QStringLiteral("layouts")).toArray()) {
        const QJsonObject layout = layoutValue.toObject();
        const double declaredLength =
            layout.value(QStringLiteral("length_m")).toDouble(0.0);
        double score = declaredLength > 0.0 && lapLength > 0.0
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

    const bool gpsAvailable =
        unified && omatrack::trackatlas::hasPositionalGps(*unified);
    QVector<QPointF> spatialMapping;
    if (gpsAvailable) {
        const QString layoutId =
            matchedLayout.value(QStringLiteral("id")).toString();
        const QString geometryKey =
            atlasGeometryKey(matchedTrackSlug, layoutId);
        if (!ensureAtlasCenterline(matchedTrackSlug, matchedLayout)) {
            if (!atlasGeometryRequests_.contains(geometryKey)) {
                trackAtlasStatus_ = QStringLiteral(
                    "Track-atlas layout geometry unavailable; GPS corners "
                    "hidden");
                emit changed();
            }
            return result;
        }
        const QString mappingKey = session->sessionKey() + QLatin1Char('|') +
                                   QString::number(lapId) + QLatin1Char('|') +
                                   geometryKey;
        spatialMapping = atlasSpatialMappings_.value(mappingKey);
        if (spatialMapping.isEmpty()) {
            spatialMapping = omatrack::trackatlas::spatialStationMap(
                *unified, atlasCenterlines_.value(geometryKey));
            if (!spatialMapping.isEmpty())
                atlasSpatialMappings_.insert(mappingKey, spatialMapping);
        }
        if (spatialMapping.isEmpty()) {
            trackAtlasStatus_ = QStringLiteral(
                "Track-atlas GPS corner match failed; corners hidden");
            emit changed();
            return result;
        }
        trackAtlasStatus_ =
            QStringLiteral("%1 tracks cached · GPS-mapped corners")
                .arg(atlasTracks_.size());
        emit changed();
    } else {
        trackAtlasStatus_ =
            QStringLiteral("%1 tracks cached · distance fallback (no GPS)")
                .arg(atlasTracks_.size());
        emit changed();
    }

    const QString labelDefault =
        matchedLayout.value(QStringLiteral("label_default"))
            .toString(QStringLiteral("numbered"));
    auto fractionAtMarker = [&](double normalizedDistance) {
        if (gpsAvailable)
            return omatrack::trackatlas::lapFractionAtStation(
                spatialMapping, normalizedDistance);
        if (!unified || unified->distance.size() < 2 || lapLength <= 0.0)
            return qBound(0.0, normalizedDistance, 1.0);
        const double target = qBound(0.0, normalizedDistance, 1.0) * lapLength;
        return omatrack::invertFraction(unified->distance, target);
    };

    for (const QJsonValue& rangeValue : ranges) {
        const QJsonObject range = rangeValue.toObject();
        const QString anchor = range.value(QStringLiteral("anchor")).toString();
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
        corner.start =
            fractionAtMarker(range.value(QStringLiteral("start")).toDouble());
        corner.end =
            fractionAtMarker(range.value(QStringLiteral("end")).toDouble());
        if (!corner.name.isEmpty() && corner.end > corner.start)
            result.append(corner);
    }
    std::sort(result.begin(), result.end(),
              [](const CornerZone& a, const CornerZone& b) {
                  return a.start < b.start;
              });
    return result;
}
