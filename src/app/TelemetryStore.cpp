#include "TelemetryStore.h"

#include "ComparisonAlignment.h"
#include "SessionMetadataCache.h"
#include "TrackMetadata.h"
#include "core/TelemetryEngine.h"
#include "YamlConfig.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDateTime>
#include <QElapsedTimer>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJSEngine>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QRegularExpression>
#include <QSslError>
#include <QSaveFile>
#include <QSettings>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>
#include <QUrl>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace {
constexpr qint64 kTrackAtlasMaxAgeSeconds = 24 * 60 * 60;
constexpr int kTrackAtlasCheckIntervalMs = 6 * 60 * 60 * 1000;
const QUrl kTrackAtlasUrl(QStringLiteral(
    "https://raw.githubusercontent.com/tobi/track-atlas/main/tracks.jsonl"));
QPointer<TelemetryStore> s_storeInstance;
QString legacyAppDataPath() {
    return QStandardPaths::writableLocation(
               QStandardPaths::GenericDataLocation) +
           QStringLiteral("/racecraft/racecraft-qt");
}
QString telemetryPathForInput(const QString& path) {
    if (QFileInfo(path).suffix().compare(QStringLiteral("ldx"),
                                         Qt::CaseInsensitive) != 0)
        return path;
    QString stem = path;
    stem.chop(3);
    for (const QString& extension :
         {QStringLiteral("ld"), QStringLiteral("LD")}) {
        const QString companion = stem + extension;
        if (QFileInfo::exists(companion)) return companion;
    }
    return {};
}

QDate sessionDate(const SessionHandle* session) {
    if (!session) return {};
    QDate date =
        QDate::fromString(session->date(), QStringLiteral("dd/MM/yyyy"));
    if (date.isValid()) return date;
    return QDate::fromString(QFileInfo(session->path()).dir().dirName(),
                             Qt::ISODate);
}

QString normalizeAtlasName(QString value) {
    value = value.toLower();
    value.remove(QRegularExpression(QStringLiteral("[^a-z0-9]")));
    return value;
}

double geoDistanceKm(double firstLat, double firstLon, double secondLat,
                     double secondLon) {
    constexpr double kEarthRadiusKm = 6371.0088;
    constexpr double kRadiansPerDegree = 3.14159265358979323846 / 180.0;
    const double lat1 = firstLat * kRadiansPerDegree;
    const double lat2 = secondLat * kRadiansPerDegree;
    const double deltaLat = (secondLat - firstLat) * kRadiansPerDegree;
    const double deltaLon = (secondLon - firstLon) * kRadiansPerDegree;
    const double sinLat = std::sin(deltaLat * 0.5);
    const double sinLon = std::sin(deltaLon * 0.5);
    const double a =
        sinLat * sinLat + std::cos(lat1) * std::cos(lat2) * sinLon * sinLon;
    return 2.0 * kEarthRadiusKm * std::asin(std::sqrt(std::clamp(a, 0.0, 1.0)));
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

std::pair<QString, QString> channelMetadata(const QString& key) {
    static const QHash<QString, std::pair<QString, QString>> metadata = {
        {"speed", {"Speed", "km/h"}},
        {"throttle", {"Throttle", "%"}},
        {"brake", {"Brake", "bar"}},
        {"steering", {"Steering", "deg"}},
        {"gear", {"Gear", ""}},
        {"dampers", {"Dampers", "mm"}},
        {"g_long", {"G Long", "g"}},
        {"delta", {"Δ Time", "s"}},
        {"clutch", {"Clutch", "%"}},
        {"driver_throttle", {"Driver throttle", "%"}},
        {"gps_lat", {"GPS latitude", "°"}},
        {"gps_lon", {"GPS longitude", "°"}},
    };
    return metadata.value(key, {key, {}});
}

/// YAML scalars arrive as strings; QVariant("false").toBool() is true.
bool yamlBool(const QVariant& value, bool fallback) {
    if (!value.isValid()) return fallback;
    if (value.typeId() == QMetaType::Bool) return value.toBool();
    const QString text = value.toString().trimmed().toLower();
    if (text == QStringLiteral("true") || text == QStringLiteral("1") ||
        text == QStringLiteral("yes"))
        return true;
    if (text == QStringLiteral("false") || text == QStringLiteral("0") ||
        text == QStringLiteral("no"))
        return false;
    return fallback;
}

QVariant nestedValue(QVariantMap node, const QStringList& path) {
    for (int index = 0; index < path.size(); ++index) {
        const auto it = node.constFind(path.at(index));
        if (it == node.cend()) return {};
        if (index == path.size() - 1) return it.value();
        if (it.value().typeId() != QMetaType::QVariantMap) return {};
        node = it.value().toMap();
    }
    return {};
}

QString nestedText(const QVariantMap& node, const QStringList& path) {
    return nestedValue(node, path).toString().trimmed();
}

QString canonicalInputPath(const QString& path) {
    const QFileInfo info(path);
    if (!info.exists() || !info.isFile()) return {};
    const QString canonical = info.canonicalFilePath();
    return canonical.isEmpty() ? info.absoluteFilePath() : canonical;
}

QString canonicalDirectoryPath(const QString& path) {
    const QFileInfo info(path);
    if (!info.exists() || !info.isDir()) return {};
    const QString canonical = info.canonicalFilePath();
    return canonical.isEmpty() ? info.absoluteFilePath() : canonical;
}

bool isVideoPath(const QString& path) {
    static const QSet<QString> extensions{
        QStringLiteral("mp4"), QStringLiteral("mov"), QStringLiteral("mkv"),
        QStringLiteral("avi"), QStringLiteral("m4v"), QStringLiteral("webm")};
    return extensions.contains(QFileInfo(path).suffix().toLower());
}

QStringList trackMetadataPaths(const QStringList& discovered,
                               const QString& directoryPath) {
    QSet<QString> paths(discovered.cbegin(), discovered.cend());
    const QStringList inherited =
        omatrack::track_metadata::hierarchyPaths(directoryPath);
    for (const QString& path : inherited)
        paths.insert(QFileInfo(path).absoluteFilePath());
    QStringList result(paths.cbegin(), paths.cend());
    std::sort(result.begin(), result.end());
    return result;
}

struct MetadataChannelDefinition {
    const char* key;
    const char* label;
    const char* unit;
    const char* detail;
};

const std::vector<MetadataChannelDefinition>& metadataChannelDefinitions() {
    static const std::vector<MetadataChannelDefinition> definitions{
        {"driver_id", "Driver ID channel", "numeric code",
         "Numeric logger code identifying the active driver"},
        {"speed", "Speed", "km/h", "Vehicle or corrected speed"},
        {"throttle", "Throttle", "0–1", "Powertrain throttle position"},
        {"driver_throttle", "Driver throttle", "0–1", "Pedal demand before TC"},
        {"brake", "Brake pressure", "bar", "Front brake pressure"},
        {"clutch", "Clutch", "0–1", "Clutch position"},
        {"steering", "Steering", "deg", "Steering-wheel angle"},
        {"gear", "Gear", "integer", "Selected gear"},
        {"g_long", "Longitudinal G", "g", "Longitudinal acceleration"},
        {"distance", "Lap distance", "m", "Native corrected lap distance"},
        {"damper_fl", "Damper FL", "mm", "Front-left damper travel"},
        {"damper_fr", "Damper FR", "mm", "Front-right damper travel"},
        {"damper_rl", "Damper RL", "mm", "Rear-left damper travel"},
        {"damper_rr", "Damper RR", "mm", "Rear-right damper travel"},
        {"gps_lat", "GPS latitude", "deg", "Latitude position"},
        {"gps_lon", "GPS longitude", "deg", "Longitude position"},
        {"gps_speed", "GPS speed", "m/s", "Receiver Doppler speed"},
        {"gps_position_accuracy", "GPS position accuracy", "m",
         "Reported position uncertainty"},
        {"gps_speed_accuracy", "GPS speed accuracy", "m/s",
         "Reported speed uncertainty"},
        {"lap_number", "Lap number", "integer", "Logger lap counter"},
    };
    return definitions;
}

bool metadataUnitsCompatible(const QString& expectedUnit,
                             const QString& sourceUnit) {
    QString expected = expectedUnit.trimmed().toLower();
    QString source = sourceUnit.trimmed().toLower();
    source.remove(QLatin1Char(' '));
    source.replace(QStringLiteral("²"), QStringLiteral("2"));
    if (source.isEmpty()) return false;
    if (expected == QStringLiteral("numeric code"))
        return QSet<QString>{
            QStringLiteral("count"),   QStringLiteral("raw"),
            QStringLiteral("integer"), QStringLiteral("int"),
            QStringLiteral("enum"),    QStringLiteral("float"),
            QStringLiteral("double"),  QStringLiteral("real"),
            QStringLiteral("number"),  QStringLiteral("numeric"),
            QStringLiteral("decimal"), QStringLiteral("unitless")}
            .contains(source);
    if (expected == QStringLiteral("integer"))
        return QSet<QString>{QStringLiteral("count"), QStringLiteral("raw"),
                             QStringLiteral("integer"), QStringLiteral("int"),
                             QStringLiteral("enum")}
            .contains(source);
    if (expected == QStringLiteral("km/h"))
        return QSet<QString>{QStringLiteral("km/h"), QStringLiteral("kph"),
                             QStringLiteral("m/s"), QStringLiteral("mph")}
            .contains(source);
    if (expected == QStringLiteral("0–1"))
        return QSet<QString>{
            QStringLiteral("%"),     QStringLiteral("percent"),
            QStringLiteral("ratio"), QStringLiteral("fraction"),
            QStringLiteral("rad"),   QStringLiteral("deg")}
            .contains(source);
    if (expected == QStringLiteral("bar"))
        return QSet<QString>{QStringLiteral("bar"), QStringLiteral("psi"),
                             QStringLiteral("kpa"), QStringLiteral("pa"),
                             QStringLiteral("mpa")}
            .contains(source);
    if (expected == QStringLiteral("deg"))
        return source == QStringLiteral("deg") ||
               source == QStringLiteral("rad");
    if (expected == QStringLiteral("g"))
        return source == QStringLiteral("g") ||
               source == QStringLiteral("m/s2") ||
               source == QStringLiteral("m/s^2");
    if (expected == QStringLiteral("m"))
        return QSet<QString>{QStringLiteral("m"), QStringLiteral("cm"),
                             QStringLiteral("mm"), QStringLiteral("km"),
                             QStringLiteral("ft")}
            .contains(source);
    if (expected == QStringLiteral("mm"))
        return QSet<QString>{QStringLiteral("mm"), QStringLiteral("cm"),
                             QStringLiteral("m"), QStringLiteral("in")}
            .contains(source);
    if (expected == QStringLiteral("m/s"))
        return QSet<QString>{QStringLiteral("m/s"), QStringLiteral("km/h"),
                             QStringLiteral("kph"), QStringLiteral("mph")}
            .contains(source);
    return expected == source;
}

QString normalizedDriverId(const QVariant& value) {
    bool ok = false;
    const double id = value.toDouble(&ok);
    return ok && std::isfinite(id) && id > 0.0 ? QString::number(id, 'g', 15)
                                               : QString();
}

class TextConsensus {
public:
    void add(const QString& value) {
        const QString clean = value.trimmed();
        if (clean.isEmpty()) return;
        const QString key = clean.toCaseFolded();
        ++counts_[key];
        values_[key] = clean;
        ++observations_;
    }

    QString confidentValue() const {
        QString selected;
        int selectedCount = 0;
        bool tied = false;
        for (auto it = counts_.cbegin(); it != counts_.cend(); ++it) {
            if (it.value() > selectedCount) {
                selected = values_.value(it.key());
                selectedCount = it.value();
                tied = false;
            } else if (it.value() == selectedCount) {
                tied = true;
            }
        }
        if (tied || selectedCount * 3 < observations_ * 2) return {};
        return selected;
    }

private:
    QHash<QString, int> counts_;
    QHash<QString, QString> values_;
    int observations_ = 0;
};

QVariantMap driverNameMappings(const QVariantMap& metadata) {
    QVariantMap result;
    const QVariantMap driver = metadata.value(QStringLiteral("driver")).toMap();
    const QVariantMap mappings =
        driver.value(QStringLiteral("mappings")).toMap();
    for (auto it = mappings.cbegin(); it != mappings.cend(); ++it) {
        const QString id =
            omatrack::track_metadata::normalizedDriverMappingKey(it.key());
        const QString name = it.value().toString().trimmed();
        if (!id.isEmpty() && !name.isEmpty()) result.insert(id, name);
    }
    return result;
}

QString driverNameForId(const QVariantMap& metadata, const QString& driverId) {
    return omatrack::track_metadata::driverNameForId(metadata, driverId);
}

QVariantMap metadataDocument(const QVariantMap& metadata) {
    auto insertText = [](QVariantMap* target, const QString& key,
                         const QVariant& value) {
        const QString text = value.toString().trimmed();
        if (!text.isEmpty()) target->insert(key, text);
    };

    QVariantMap submittedDriverMappings =
        metadata.value(QStringLiteral("driverMappings")).toMap();
    QVariantMap normalizedDriverMappings;
    for (auto it = submittedDriverMappings.cbegin();
         it != submittedDriverMappings.cend(); ++it) {
        const QString id =
            omatrack::track_metadata::normalizedDriverMappingKey(it.key());
        const QString name = it.value().toString().trimmed();
        if (!id.isEmpty() && !name.isEmpty())
            normalizedDriverMappings.insert(id, name);
    }
    QVariantMap driver;
    if (!normalizedDriverMappings.isEmpty())
        driver.insert(QStringLiteral("mappings"), normalizedDriverMappings);
    QVariantMap car;
    insertText(&car, QStringLiteral("number"),
               metadata.value(QStringLiteral("carNumber")));
    insertText(&car, QStringLiteral("class"),
               metadata.value(QStringLiteral("carClass")));
    QVariantMap track;
    insertText(&track, QStringLiteral("name"),
               metadata.value(QStringLiteral("trackName")));
    const QString trackSlug = metadata.value(QStringLiteral("trackSlug"))
                                  .toString()
                                  .trimmed()
                                  .toLower();
    if (!trackSlug.isEmpty()) track.insert(QStringLiteral("slug"), trackSlug);

    QVariantMap channels;
    const QVariantMap submittedChannels =
        metadata.value(QStringLiteral("channels")).toMap();
    for (const MetadataChannelDefinition& definition :
         metadataChannelDefinitions()) {
        const QString field = QString::fromLatin1(definition.key);
        insertText(&channels, field, submittedChannels.value(field));
    }

    QVariantMap document;
    if (!driver.isEmpty()) document.insert(QStringLiteral("driver"), driver);
    if (!car.isEmpty()) document.insert(QStringLiteral("car"), car);
    insertText(&document, QStringLiteral("event"),
               metadata.value(QStringLiteral("event")));
    insertText(&document, QStringLiteral("series"),
               metadata.value(QStringLiteral("series")));
    if (!track.isEmpty()) document.insert(QStringLiteral("track"), track);
    if (!channels.isEmpty())
        document.insert(QStringLiteral("channels"), channels);
    if (!document.isEmpty())
        document.insert(QStringLiteral("schema"), QStringLiteral("2"));
    return document;
}

omatrack::ChannelOverrides channelOverrides(const QVariantMap& metadata) {
    omatrack::ChannelOverrides result;
    const QVariantMap channels =
        metadata.value(QStringLiteral("channels")).toMap();
    for (const MetadataChannelDefinition& definition :
         metadataChannelDefinitions()) {
        const QString key = QString::fromLatin1(definition.key);
        const QString value = channels.value(key).toString().trimmed();
        if (!value.isEmpty())
            result.emplace(key.toStdString(), value.toStdString());
    }
    return result;
}

}  // namespace

using namespace omatrack;

// ── SessionHandle ───────────────────────────────────────────────────

SessionHandle::SessionHandle(const QString& path,
                             const QJsonObject& cachedMetadata)
    : path_(path) {
    // Resolve track + metadata eagerly from filename only (cheap, no parse).
    const QFileInfo info(path);
    const SessionMeta meta =
        sessionMetaFromFilename(info.completeBaseName().toStdString());
    venue_ = QString::fromStdString(meta.venue);
    vehicle_ = QString::fromStdString(meta.vehicleId);
    time_ = QString::fromStdString(meta.time);
    driverId_ = QString::fromStdString(meta.driverTag);
    driver_ = driverId_.isEmpty()
                  ? QStringLiteral("Unknown driver")
                  : QStringLiteral("Driver id %1").arg(driverId_);
    const QString stem = info.completeBaseName();
    const QRegularExpression carPattern(
        QStringLiteral("(?:^|[_ ])Car(\\d+)"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch carMatch = carPattern.match(stem);
    if (carMatch.hasMatch()) {
        carNumber_ = carMatch.captured(1);
    } else {
        const QRegularExpression hashPattern(QStringLiteral("#(\\d+)"));
        const QRegularExpressionMatch hashMatch = hashPattern.match(stem);
        if (hashMatch.hasMatch()) carNumber_ = hashMatch.captured(1);
    }
    const int carMarker = stem.lastIndexOf(
        QRegularExpression(QStringLiteral("\\s*#\\d+(?:\\D|$)"),
                           QRegularExpression::CaseInsensitiveOption));
    if (carMarker > 0) {
        const QString beforeCar = stem.left(carMarker).trimmed();
        const QRegularExpression classPattern(
            QStringLiteral("([A-Z][A-Z0-9-]*(?:[_ ][A-Z][A-Z0-9-]*)?)$"));
        const QRegularExpressionMatch classMatch =
            classPattern.match(beforeCar);
        if (classMatch.hasMatch()) carClass_ = classMatch.captured(1);
    }

    // Until Track Atlas resolves the layout, use the containing folder as the
    // generic track label. Filename-specific venue assumptions belong in user
    // configuration, not the analysis core.
    track_ = venue_.isEmpty() ? info.dir().dirName() : venue_;
    date_ = QString::fromStdString(meta.date);
    if (date_.isEmpty()) {
        const QDate folderDate =
            QDate::fromString(info.dir().dirName(), Qt::ISODate);
        date_ = folderDate.isValid()
                    ? folderDate.toString(QStringLiteral("dd/MM/yyyy"))
                    : QStringLiteral("Unknown");
    }
    applyCachedMetadata(cachedMetadata);
}

SessionHandle::~SessionHandle() = default;

QString SessionHandle::stem() const {
    return QFileInfo(path_).completeBaseName();
}

QString SessionHandle::sessionKey() const { return path_; }

bool SessionHandle::isVideo() const { return isVideoPath(path_); }

void SessionHandle::populateLaps(const std::vector<Lap>& detected) {
    laps_.clear();
    laps_.reserve(int(detected.size()));
    int lapNumber = 0;
    for (size_t i = 0; i < detected.size(); ++i) {
        const Lap& lap = detected[i];
        LapEntry entry;
        entry.lapId = lap.id;
        entry.startTime = lap.startTime;
        entry.endTime = lap.endTime;
        entry.timeMs = lap.timeMs;
        entry.isComplete = lap.complete;
        entry.label = lap.complete ? QStringLiteral("L%1").arg(++lapNumber)
                      : i == 0     ? QStringLiteral("Out")
                      : i + 1 == detected.size() ? QStringLiteral("In")
                                                 : QStringLiteral("Frag");
        entry.timeText = QString::fromStdString(formatLapTime(lap.timeMs));
        laps_.append(entry);
    }
    // Pit in/out laps are crossing-bounded but not representative: their
    // time sits far above the session median. Requires enough timed laps
    // for the median to mean anything.
    QVector<double> timedMs;
    for (const LapEntry& entry : laps_)
        if (entry.isComplete) timedMs.append(entry.timeMs);
    if (timedMs.size() >= 3) {
        std::sort(timedMs.begin(), timedMs.end());
        const double limit = timedMs[timedMs.size() / 2] * 1.35;
        for (LapEntry& entry : laps_)
            if (entry.isComplete && entry.timeMs > limit) entry.isPitLap = true;
    }
    double fastest = std::numeric_limits<double>::max();
    int fastestId = -1;
    for (const LapEntry& entry : laps_) {
        if (entry.countsForBest() && entry.timeMs < fastest) {
            fastest = entry.timeMs;
            fastestId = entry.lapId;
        }
    }
    for (LapEntry& entry : laps_)
        entry.isFastest = entry.lapId == fastestId && fastestId >= 0;
    summaryLoaded_ = true;
}

void SessionHandle::applyEventDriverId(double eventDriverId, bool force) {
    // The lightweight pass reads raw logger values and wins; the loaded source
    // decodes physical samples and only fills the gap.
    const QString normalized = normalizedDriverId(eventDriverId);
    if (normalized.isEmpty() || (driverIdResolved_ && !force)) return;
    driverIdResolved_ = true;
    driverId_ = normalized;
    driver_ = QStringLiteral("Driver id %1").arg(driverId_);
}

void SessionHandle::ensureLapSummary() {
    if (summaryLoaded_) return;
    double eventDriverId = 0.0;
    populateLaps(detectLapsLightweight(path_.toStdString(), &eventDriverId));
    applyEventDriverId(eventDriverId);
}

bool SessionHandle::loadSummaryForIndex() {
    if (summaryLoaded_) return true;
    if (!isVideo()) {
        ensureLapSummary();
        return true;
    }
    std::unique_ptr<TelemetrySource> source =
        TelemetrySource::open(path_.toStdString());
    if (!source) return false;
    captureSourceChannels(*source);
    populateLaps(source->detectLaps());
    applyEventDriverId(source->detectDriverId());
    captureGpsLocation(*source);
    return true;
}

void SessionHandle::captureSourceChannels(const TelemetrySource& source) {
    sourceChannels_.clear();
    automaticChannelMappings_.clear();
    const auto& channels = source.channels();
    sourceChannels_.reserve(int(channels.size()));
    QSet<QString> seenNames;
    for (const RawChannel& channel : channels) {
        if (channel.samples.empty()) continue;
        const QString name = QString::fromStdString(channel.name).trimmed();
        const QString normalizedName = name.toCaseFolded();
        if (name.isEmpty() || seenNames.contains(normalizedName)) continue;
        seenNames.insert(normalizedName);

        SourceChannelSummary summary;
        summary.name = name;
        summary.unit = QString::fromStdString(channel.unit).trimmed();
        summary.frequencyHz = channel.frequencyHz;
        QSet<QString> seenExamples;
        constexpr int kCandidateCount = 9;
        for (int slot = 0; slot < kCandidateCount; ++slot) {
            const size_t index = channel.samples.size() == 1
                                     ? 0
                                     : (channel.samples.size() - 1) *
                                           size_t(slot) /
                                           size_t(kCandidateCount - 1);
            const double sample = channel.samples[index];
            if (!std::isfinite(sample)) continue;
            const QString formatted = QString::number(sample, 'g', 7);
            if (seenExamples.contains(formatted)) continue;
            seenExamples.insert(formatted);
            summary.examples.append(formatted);
            if (summary.examples.size() == 5) break;
        }
        sourceChannels_.append(std::move(summary));
    }

    const auto mappings = source.mapChannels();
    for (const auto& [field, index] : mappings) {
        if (index < 0 || index >= int(channels.size())) continue;
        automaticChannelMappings_.insert(
            QString::fromStdString(field),
            QString::fromStdString(channels[size_t(index)].name));
    }
}

void SessionHandle::captureGpsLocation(const TelemetrySource& source) {
    const auto mapping = source.mapChannels();
    const auto latitudeIt = mapping.find("gps_lat");
    const auto longitudeIt = mapping.find("gps_lon");
    if (latitudeIt == mapping.end() || longitudeIt == mapping.end()) return;

    const int latitudeId = latitudeIt->second;
    const int longitudeId = longitudeIt->second;
    const auto& channels = source.channels();
    if (latitudeId < 0 || longitudeId < 0 ||
        latitudeId >= int(channels.size()) ||
        longitudeId >= int(channels.size()))
        return;
    const RawChannel& latitudeChannel = channels[size_t(latitudeId)];
    const RawChannel& longitudeChannel = channels[size_t(longitudeId)];
    const double duration =
        std::min(latitudeChannel.durationSec, longitudeChannel.durationSec);
    if (!(duration > 0.0)) return;

    const bool latitudeRadians =
        QString::fromStdString(latitudeChannel.unit)
            .trimmed()
            .compare(QStringLiteral("rad"), Qt::CaseInsensitive) == 0;
    const bool longitudeRadians =
        QString::fromStdString(longitudeChannel.unit)
            .trimmed()
            .compare(QStringLiteral("rad"), Qt::CaseInsensitive) == 0;
    constexpr double kDegreesPerRadian = 180.0 / 3.14159265358979323846;
    std::vector<double> latitudes;
    std::vector<double> longitudes;
    for (int sample = 1; sample <= 19; ++sample) {
        const double time = duration * double(sample) / 20.0;
        double latitude = 0.0;
        double longitude = 0.0;
        if (!source.sampleAt(size_t(latitudeId), time, &latitude) ||
            !source.sampleAt(size_t(longitudeId), time, &longitude))
            continue;
        if (latitudeRadians) latitude *= kDegreesPerRadian;
        if (longitudeRadians) longitude *= kDegreesPerRadian;
        if (!std::isfinite(latitude) || !std::isfinite(longitude) ||
            std::fabs(latitude) > 90.0 || std::fabs(longitude) > 180.0 ||
            (std::fabs(latitude) < 0.001 && std::fabs(longitude) < 0.001))
            continue;
        latitudes.push_back(latitude);
        longitudes.push_back(longitude);
    }
    if (latitudes.empty()) return;
    std::sort(latitudes.begin(), latitudes.end());
    std::sort(longitudes.begin(), longitudes.end());
    gpsLatitude_ = latitudes[latitudes.size() / 2];
    gpsLongitude_ = longitudes[longitudes.size() / 2];
}

void SessionHandle::applyCachedMetadata(const QJsonObject& metadata) {
    // Version 10 preserves numeric driver codes without integer coercion and
    // retains source units and representative values for the channel browser.
    if (metadata.value(QStringLiteral("version")).toInt() != 10) return;
    time_ = metadata.value(QStringLiteral("time")).toString(time_);
    driverId_ = metadata.value(QStringLiteral("driverId")).toString(driverId_);
    track_ = metadata.value(QStringLiteral("track")).toString(track_);
    date_ = metadata.value(QStringLiteral("date")).toString(date_);
    carNumber_ =
        metadata.value(QStringLiteral("carNumber")).toString(carNumber_);
    const QString cachedCarClass =
        metadata.value(QStringLiteral("carClass")).toString();
    if (!cachedCarClass.isEmpty()) carClass_ = cachedCarClass;
    driver_ = metadata.value(QStringLiteral("driver")).toString(driver_);
    vehicle_ = metadata.value(QStringLiteral("vehicle")).toString(vehicle_);
    venue_ = metadata.value(QStringLiteral("venue")).toString(venue_);
    gpsLatitude_ = metadata.value(QStringLiteral("gpsLatitude"))
                       .toDouble(std::numeric_limits<double>::quiet_NaN());
    gpsLongitude_ = metadata.value(QStringLiteral("gpsLongitude"))
                        .toDouble(std::numeric_limits<double>::quiet_NaN());
    driverIdResolved_ =
        metadata.value(QStringLiteral("driverIdResolved")).toBool(false);
    for (const QJsonValue& value :
         metadata.value(QStringLiteral("sourceChannels")).toArray()) {
        if (!value.isObject()) continue;
        const QJsonObject object = value.toObject();
        SourceChannelSummary summary;
        summary.name = object.value(QStringLiteral("name")).toString();
        summary.unit = object.value(QStringLiteral("unit")).toString();
        summary.frequencyHz =
            object.value(QStringLiteral("frequencyHz")).toDouble();
        for (const QJsonValue& example :
             object.value(QStringLiteral("examples")).toArray())
            summary.examples.append(example.toString());
        if (!summary.name.isEmpty()) sourceChannels_.append(std::move(summary));
    }
    const QJsonObject automaticMappings =
        metadata.value(QStringLiteral("automaticChannelMappings")).toObject();
    for (auto it = automaticMappings.begin(); it != automaticMappings.end();
         ++it)
        automaticChannelMappings_.insert(it.key(), it.value().toString());

    laps_.clear();
    const QJsonArray laps = metadata.value(QStringLiteral("laps")).toArray();
    laps_.reserve(laps.size());
    for (const QJsonValue& value : laps) {
        if (!value.isObject()) continue;
        const QJsonObject object = value.toObject();
        LapEntry lap;
        lap.lapId = object.value(QStringLiteral("id")).toInt();
        lap.startTime = object.value(QStringLiteral("start")).toDouble();
        lap.endTime = object.value(QStringLiteral("end")).toDouble();
        lap.timeMs = object.value(QStringLiteral("timeMs")).toDouble();
        lap.label = object.value(QStringLiteral("label")).toString();
        lap.timeText = object.value(QStringLiteral("timeText")).toString();
        lap.isFastest = object.value(QStringLiteral("fastest")).toBool();
        lap.isComplete = object.value(QStringLiteral("complete")).toBool(true);
        lap.isPitLap = object.value(QStringLiteral("pit")).toBool();
        laps_.append(lap);
    }
    summaryLoaded_ = true;
}

QJsonObject SessionHandle::metadataForCache() const {
    QJsonArray laps;
    for (const LapEntry& lap : laps_) {
        laps.append(QJsonObject{
            {QStringLiteral("id"), lap.lapId},
            {QStringLiteral("start"), lap.startTime},
            {QStringLiteral("end"), lap.endTime},
            {QStringLiteral("timeMs"), lap.timeMs},
            {QStringLiteral("label"), lap.label},
            {QStringLiteral("timeText"), lap.timeText},
            {QStringLiteral("fastest"), lap.isFastest},
            {QStringLiteral("complete"), lap.isComplete},
            {QStringLiteral("pit"), lap.isPitLap},
        });
    }
    QJsonArray sourceChannels;
    for (const SourceChannelSummary& summary : sourceChannels_) {
        QJsonArray examples;
        for (const QString& example : summary.examples)
            examples.append(example);
        sourceChannels.append(QJsonObject{
            {QStringLiteral("name"), summary.name},
            {QStringLiteral("unit"), summary.unit},
            {QStringLiteral("frequencyHz"), summary.frequencyHz},
            {QStringLiteral("examples"), examples},
        });
    }
    QJsonObject automaticMappings;
    for (auto it = automaticChannelMappings_.cbegin();
         it != automaticChannelMappings_.cend(); ++it)
        automaticMappings.insert(it.key(), it.value());
    return QJsonObject{
        {QStringLiteral("version"), 10},
        {QStringLiteral("time"), time_},
        {QStringLiteral("driverId"), driverId_},
        {QStringLiteral("track"), track_},
        {QStringLiteral("date"), date_},
        {QStringLiteral("carNumber"), carNumber_},
        {QStringLiteral("carClass"), carClass_},
        {QStringLiteral("driver"), driver_},
        {QStringLiteral("vehicle"), vehicle_},
        {QStringLiteral("venue"), venue_},
        {QStringLiteral("gpsLatitude"), gpsLatitude_},
        {QStringLiteral("gpsLongitude"), gpsLongitude_},
        {QStringLiteral("driverIdResolved"), driverIdResolved_},
        {QStringLiteral("sourceChannels"), sourceChannels},
        {QStringLiteral("automaticChannelMappings"), automaticMappings},
        {QStringLiteral("laps"), laps},
    };
}

double SessionHandle::bestLapMs() {
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

QString averageFastestQuartileTime(const SessionHandle* session) {
    if (!session) return {};
    QVector<double> representativeTimes;
    for (const LapEntry& lap : session->laps()) {
        if (lap.countsForBest() && std::isfinite(lap.timeMs) &&
            lap.timeMs > 0.0)
            representativeTimes.append(lap.timeMs);
    }
    if (representativeTimes.isEmpty()) return QStringLiteral("—");
    std::sort(representativeTimes.begin(), representativeTimes.end());
    const qsizetype count =
        std::max<qsizetype>(1, (representativeTimes.size() + 3) / 4);
    double totalMs = 0.0;
    for (qsizetype index = 0; index < count; ++index)
        totalMs += representativeTimes.at(index);
    return QString::fromStdString(formatLapTime(totalMs / double(count)));
}

QString indexedDriveTime(const SessionHandle* session) {
    if (!session) return {};
    double totalMs = 0.0;
    for (const LapEntry& lap : session->laps())
        if (std::isfinite(lap.timeMs) && lap.timeMs > 0.0)
            totalMs += lap.timeMs;
    if (!(totalMs > 0.0)) return QStringLiteral("—");

    const qint64 totalSeconds = qRound64(totalMs / 1000.0);
    const qint64 hours = totalSeconds / 3600;
    const qint64 minutes = (totalSeconds % 3600) / 60;
    const qint64 seconds = totalSeconds % 60;
    if (hours > 0)
        return QStringLiteral("%1:%2:%3")
            .arg(hours)
            .arg(minutes, 2, 10, QLatin1Char('0'))
            .arg(seconds, 2, 10, QLatin1Char('0'));
    return QStringLiteral("%1:%2").arg(minutes).arg(seconds, 2, 10,
                                                    QLatin1Char('0'));
}

int indexedLapCount(const SessionHandle* session) {
    if (!session) return 0;
    return int(
        std::count_if(session->laps().cbegin(), session->laps().cend(),
                      [](const LapEntry& lap) { return lap.isComplete; }));
}

std::shared_ptr<const omatrack::UnifiedLap> SessionHandle::unifiedLap(
    int lapId) const {
    return unifiedCache_.value(lapId);
}

void SessionHandle::adoptLoadedLap(int lapId,
                                   std::unique_ptr<TelemetrySource> source,
                                   std::shared_ptr<const UnifiedLap> unified,
                                   double driverId, bool forceDriverId) {
    if (!source || !unified || unified->size() == 0) return;
    applyEventDriverId(driverId > 0 ? driverId : source->detectDriverId(),
                       forceDriverId);
    if (!hasGpsLocation()) captureGpsLocation(*source);
    captureSourceChannels(*source);
    src_ = std::move(source);
    unifiedCache_.insert(lapId, std::move(unified));
}

struct SessionScanResult {
    std::vector<std::unique_ptr<SessionHandle>> sessions;
    QVariantList fileSources;
    QHash<QString, QVariantMap> fileMetadata;
    QStringList trackMetadataPaths;
    int cacheHits = 0;
    int cacheMisses = 0;
    qint64 elapsedMs = 0;
};

struct FileTreeFolder {
    QString name;
    QString path;
    qint64 modifiedMs = 0;
    std::vector<std::unique_ptr<FileTreeFolder>> folders;
    QVector<QVariantMap> files;
};

FileTreeFolder* childFolder(FileTreeFolder& parent, const QString& name) {
    for (const auto& folder : parent.folders)
        if (folder->name == name) return folder.get();
    auto folder = std::make_unique<FileTreeFolder>();
    folder->name = name;
    folder->path = QDir(parent.path).filePath(name);
    FileTreeFolder* result = folder.get();
    parent.folders.push_back(std::move(folder));
    return result;
}

QVariantMap fileTreeFolderMap(FileTreeFolder& folder) {
    std::sort(folder.folders.begin(), folder.folders.end(),
              [](const auto& left, const auto& right) {
                  if (left->modifiedMs != right->modifiedMs)
                      return left->modifiedMs > right->modifiedMs;
                  return left->name.compare(right->name, Qt::CaseInsensitive) <
                         0;
              });
    std::sort(folder.files.begin(), folder.files.end(),
              [](const QVariantMap& left, const QVariantMap& right) {
                  const qint64 leftTime =
                      left.value(QStringLiteral("modifiedMs")).toLongLong();
                  const qint64 rightTime =
                      right.value(QStringLiteral("modifiedMs")).toLongLong();
                  if (leftTime != rightTime) return leftTime > rightTime;
                  return left.value(QStringLiteral("name"))
                             .toString()
                             .compare(
                                 right.value(QStringLiteral("name")).toString(),
                                 Qt::CaseInsensitive) < 0;
              });

    QVariantList children;
    children.reserve(qsizetype(folder.folders.size()) + folder.files.size());
    for (const auto& child : folder.folders)
        children.append(fileTreeFolderMap(*child));
    for (const QVariantMap& file : std::as_const(folder.files))
        children.append(file);
    return QVariantMap{
        {QStringLiteral("role"), QStringLiteral("folder")},
        {QStringLiteral("name"), folder.name},
        {QStringLiteral("path"), folder.path},
        {QStringLiteral("modifiedMs"), folder.modifiedMs},
        {QStringLiteral("children"), children},
    };
}

QVariantMap buildFileSource(const QString& directory,
                            const QSet<QString>& paths) {
    const QFileInfo sourceInfo(directory);
    const QString sourcePath = sourceInfo.absoluteFilePath();
    FileTreeFolder root;
    root.name =
        sourceInfo.fileName().isEmpty() ? sourcePath : sourceInfo.fileName();
    root.path = sourcePath;
    QDir sourceDirectory(sourcePath);

    for (const QString& path : paths) {
        const QFileInfo fileInfo(path);
        QString relative = sourceDirectory.relativeFilePath(path);
        QStringList parts = relative.split('/', Qt::SkipEmptyParts);
        if (relative.startsWith(QStringLiteral("../")) || parts.isEmpty())
            parts = {fileInfo.fileName()};

        const qint64 modifiedMs = fileInfo.lastModified().toMSecsSinceEpoch();
        root.modifiedMs = std::max(root.modifiedMs, modifiedMs);
        FileTreeFolder* folder = &root;
        for (int index = 0; index + 1 < parts.size(); ++index) {
            folder = childFolder(*folder, parts.at(index));
            folder->modifiedMs = std::max(folder->modifiedMs, modifiedMs);
        }
        folder->files.append(QVariantMap{
            {QStringLiteral("role"), QStringLiteral("file")},
            {QStringLiteral("name"), parts.constLast()},
            {QStringLiteral("path"), path},
            {QStringLiteral("modifiedMs"), modifiedMs},
            {QStringLiteral("modified"),
             fileInfo.lastModified().toString(
                 QStringLiteral("yyyy-MM-dd HH:mm"))},
            {QStringLiteral("children"), QVariantList{}},
        });
    }

    QVariantMap source = fileTreeFolderMap(root);
    source.insert(QStringLiteral("role"), QStringLiteral("source"));
    source.insert(QStringLiteral("available"), sourceInfo.isDir());
    source.insert(QStringLiteral("fileCount"), paths.size());
    return source;
}

struct SessionLapLoadResult {
    QString sessionKey;
    int lapId = -1;
    std::unique_ptr<TelemetrySource> source;
    std::shared_ptr<const UnifiedLap> unified;
    double driverId = 0.0;
    bool forceDriverId = false;
    QString error;
};

QVariantMap configuredRecordingMetadataForPath(
    const QString& path, const QHash<QString, QVariantMap>& saved) {
    const QString canonical = canonicalInputPath(path);
    if (canonical.isEmpty()) return {};
    QVariantMap result = omatrack::track_metadata::readHierarchy(
        QFileInfo(canonical).absolutePath());
    omatrack::track_metadata::merge(&result, saved.value(canonical));
    return result;
}

QVariantMap recordingMetadataForPath(const QString& path,
                                     const QHash<QString, QVariantMap>& saved) {
    return configuredRecordingMetadataForPath(path, saved);
}

QVariantList metadataChannelRows(
    const QVariantMap& metadata, const QVariantMap& inheritedMetadata,
    const QVector<SourceChannelSummary>& sourceChannels,
    const QHash<QString, QString>& automaticMappings,
    const QStringList& trackFiles) {
    QHash<QString, QHash<QString, int>> historicalCounts;
    QHash<QString, QHash<QString, QString>> historicalValues;
    for (const QString& path : trackFiles) {
        const QVariantMap document = YamlConfig::readDocument(path);
        const QVariantMap mappings =
            document.value(QStringLiteral("channels")).toMap();
        for (const MetadataChannelDefinition& definition :
             metadataChannelDefinitions()) {
            const QString field = QString::fromLatin1(definition.key);
            const QString value = mappings.value(field).toString().trimmed();
            if (value.isEmpty()) continue;
            const QString normalized = value.toCaseFolded();
            historicalCounts[field][normalized] += 1;
            historicalValues[field][normalized] = value;
        }
    }

    QHash<QString, SourceChannelSummary> sourceChannelsByName;
    for (const SourceChannelSummary& summary : sourceChannels)
        sourceChannelsByName.insert(summary.name.toCaseFolded(), summary);
    const QVariantMap channels =
        metadata.value(QStringLiteral("channels")).toMap();
    const QVariantMap inheritedChannels =
        inheritedMetadata.value(QStringLiteral("channels")).toMap();

    QVariantList rows;
    for (const MetadataChannelDefinition& definition :
         metadataChannelDefinitions()) {
        const QString field = QString::fromLatin1(definition.key);
        const QString current = channels.value(field).toString().trimmed();
        const QString inherited =
            inheritedChannels.value(field).toString().trimmed();
        const QString effective = current.isEmpty() ? inherited : current;
        const QString automatic = automaticMappings.value(field);

        QHash<QString, QVariantMap> candidates;
        auto addCandidate = [&](const QString& value, int historicalCount,
                                bool isAutomatic) {
            const QString clean = value.trimmed();
            if (clean.isEmpty()) return;
            const QString normalized = clean.toCaseFolded();
            QVariantMap candidate = candidates.value(normalized);
            const auto source = sourceChannelsByName.constFind(normalized);
            const bool available = source != sourceChannelsByName.cend();
            candidate.insert(QStringLiteral("value"), clean);
            candidate.insert(
                QStringLiteral("historicalCount"),
                std::max(
                    candidate.value(QStringLiteral("historicalCount")).toInt(),
                    historicalCount));
            candidate.insert(QStringLiteral("available"), available);
            if (available) {
                const QString sourceUnit =
                    source->unit.isEmpty() &&
                            QString::fromUtf8(definition.unit) ==
                                QStringLiteral("numeric code")
                        ? QStringLiteral("unitless")
                        : source->unit;
                candidate.insert(QStringLiteral("unit"), sourceUnit);
                candidate.insert(
                    QStringLiteral("unitCompatible"),
                    metadataUnitsCompatible(QString::fromUtf8(definition.unit),
                                            sourceUnit));
                candidate.insert(QStringLiteral("examples"), source->examples);
                candidate.insert(QStringLiteral("frequencyHz"),
                                 source->frequencyHz);
                candidate.insert(QStringLiteral("recordingCount"),
                                 source->recordingCount);
            }
            candidate.insert(
                QStringLiteral("automatic"),
                candidate.value(QStringLiteral("automatic")).toBool() ||
                    isAutomatic);
            candidates.insert(normalized, candidate);
        };

        addCandidate(effective,
                     historicalCounts[field].value(effective.toCaseFolded()),
                     effective == automatic && !effective.isEmpty());
        addCandidate(automatic,
                     historicalCounts[field].value(automatic.toCaseFolded()),
                     true);
        for (auto it = historicalCounts[field].cbegin();
             it != historicalCounts[field].cend(); ++it)
            addCandidate(historicalValues[field].value(it.key()), it.value(),
                         false);
        for (const SourceChannelSummary& source : sourceChannels)
            addCandidate(source.name, 0, false);

        QVector<QVariantMap> ordered(candidates.cbegin(), candidates.cend());
        std::sort(
            ordered.begin(), ordered.end(),
            [](const QVariantMap& left, const QVariantMap& right) {
                auto score = [](const QVariantMap& candidate) {
                    return (candidate.value(QStringLiteral("automatic"))
                                    .toBool()
                                ? 100000
                                : 0) +
                           candidate.value(QStringLiteral("historicalCount"))
                                   .toInt() *
                               100 +
                           (candidate.value(QStringLiteral("available"))
                                    .toBool()
                                ? 10
                                : 0) +
                           (candidate.value(QStringLiteral("unitCompatible"))
                                    .toBool()
                                ? 25
                                : 0);
                };
                const int leftScore = score(left);
                const int rightScore = score(right);
                if (leftScore != rightScore) return leftScore > rightScore;
                return left.value(QStringLiteral("value"))
                           .toString()
                           .compare(
                               right.value(QStringLiteral("value")).toString(),
                               Qt::CaseInsensitive) < 0;
            });
        QVariantList suggestions;
        suggestions.reserve(ordered.size());
        for (const QVariantMap& candidate : ordered)
            suggestions.append(candidate);

        rows.append(QVariantMap{
            {QStringLiteral("key"), field},
            {QStringLiteral("label"), QString::fromUtf8(definition.label)},
            {QStringLiteral("expectedUnit"),
             QString::fromUtf8(definition.unit)},
            {QStringLiteral("detail"), QString::fromUtf8(definition.detail)},
            {QStringLiteral("value"), current},
            {QStringLiteral("inheritedValue"), inherited},
            {QStringLiteral("automaticValue"), automatic},
            {QStringLiteral("suggestions"), suggestions},
        });
    }
    return rows;
}

QVariantList metadataDriverRows(const QVariantMap& metadata,
                                const QVariantMap& inheritedMetadata,
                                const QStringList& detectedDriverIds,
                                const QVariantMap& detectedDriverNames,
                                const QStringList& trackFiles) {
    const QVariantMap current = driverNameMappings(metadata);
    const QVariantMap inherited = driverNameMappings(inheritedMetadata);
    QHash<QString, QHash<QString, int>> historicalCounts;
    QHash<QString, QHash<QString, QString>> historicalValues;
    for (const QString& path : trackFiles) {
        const QVariantMap mappings =
            driverNameMappings(YamlConfig::readDocument(path));
        for (auto it = mappings.cbegin(); it != mappings.cend(); ++it) {
            const QString normalizedName =
                it.value().toString().trimmed().toCaseFolded();
            if (normalizedName.isEmpty()) continue;
            historicalCounts[it.key()][normalizedName] += 1;
            historicalValues[it.key()][normalizedName] = it.value().toString();
        }
    }

    QSet<QString> ids;
    for (auto it = current.cbegin(); it != current.cend(); ++it)
        ids.insert(it.key());
    for (auto it = inherited.cbegin(); it != inherited.cend(); ++it)
        ids.insert(it.key());
    if (historicalCounts.contains(QStringLiteral("*")))
        ids.insert(QStringLiteral("*"));
    QSet<QString> detected;
    for (const QString& detectedDriverId : detectedDriverIds) {
        const QString id = normalizedDriverId(detectedDriverId);
        if (id.isEmpty()) continue;
        detected.insert(id);
        ids.insert(id);
    }

    QStringList orderedIds(ids.cbegin(), ids.cend());
    std::sort(orderedIds.begin(), orderedIds.end(),
              [](const QString& left, const QString& right) {
                  if (left == QStringLiteral("*")) return false;
                  if (right == QStringLiteral("*")) return true;
                  bool leftOk = false;
                  bool rightOk = false;
                  const double leftId = left.toDouble(&leftOk);
                  const double rightId = right.toDouble(&rightOk);
                  if (leftOk && rightOk && leftId != rightId)
                      return leftId < rightId;
                  return left < right;
              });

    QVariantList rows;
    for (const QString& id : orderedIds) {
        QHash<QString, QVariantMap> candidates;
        auto addCandidate = [&](const QString& value, int historicalCount) {
            const QString clean = value.trimmed();
            if (clean.isEmpty()) return;
            const QString normalized = clean.toCaseFolded();
            QVariantMap candidate = candidates.value(normalized);
            candidate.insert(QStringLiteral("value"), clean);
            candidate.insert(
                QStringLiteral("historicalCount"),
                std::max(
                    candidate.value(QStringLiteral("historicalCount")).toInt(),
                    historicalCount));
            candidates.insert(normalized, candidate);
        };
        addCandidate(current.value(id).toString(),
                     historicalCounts[id].value(
                         current.value(id).toString().toCaseFolded()));
        addCandidate(inherited.value(id).toString(),
                     historicalCounts[id].value(
                         inherited.value(id).toString().toCaseFolded()));
        addCandidate(detectedDriverNames.value(id).toString(), 0);
        for (auto it = historicalCounts[id].cbegin();
             it != historicalCounts[id].cend(); ++it)
            addCandidate(historicalValues[id].value(it.key()), it.value());

        QVector<QVariantMap> orderedCandidates(candidates.cbegin(),
                                               candidates.cend());
        std::sort(
            orderedCandidates.begin(), orderedCandidates.end(),
            [](const QVariantMap& left, const QVariantMap& right) {
                const int leftCount =
                    left.value(QStringLiteral("historicalCount")).toInt();
                const int rightCount =
                    right.value(QStringLiteral("historicalCount")).toInt();
                if (leftCount != rightCount) return leftCount > rightCount;
                return left.value(QStringLiteral("value"))
                           .toString()
                           .compare(
                               right.value(QStringLiteral("value")).toString(),
                               Qt::CaseInsensitive) < 0;
            });
        QVariantList suggestions;
        for (const QVariantMap& candidate : orderedCandidates)
            suggestions.append(candidate);

        rows.append(QVariantMap{
            {QStringLiteral("id"), id},
            {QStringLiteral("value"), current.value(id).toString()},
            {QStringLiteral("inheritedValue"), inherited.value(id).toString()},
            {QStringLiteral("detected"), detected.contains(id)},
            {QStringLiteral("wildcard"), id == QStringLiteral("*")},
            {QStringLiteral("suggestions"), suggestions},
        });
    }
    return rows;
}

QVariantMap channelRow(const QVariantList& rows, const QString& key) {
    for (const QVariant& value : rows) {
        const QVariantMap row = value.toMap();
        if (row.value(QStringLiteral("key")).toString() == key) return row;
    }
    return {};
}

QVariantList channelRowsWithout(const QVariantList& rows, const QString& key) {
    QVariantList result;
    for (const QVariant& value : rows)
        if (value.toMap().value(QStringLiteral("key")).toString() != key)
            result.append(value);
    return result;
}

struct FileOpenResult {
    QString path;
    std::unique_ptr<SessionHandle> handle;
    std::shared_ptr<SessionLapLoadResult> lap;
    bool standaloneVideo = false;
};

namespace {
struct IndexedSession {
    std::unique_ptr<SessionHandle> handle;
    bool unsupportedVideo = false;
};

IndexedSession indexSession(const QString& path, SessionMetadataCache& cache,
                            int* cacheHits = nullptr,
                            int* cacheMisses = nullptr) {
    const QString fingerprint = SessionMetadataCache::fingerprint(path);
    const SessionMetadataCache::Lookup cached = cache.lookup(fingerprint);
    if (cached.found && !cached.supported) {
        if (cacheHits) ++*cacheHits;
        IndexedSession result;
        result.unsupportedVideo = true;
        return result;
    }
    if (cached.found) {
        auto handle = std::make_unique<SessionHandle>(path, cached.metadata);
        if (handle->hasSummary()) {
            if (cacheHits) ++*cacheHits;
            IndexedSession result;
            result.handle = std::move(handle);
            return result;
        }
    }

    if (cacheMisses) ++*cacheMisses;
    auto handle = std::make_unique<SessionHandle>(path);
    const bool supported = handle->loadSummaryForIndex();
    cache.store(fingerprint, path, supported,
                supported ? handle->metadataForCache() : QJsonObject{});
    IndexedSession result;
    if (!supported) {
        result.unsupportedVideo = handle->isVideo();
        return result;
    }
    result.handle = std::move(handle);
    return result;
}

const LapEntry* bestLap(const SessionHandle& session) {
    const LapEntry* fallback = nullptr;
    for (const LapEntry& lap : session.laps()) {
        if (!fallback && lap.isComplete) fallback = &lap;
        if (lap.isFastest) return &lap;
    }
    return fallback;
}

std::shared_ptr<SessionLapLoadResult> loadSessionLap(
    const QString& path, const QString& sessionKey, const LapEntry& lap,
    const QVariantMap& metadata = {}) {
    auto result = std::make_shared<SessionLapLoadResult>();
    result->sessionKey = sessionKey;
    result->lapId = lap.lapId;
    std::string error;
    result->source = TelemetrySource::open(path.toStdString(), &error);
    if (!result->source) {
        result->error = error.empty()
                            ? QStringLiteral("Unable to open telemetry source")
                            : QString::fromStdString(error);
        return result;
    }
    const ChannelOverrides overrides = channelOverrides(metadata);
    result->driverId = result->source->detectDriverId(overrides);
    result->forceDriverId = overrides.find("driver_id") != overrides.end();
    auto unified = std::make_shared<UnifiedLap>(
        result->source->unifyLap(lap.startTime, lap.endTime, overrides));
    if (unified->size() == 0) {
        result->error = QStringLiteral("Unable to normalize selected lap");
        result->source.reset();
        return result;
    }
    result->unified = std::move(unified);
    return result;
}

std::shared_ptr<SessionScanResult> scanSessionDirectories(
    const QStringList& directories, const QSet<QString>& extraPaths,
    const QString& cachePath) {
    QElapsedTimer timer;
    timer.start();
    auto result = std::make_shared<SessionScanResult>();
    SessionMetadataCache cache(cachePath);
    QSet<QString> uniquePaths = extraPaths;
    QSet<QString> metadataPaths;
    const QStringList filters{
        "*.pds", "*.PDS", "*.ld",  "*.LD",  "*.ldx",  "*.LDX",  "*.vbo",
        "*.VBO", "*.mp4", "*.MP4", "*.mov", "*.MOV",  "*.mkv",  "*.MKV",
        "*.avi", "*.AVI", "*.m4v", "*.M4V", "*.webm", "*.WEBM", "TRACK.yml"};
    for (const QString& directory : directories) {
        QSet<QString> sourcePaths;
        QDirIterator it(directory, filters, QDir::Files,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) {
            it.next();
            if (it.fileName() == QStringLiteral("TRACK.yml")) {
                metadataPaths.insert(
                    QFileInfo(it.filePath()).absoluteFilePath());
                continue;
            }
            const QString path = telemetryPathForInput(it.filePath());
            if (path.isEmpty()) continue;
            const QFileInfo resolved(path);
            const QString canonical = resolved.canonicalFilePath().isEmpty()
                                          ? resolved.absoluteFilePath()
                                          : resolved.canonicalFilePath();
            sourcePaths.insert(canonical);
            uniquePaths.insert(canonical);
        }
        result->fileSources.append(buildFileSource(directory, sourcePaths));
    }
    result->trackMetadataPaths =
        QStringList(metadataPaths.cbegin(), metadataPaths.cend());
    std::sort(result->trackMetadataPaths.begin(),
              result->trackMetadataPaths.end());

    QStringList paths(uniquePaths.cbegin(), uniquePaths.cend());
    std::sort(paths.begin(), paths.end());
    result->sessions.reserve(size_t(paths.size()));
    QHash<QString, QVariantMap> metadataDocuments;
    for (const QString& path : paths) {
        const QString directory = QFileInfo(path).absolutePath();
        if (!metadataDocuments.contains(directory))
            metadataDocuments.insert(
                directory, omatrack::track_metadata::readHierarchy(directory));
        result->fileMetadata.insert(path, metadataDocuments.value(directory));
        IndexedSession indexed =
            indexSession(path, cache, &result->cacheHits, &result->cacheMisses);
        if (indexed.handle)
            result->sessions.push_back(std::move(indexed.handle));
    }
    cache.save();
    result->elapsedMs = timer.elapsed();
    return result;
}

std::shared_ptr<FileOpenResult> openIndexedFile(const QString& path,
                                                const QString& cachePath,
                                                const QVariantMap& metadata) {
    auto result = std::make_shared<FileOpenResult>();
    result->path = path;
    SessionMetadataCache cache(cachePath);
    IndexedSession indexed = indexSession(path, cache);
    cache.save();
    if (!indexed.handle) {
        result->standaloneVideo = indexed.unsupportedVideo;
        return result;
    }
    const LapEntry* lap = bestLap(*indexed.handle);
    if (!lap) {
        result->standaloneVideo = indexed.handle->isVideo();
        return result;
    }
    result->lap =
        loadSessionLap(path, indexed.handle->sessionKey(), *lap, metadata);
    result->handle = std::move(indexed.handle);
    return result;
}
}  // namespace

// ── TelemetryStore ──────────────────────────────────────────────────

TelemetryStore::TelemetryStore(QObject* parent) : QObject(parent) {
    if (s_storeInstance)
        qFatal("Only one TelemetryStore may exist in a process");
    s_storeInstance = this;
    scanWatcher_ = new QFutureWatcher<std::shared_ptr<SessionScanResult>>(this);
    connect(scanWatcher_,
            &QFutureWatcher<std::shared_ptr<SessionScanResult>>::finished, this,
            &TelemetryStore::finishSessionScan);
    loadPreferences();
    loadChannelsConfig();
    // A fresh install gets a real omatrack.yml immediately, so the defaults
    // are visible and hand-editable instead of implicit.
    if (YamlConfig::instance().isFresh()) savePreferences();

    atlasNetwork_ = new QNetworkAccessManager(this);
    connect(atlasNetwork_, &QNetworkAccessManager::sslErrors, this,
            [](QNetworkReply* reply, const QList<QSslError>& errors) {
                for (const QSslError& e : errors)
                    qWarning() << "Track-atlas TLS error:" << e.errorString();
            });
    atlasTimer_ = new QTimer(this);
    atlasTimer_->setInterval(kTrackAtlasCheckIntervalMs);
    connect(atlasTimer_, &QTimer::timeout, this,
            [this]() { updateTrackAtlas(false); });
    atlasTimer_->start();
    loadTrackAtlasCache();
    QTimer::singleShot(0, this, [this]() { updateTrackAtlas(false); });

    scan();
}

TelemetryStore::~TelemetryStore() {
    if (s_storeInstance == this) s_storeInstance.clear();
}

void TelemetryStore::loadPreferences() {
    YamlConfig& config = YamlConfig::instance();
    if (config.isFresh()) {
        // Read the pre-YAML store only during first-run migration. The
        // organization/application names are explicit because the process now
        // identifies as Omatrack and default QSettings would point elsewhere.
        const QString legacyOrganization =
            QCoreApplication::organizationName().endsWith(
                QStringLiteral("-autotest"))
                ? QStringLiteral("racecraft-autotest")
                : QStringLiteral("racecraft");
        QSettings legacy(QSettings::NativeFormat, QSettings::UserScope,
                         legacyOrganization, QStringLiteral("racecraft-qt"));
        legacy.setFallbacksEnabled(false);
        if (!legacy.allKeys().isEmpty()) {
            config.setValue(QStringLiteral("telemetry_dirs"),
                            legacy.value(QStringLiteral("sessionDirs")));
            config.setValue(QStringLiteral("view/channel_height"),
                            legacy.value(QStringLiteral("channelHeight"), 110));
            config.setValue(QStringLiteral("video/muted"),
                            legacy.value(QStringLiteral("videoMuted"), false));
            config.setMap(
                {QStringLiteral("selection")},
                QVariantMap{
                    {QStringLiteral("primary_key"),
                     legacy.value(QStringLiteral("lastPrimaryKey"))},
                    {QStringLiteral("primary_lap"),
                     legacy.value(QStringLiteral("lastPrimaryLap"), -1)},
                    {QStringLiteral("compare_key"),
                     legacy.value(QStringLiteral("lastCompareKey"))},
                    {QStringLiteral("compare_lap"),
                     legacy.value(QStringLiteral("lastCompareLap"), -1)}});

            auto importStringGroup = [&legacy](const QString& group) {
                QVariantMap values;
                legacy.beginGroup(group);
                for (const QString& key : legacy.childKeys())
                    values.insert(key, legacy.value(key));
                legacy.endGroup();
                return values;
            };
            config.setMap({QStringLiteral("driver_aliases")},
                          importStringGroup(QStringLiteral("driverAliases")));
            config.setMap({QStringLiteral("driver_mappings")},
                          importStringGroup(QStringLiteral("driverMappings")));

            QVariantMap channels;
            legacy.beginGroup(QStringLiteral("channels"));
            const bool currentPalette =
                legacy.value(QStringLiteral("_paletteSchema"), 1).toInt() >= 3;
            for (const QString& key : legacy.childGroups()) {
                legacy.beginGroup(key);
                QVariantMap channel;
                if (legacy.contains(QStringLiteral("visible")))
                    channel.insert(QStringLiteral("visible"),
                                   legacy.value(QStringLiteral("visible")));
                if (currentPalette && legacy.contains(QStringLiteral("color")))
                    channel.insert(QStringLiteral("color"),
                                   legacy.value(QStringLiteral("color")));
                if (legacy.contains(QStringLiteral("weight")))
                    channel.insert(QStringLiteral("weight"),
                                   legacy.value(QStringLiteral("weight")));
                legacy.endGroup();
                if (!channel.isEmpty()) channels.insert(key, channel);
            }
            legacy.endGroup();
            config.setMap({QStringLiteral("channels")}, channels);
        }
    }
    const QVariant configuredDirs =
        config.value(QStringLiteral("telemetry_dirs"));
    const QStringList dirs =
        configuredDirs.isValid()
            ? configuredDirs.toStringList()
            : QStringList{QDir::homePath() +
                          QStringLiteral("/Documents/Telemetry")};
    for (const QString& directory : dirs)
        if (!sessionDirs_.contains(directory)) sessionDirs_.append(directory);

    channelHeight_ =
        config.value(QStringLiteral("view/channel_height"), 110).toInt();
    videoMuted_ = yamlBool(config.value(QStringLiteral("video/muted")), false);
    const QVariantMap selection = config.map({QStringLiteral("selection")});
    lastPrimaryKey_ = selection.value(QStringLiteral("primary_key")).toString();
    lastPrimaryLap_ =
        selection.value(QStringLiteral("primary_lap"), -1).toInt();
    lastCompareKey_ = selection.value(QStringLiteral("compare_key")).toString();
    lastCompareLap_ =
        selection.value(QStringLiteral("compare_lap"), -1).toInt();

    const QVariantMap aliases = config.map({QStringLiteral("driver_aliases")});
    for (auto it = aliases.cbegin(); it != aliases.cend(); ++it)
        driverAliases_.insert(it.key(), it.value().toString());

    const QVariantMap mappings =
        config.map({QStringLiteral("driver_mappings")});
    for (auto it = mappings.cbegin(); it != mappings.cend(); ++it)
        driverMappings_.insert(it.key(), it.value().toString());

    const QVariantMap trackAssignments =
        config.map({QStringLiteral("track_assignments")});
    for (auto it = trackAssignments.cbegin(); it != trackAssignments.cend();
         ++it)
        trackAssignments_.insert(it.key(), it.value().toString());

    // Folder metadata lives beside the recordings. Prune the superseded
    // central experiment rather than keeping two competing sources of truth.
    if (config.value({QStringLiteral("folder_metadata")}).isValid()) {
        config.remove({QStringLiteral("folder_metadata")});
        config.save();
    }

    const QVariantMap recordingMetadata =
        config.map({QStringLiteral("recording_metadata")});
    for (auto it = recordingMetadata.cbegin(); it != recordingMetadata.cend();
         ++it)
        if (it.value().typeId() == QMetaType::QVariantMap)
            recordingMetadata_.insert(it.key(), it.value().toMap());
}

void TelemetryStore::savePreferences() {
    YamlConfig& config = YamlConfig::instance();
    config.setValue(QStringList{QStringLiteral("telemetry_dirs")},
                    QVariant(sessionDirs_));
    config.setValue(QStringLiteral("view/channel_height"), channelHeight_);
    config.setValue(QStringLiteral("video/muted"), videoMuted_);
    config.setMap(
        {QStringLiteral("selection")},
        QVariantMap{{QStringLiteral("primary_key"), lastPrimaryKey_},
                    {QStringLiteral("primary_lap"), lastPrimaryLap_},
                    {QStringLiteral("compare_key"), lastCompareKey_},
                    {QStringLiteral("compare_lap"), lastCompareLap_}});

    QVariantMap aliases;
    for (auto it = driverAliases_.cbegin(); it != driverAliases_.cend(); ++it)
        aliases.insert(it.key(), it.value());
    config.setMap({QStringLiteral("driver_aliases")}, aliases);

    QVariantMap mappings;
    for (auto it = driverMappings_.cbegin(); it != driverMappings_.cend(); ++it)
        mappings.insert(it.key(), it.value());
    config.setMap({QStringLiteral("driver_mappings")}, mappings);

    QVariantMap trackAssignments;
    for (auto it = trackAssignments_.cbegin(); it != trackAssignments_.cend();
         ++it)
        trackAssignments.insert(it.key(), it.value());
    config.setMap({QStringLiteral("track_assignments")}, trackAssignments);

    QVariantMap recordingMetadata;
    for (auto it = recordingMetadata_.cbegin(); it != recordingMetadata_.cend();
         ++it)
        recordingMetadata.insert(it.key(), it.value());
    config.setMap({QStringLiteral("recording_metadata")}, recordingMetadata);

    QVariantMap channels = config.map({QStringLiteral("channels")});
    for (const QString& key : channelOrder_)
        channels.insert(
            key,
            QVariantMap{
                {QStringLiteral("visible"),
                 channelVisible_.value(key, key != QStringLiteral("delta"))},
                {QStringLiteral("color"),
                 channelColors_.value(key).name(QColor::HexRgb)},
                {QStringLiteral("weight"), channelWeights_.value(key, 1.0)}});
    config.setMap({QStringLiteral("channels")}, channels);
    config.save();
}

void TelemetryStore::loadChannelsConfig() {
    // The dialog exposes every UnifiedLap channel; extras start hidden so
    // enabling them never changes the default overview.
    static const char* order[] = {"speed",    "throttle", "brake",
                                  "steering", "gear",     "dampers",
                                  "g_long",   "clutch",   "driver_throttle",
                                  "gps_lat",  "gps_lon",  "delta"};
    channelOrder_ =
        QStringList{order, order + sizeof(order) / sizeof(order[0])};
    const QVariantMap channels =
        YamlConfig::instance().map({QStringLiteral("channels")});
    for (const QString& k : channelOrder_) {
        const bool defaultVisible = k != "delta" && k != "clutch" &&
                                    k != "driver_throttle" && k != "gps_lat" &&
                                    k != "gps_lon";
        const QVariantMap entry = channels.value(k).toMap();
        channelVisible_[k] =
            yamlBool(entry.value(QStringLiteral("visible")), defaultVisible);
        const QColor color(
            entry
                .value(QStringLiteral("color"),
                       defaultChannelColor(k).name(QColor::HexRgb))
                .toString());
        channelColors_[k] = color.isValid() ? color : defaultChannelColor(k);
        channelWeights_[k] = qBound(
            0.5, entry.value(QStringLiteral("weight"), 1.0).toDouble(), 2.0);
    }
}

QString TelemetryStore::trackAtlasCachePath() const {
    const QString directory =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
        QStringLiteral("/track-atlas");
    QDir().mkpath(directory);
    const QString current = directory + QStringLiteral("/tracks.jsonl");
    if (!QFile::exists(current)) {
        const QString legacy =
            legacyAppDataPath() + QStringLiteral("/track-atlas/tracks.jsonl");
        if (QFile::exists(legacy) && !QFile::copy(legacy, current))
            qWarning() << "Unable to migrate Track Atlas cache" << legacy;
    }
    return current;
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
    emit sessionsChanged();
    if (primarySession_) {
        loadCornersForPrimary();
        emit cornersChanged();
    }
    return true;
}

void TelemetryStore::loadTrackAtlasCache() {
    const QString currentPath = trackAtlasCachePath();
    QFile cache(currentPath);
    if (!cache.open(QIODevice::ReadOnly)) {
        cache.setFileName(legacyAppDataPath() +
                          QStringLiteral("/track-atlas/tracks.jsonl"));
        if (!cache.open(QIODevice::ReadOnly)) {
            trackAtlasStatus_ = QStringLiteral("No track-atlas cache");
            return;
        }
    }
    if (!parseTrackAtlas(cache.readAll()))
        trackAtlasStatus_ = QStringLiteral("Invalid track-atlas cache");
}

void TelemetryStore::refreshTrackAtlas() { updateTrackAtlas(true); }

void TelemetryStore::updateTrackAtlas(bool force) {
    const QFileInfo cache(trackAtlasCachePath());
    if (!force && cache.exists()) {
        const qint64 ageSeconds =
            cache.lastModified().secsTo(QDateTime::currentDateTimeUtc());
        if (ageSeconds >= 0 && ageSeconds < kTrackAtlasMaxAgeSeconds) return;
    }

    trackAtlasStatus_ = QStringLiteral("Updating track-atlas…");
    emit trackAtlasChanged();
    QNetworkRequest request(kTrackAtlasUrl);
    request.setHeader(
        QNetworkRequest::UserAgentHeader,
        QStringLiteral("Omatrack/") + QCoreApplication::applicationVersion());
    request.setTransferTimeout(15000);
    QNetworkReply* reply = atlasNetwork_->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        const auto cleanup = qScopeGuard([reply]() { reply->deleteLater(); });
        if (reply->error() != QNetworkReply::NoError) {
            trackAtlasStatus_ =
                atlasTracks_.isEmpty()
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
        const bool opened = output.open(QIODevice::WriteOnly);
        const bool written =
            opened && output.write(payload) == qsizetype(payload.size());
        if (!written || !output.commit()) {
            trackAtlasStatus_ =
                QStringLiteral("Track-atlas cache write failed");
            emit trackAtlasChanged();
        }
    });
}

// ── scanning / grouping ─────────────────────────────────────────────

QString TelemetryStore::sessionIndexCachePath() const {
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) +
           QStringLiteral("/session-index.json");
}

void TelemetryStore::scan() {
    closedTracks_.clear();
    if (loading_) {
        rescanPending_ = true;
        return;
    }
    startSessionScan();
}

void TelemetryStore::startSessionScan() {
    if (!loading_) {
        loading_ = true;
        emit loadingChanged();
    }
    const QStringList directories = sessionDirs_;
    const QSet<QString> extraPaths = scanExtraPaths_;
    const QString cachePath = sessionIndexCachePath();
    scanWatcher_->setFuture(
        QtConcurrent::run([directories, extraPaths, cachePath]() {
            return scanSessionDirectories(directories, extraPaths, cachePath);
        }));
}

void TelemetryStore::finishSessionScan() {
    const std::shared_ptr<SessionScanResult> result = scanWatcher_->result();
    if (rescanPending_) {
        rescanPending_ = false;
        startSessionScan();
        return;
    }

    clearSessions();
    sessions_ = std::move(result->sessions);
    fileSources_ = result->fileSources;
    fileMetadata_ = result->fileMetadata;
    trackMetadataPaths_ = result->trackMetadataPaths;
    scanExtraPaths_.clear();
    qInfo() << "Session index:" << sessions_.size() << "sessions,"
            << result->cacheHits << "cache hits," << result->cacheMisses
            << "misses in" << result->elapsedMs << "ms";
    if (!lastPrimaryKey_.isEmpty()) {
        SessionHandle* saved = findSession(lastPrimaryKey_);
        if (saved) {
            for (const LapEntry& lap : saved->laps()) {
                if (lap.lapId == lastPrimaryLap_) {
                    requestLapLoad(saved, lastPrimaryLap_, false);
                    break;
                }
            }
        }
    }
    if (!lastCompareKey_.isEmpty()) {
        SessionHandle* saved = findSession(lastCompareKey_);
        if (saved) {
            for (const LapEntry& lap : saved->laps()) {
                if (lap.lapId == lastCompareLap_) {
                    requestLapLoad(saved, lastCompareLap_, true);
                    break;
                }
            }
        }
    }
    if (!ready_) {
        ready_ = true;
        emit readyChanged();
    }
    loading_ = false;
    emit loadingChanged();
    emit sessionsChanged();
}

void TelemetryStore::addSessionDirectory(const QString& dirPath) {
    if (dirPath.isEmpty() || !QFileInfo::exists(dirPath) ||
        sessionDirs_.contains(dirPath))
        return;
    sessionDirs_.append(dirPath);
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
    const QString resolvedPath = telemetryPathForInput(filePath);
    if (resolvedPath.isEmpty() || !QFileInfo::exists(resolvedPath)) return;
    const QFileInfo resolved(resolvedPath);
    const QString telemetryPath = resolved.canonicalFilePath().isEmpty()
                                      ? resolved.absoluteFilePath()
                                      : resolved.canonicalFilePath();
    if (loading_) {
        scanExtraPaths_.insert(telemetryPath);
        rescanPending_ = true;
    }
    pendingFileOpens_.append(telemetryPath);
    startNextFileOpen();
}

void TelemetryStore::startNextFileOpen() {
    if (fileOpenLoading_ && pendingFileOpens_.isEmpty()) {
        fileOpenLoading_ = false;
        emit lapLoadingChanged();
        return;
    }
    if (fileOpenLoading_ || pendingFileOpens_.isEmpty()) return;
    fileOpenLoading_ = true;
    emit lapLoadingChanged();

    const QString path = pendingFileOpens_.takeFirst();
    const QString cachePath = sessionIndexCachePath();
    const QVariantMap metadata =
        recordingMetadataForPath(path, recordingMetadata_);
    auto* watcher = new QFutureWatcher<std::shared_ptr<FileOpenResult>>(this);
    connect(
        watcher, &QFutureWatcher<std::shared_ptr<FileOpenResult>>::finished,
        this, [this, watcher]() {
            std::shared_ptr<FileOpenResult> result = watcher->result();
            watcher->deleteLater();
            if (result->standaloneVideo) {
                // An ordinary video has no analytical relationship to the
                // currently selected laps. Clear both roles before handing
                // playback to QML so stale traces are never presented as if
                // they belonged to this recording.
                clearPrimary();
                emit standaloneVideoRequested(
                    QUrl::fromLocalFile(result->path));
            } else if (result->handle && result->lap &&
                       result->lap->error.isEmpty() && result->lap->source &&
                       result->lap->unified) {
                SessionHandle* session = findSession(result->path);
                bool added = false;
                if (!session) {
                    session = result->handle.get();
                    sessions_.push_back(std::move(result->handle));
                    added = true;
                }
                session->adoptLoadedLap(
                    result->lap->lapId, std::move(result->lap->source),
                    std::move(result->lap->unified), result->lap->driverId,
                    result->lap->forceDriverId);
                if (added) emit sessionsChanged();
                // Opening a second telemetry-bearing video means
                // "compare these": preserve the first as primary.
                if (session->isVideo() && primarySession_ &&
                    primarySession_->isVideo() && primarySession_ != session) {
                    setCompare(session, result->lap->lapId);
                } else {
                    setPrimary(session, result->lap->lapId);
                    viewStart_ = 0.0;
                    viewEnd_ = 1.0;
                }
                if (!ready_) {
                    ready_ = true;
                    emit readyChanged();
                }
            } else {
                qWarning() << "Unable to open telemetry file" << result->path
                           << (result->lap ? result->lap->error : QString());
            }
            fileOpenLoading_ = false;
            if (pendingFileOpens_.isEmpty())
                emit lapLoadingChanged();
            else
                startNextFileOpen();
        });
    watcher->setFuture(QtConcurrent::run([path, cachePath, metadata]() {
        return openIndexedFile(path, cachePath, metadata);
    }));
}

void TelemetryStore::clearSessions() {
    sessions_.clear();
    fileSources_.clear();
    fileMetadata_.clear();
    primarySession_ = nullptr;
    compareSession_ = nullptr;
    primaryLap_ = -1;
    compareLap_ = -1;
    ++primaryLoadGeneration_;
    ++compareLoadGeneration_;
    setPrimaryLapLoading(false);
    setCompareLapLoading(false);
    deltaCacheValid_ = false;
    damperAlignmentValid_ = false;
    setReferenceAlignment(0.0);
    corners_.clear();
    emit selectionChanged();
    emit videoTimeChanged();
    emit cornersChanged();
}

QStringList TelemetryStore::sessionDirectories() const { return sessionDirs_; }

QVariantList TelemetryStore::fileSources() const {
    auto enrichNode = [this](auto&& self,
                             const QVariantMap& sourceNode) -> QVariantMap {
        QVariantMap node = sourceNode;
        const QString role = node.value(QStringLiteral("role")).toString();
        const QString path = node.value(QStringLiteral("path")).toString();
        if (role == QStringLiteral("file")) {
            SessionHandle* session = findSession(path);
            const bool video = isVideoPath(path);
            QVariantMap metadata = fileMetadata_.value(path);
            omatrack::track_metadata::merge(&metadata,
                                            recordingMetadata_.value(path));
            auto metadataOr = [&metadata](const QStringList& field,
                                          const QString& fallback) {
                const QString value = nestedText(metadata, field);
                return value.isEmpty() ? fallback : value;
            };
            QString date = session ? session->date() : QString();
            if (date == QStringLiteral("Unknown")) date.clear();
            node.insert(QStringLiteral("key"),
                        session ? session->sessionKey() : QString());
            node.insert(QStringLiteral("hasSession"), session != nullptr);
            node.insert(QStringLiteral("isVideo"), video);
            node.insert(QStringLiteral("driver"),
                        session ? driverDisplay(session) : QString());
            node.insert(QStringLiteral("mappingKey"),
                        session ? session->driverMappingKey() : QString());
            node.insert(QStringLiteral("bestTime"),
                        session ? session->bestLapTime() : QString());
            node.insert(QStringLiteral("topQuartileTime"),
                        averageFastestQuartileTime(session));
            node.insert(QStringLiteral("driveTime"), indexedDriveTime(session));
            node.insert(QStringLiteral("lapCount"), indexedLapCount(session));
            node.insert(
                QStringLiteral("carClass"),
                metadataOr({QStringLiteral("car"), QStringLiteral("class")},
                           session ? session->carClass() : QString()));
            node.insert(QStringLiteral("seriesName"),
                        nestedText(metadata, {QStringLiteral("series")}));
            node.insert(QStringLiteral("sessionDate"), date);
        }

        const QVariantList sourceChildren =
            node.value(QStringLiteral("children")).toList();
        QVariantList children;
        children.reserve(sourceChildren.size());
        for (const QVariant& child : sourceChildren)
            children.append(self(self, child.toMap()));
        node.insert(QStringLiteral("children"), children);
        return node;
    };

    QVariantList sources;
    sources.reserve(fileSources_.size());
    for (const QVariant& source : fileSources_)
        sources.append(enrichNode(enrichNode, source.toMap()));
    return sources;
}

bool TelemetryStore::directoryExists(const QString& dirPath) const {
    return !dirPath.isEmpty() && QFileInfo(dirPath).isDir();
}

// True when the active lap carries usable GPS: the damper-alignment tool is
// only the fallback for sessions that cannot be aligned positionally.
bool TelemetryStore::hasGpsData() const {
    const omatrack::UnifiedLap* lap = primaryUnified();
    if (!lap || lap->gpsLat.size() < 2 ||
        lap->gpsLat.size() != lap->gpsLon.size())
        return false;
    double minLat = lap->gpsLat.front();
    double maxLat = minLat;
    double minLon = lap->gpsLon.front();
    double maxLon = minLon;
    for (size_t i = 1; i < lap->gpsLat.size(); ++i) {
        minLat = std::min(minLat, lap->gpsLat[i]);
        maxLat = std::max(maxLat, lap->gpsLat[i]);
        minLon = std::min(minLon, lap->gpsLon[i]);
        maxLon = std::max(maxLon, lap->gpsLon[i]);
    }
    // A parked or absent receiver reports a constant (often zero) fix.
    return (maxLat - minLat) > 1e-5 || (maxLon - minLon) > 1e-5;
}
QString TelemetryStore::configFilePath() const {
    return YamlConfig::filePath();
}

SessionHandle* TelemetryStore::findSession(const QString& key) const {
    for (const auto& s : sessions_)
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
    const QVariantMap metadata =
        recordingMetadataForPath(session->path(), recordingMetadata_);
    const QString recordingName =
        driverNameForId(metadata, session->driverId());
    if (!recordingName.isEmpty()) return recordingName;
    const QString mapped =
        driverMappings_.value(session->driverMappingKey()).trimmed();
    return mapped.isEmpty() ? session->driver() : mapped;
}

QString TelemetryStore::trackAssignmentKey(const SessionHandle* session) {
    if (!session) return QString();
    const QDate date = sessionDate(session);
    if (date.isValid()) return date.toString(Qt::ISODate);
    return QFileInfo(session->path()).absolutePath();
}

QString TelemetryStore::assignedTrackSlug(const SessionHandle* session) const {
    if (!session) return QString();
    const QVariantMap metadata =
        configuredRecordingMetadataForPath(session->path(), recordingMetadata_);
    const QString recordingSlug =
        nestedText(metadata, {QStringLiteral("track"), QStringLiteral("slug")})
            .toLower();
    if (!recordingSlug.isEmpty()) return recordingSlug;
    QString slug = trackAssignments_.value(trackAssignmentKey(session));
    // Compatibility with the first track-assignment build, which scoped the
    // mapping to a source folder rather than an event date.
    if (slug.isEmpty())
        slug =
            trackAssignments_.value(QFileInfo(session->path()).absolutePath());
    return slug.trimmed().toLower();
}

QString TelemetryStore::detectedAtlasSlug(const SessionHandle* session) const {
    if (!session || atlasTracks_.isEmpty()) return {};

    auto nearestGpsTrack = [&](const SessionHandle* candidate) {
        if (!candidate || !candidate->hasGpsLocation()) return QString();
        QString nearestSlug;
        double nearestDistance = std::numeric_limits<double>::max();
        for (auto it = atlasTracks_.cbegin(); it != atlasTracks_.cend(); ++it) {
            const QJsonObject location =
                it.value().value(QStringLiteral("location")).toObject();
            const double latitude =
                location.value(QStringLiteral("lat"))
                    .toDouble(std::numeric_limits<double>::quiet_NaN());
            const double longitude =
                location.value(QStringLiteral("lon"))
                    .toDouble(std::numeric_limits<double>::quiet_NaN());
            if (!std::isfinite(latitude) || !std::isfinite(longitude)) continue;
            const double distance =
                geoDistanceKm(candidate->gpsLatitude(),
                              candidate->gpsLongitude(), latitude, longitude);
            if (distance < nearestDistance) {
                nearestDistance = distance;
                nearestSlug = it.key();
            }
        }
        // Track Atlas locations are circuit centres. A generous radius allows
        // for coarse upstream coordinates without confusing nearby venues.
        return nearestDistance <= 20.0 ? nearestSlug : QString();
    };

    const QString ownGpsMatch = nearestGpsTrack(session);
    if (!ownGpsMatch.isEmpty()) return ownGpsMatch;

    const QString wanted = normalizeAtlasName(session->track());
    if (!wanted.isEmpty()) {
        for (auto it = atlasTracks_.cbegin(); it != atlasTracks_.cend(); ++it) {
            QStringList names{
                it.key(), it.value().value(QStringLiteral("name")).toString()};
            for (const QJsonValue& alias :
                 it.value().value(QStringLiteral("aka")).toArray())
                names.append(alias.toString());
            for (const QString& name : names)
                if (normalizeAtlasName(name) == wanted) return it.key();
        }
    }

    QStringList sourceNames{session->track(),
                            QFileInfo(session->path()).completeBaseName()};
    QDir sourceDirectory(QFileInfo(session->path()).absolutePath());
    for (int level = 0; level < 3; ++level) {
        sourceNames.append(sourceDirectory.dirName());
        if (!sourceDirectory.cdUp()) break;
    }
    QSet<QString> filenameTokens;
    const QRegularExpression tokenSeparator(QStringLiteral("[^\\p{L}\\p{N}]+"));
    for (const QString& sourceName : sourceNames) {
        const QString whole = normalizeAtlasName(sourceName);
        if (whole.size() >= 3) filenameTokens.insert(whole);
        for (const QString& token :
             sourceName.split(tokenSeparator, Qt::SkipEmptyParts)) {
            const QString normalized = normalizeAtlasName(token);
            if (normalized.size() < 3 ||
                std::all_of(
                    normalized.cbegin(), normalized.cend(),
                    [](QChar character) { return character.isDigit(); }))
                continue;
            filenameTokens.insert(normalized);
        }
    }
    QSet<QString> filenameMatches;
    for (auto it = atlasTracks_.cbegin(); it != atlasTracks_.cend(); ++it) {
        QStringList names{it.key(),
                          it.value().value(QStringLiteral("name")).toString()};
        for (const QJsonValue& alias :
             it.value().value(QStringLiteral("aka")).toArray())
            names.append(alias.toString());
        const QJsonObject externalIds =
            it.value().value(QStringLiteral("external_ids")).toObject();
        for (auto external = externalIds.begin(); external != externalIds.end();
             ++external)
            names.append(external.value().toString());
        for (const QString& name : names) {
            if (!filenameTokens.contains(normalizeAtlasName(name))) continue;
            filenameMatches.insert(it.key());
            break;
        }
    }
    if (filenameMatches.size() == 1) return *filenameMatches.cbegin();

    // Some AiM recordings expose a GPS channel whose receiver payload is an
    // invalid equator placeholder. Borrow the unambiguous venue identity from
    // GPS-valid recordings on consecutive days in the same event folder.
    const QDate selectedDate = sessionDate(session);
    if (!selectedDate.isValid()) return {};
    const QString selectedDirectory = QFileInfo(session->path()).absolutePath();
    QDir selectedParentDirectory(selectedDirectory);
    selectedParentDirectory.cdUp();
    const QString selectedParent = selectedParentDirectory.absolutePath();
    QSet<QDate> nearbyDates;
    for (const auto& candidate : sessions_) {
        const QString candidateDirectory =
            QFileInfo(candidate->path()).absolutePath();
        QDir candidateParentDirectory(candidateDirectory);
        candidateParentDirectory.cdUp();
        if (candidateDirectory != selectedDirectory &&
            candidateParentDirectory.absolutePath() != selectedParent)
            continue;
        const QDate candidateDate = sessionDate(candidate.get());
        if (candidateDate.isValid()) nearbyDates.insert(candidateDate);
    }
    QSet<QDate> eventDates{selectedDate};
    bool expanded = true;
    while (expanded) {
        expanded = false;
        for (const QDate& candidateDate : nearbyDates) {
            if (eventDates.contains(candidateDate)) continue;
            for (const QDate& eventDate : eventDates) {
                if (std::abs(eventDate.daysTo(candidateDate)) != 1) continue;
                eventDates.insert(candidateDate);
                expanded = true;
                break;
            }
            if (expanded) break;
        }
    }
    QHash<QString, int> votes;
    for (const auto& candidate : sessions_) {
        if (!eventDates.contains(sessionDate(candidate.get()))) continue;
        QDir candidateParentDirectory(
            QFileInfo(candidate->path()).absolutePath());
        candidateParentDirectory.cdUp();
        if (candidateParentDirectory.absolutePath() != selectedParent) continue;
        const QString slug = nearestGpsTrack(candidate.get());
        if (!slug.isEmpty()) ++votes[slug];
    }
    QString votedSlug;
    int highestVotes = 0;
    bool tied = false;
    for (auto it = votes.cbegin(); it != votes.cend(); ++it) {
        if (it.value() > highestVotes) {
            votedSlug = it.key();
            highestVotes = it.value();
            tied = false;
        } else if (it.value() == highestVotes) {
            tied = true;
        }
    }
    if (!tied) return votedSlug;
    return {};
}

QString TelemetryStore::resolvedTrackSlug(const SessionHandle* session) const {
    const QString assigned = assignedTrackSlug(session);
    return assigned.isEmpty() ? detectedAtlasSlug(session) : assigned;
}

QString TelemetryStore::displayTrack(const SessionHandle* session) const {
    if (!session) return QString();
    const QVariantMap metadata =
        configuredRecordingMetadataForPath(session->path(), recordingMetadata_);
    const QString recordingName =
        nestedText(metadata, {QStringLiteral("track"), QStringLiteral("name")});
    if (!recordingName.isEmpty()) return recordingName;
    const QString slug = resolvedTrackSlug(session);
    if (!slug.isEmpty()) {
        const QJsonObject track = atlasTracks_.value(slug);
        const QString name = track.value(QStringLiteral("name")).toString();
        return name.isEmpty() ? slug : name;
    }
    return session->track();
}

QVariantList TelemetryStore::trackAtlasChoices() const {
    QVector<QVariantMap> rows;
    rows.reserve(atlasTracks_.size());
    for (auto it = atlasTracks_.cbegin(); it != atlasTracks_.cend(); ++it) {
        const QString name =
            it.value().value(QStringLiteral("name")).toString(it.key());
        rows.append(QVariantMap{{QStringLiteral("name"), name},
                                {QStringLiteral("slug"), it.key()}});
    }
    std::sort(rows.begin(), rows.end(),
              [](const QVariantMap& a, const QVariantMap& b) {
                  return a.value(QStringLiteral("name")).toString() <
                         b.value(QStringLiteral("name")).toString();
              });
    QVariantList result;
    result.reserve(rows.size());
    for (const QVariantMap& row : rows) result.append(row);
    return result;
}

QString TelemetryStore::detectedTrackForSession(
    const QString& sessionKey) const {
    const SessionHandle* session = findSession(sessionKey);
    if (!session) return {};
    const QString slug = detectedAtlasSlug(session);
    if (slug.isEmpty()) return session->track();
    return atlasTracks_.value(slug)
        .value(QStringLiteral("name"))
        .toString(slug);
}

QString TelemetryStore::assignedTrackForSession(
    const QString& sessionKey) const {
    return assignedTrackSlug(findSession(sessionKey));
}

QVariantMap TelemetryStore::folderMetadata(const QString& folderPath) const {
    const QString canonical = canonicalDirectoryPath(folderPath);
    if (canonical.isEmpty()) return {};

    const QString targetSidecarPath =
        omatrack::track_metadata::filePath(canonical);
    const QVariantMap metadata =
        QFileInfo::exists(targetSidecarPath)
            ? YamlConfig::readDocument(targetSidecarPath)
            : QVariantMap();
    QStringList inheritedPaths;
    const QVariantMap inheritedMetadata =
        omatrack::track_metadata::readHierarchy(canonical, false,
                                                &inheritedPaths);
    const QStringList trackFiles =
        trackMetadataPaths(trackMetadataPaths_, canonical);

    QHash<QString, SourceChannelSummary> aggregatedChannels;
    QHash<QString, QHash<QString, int>> automaticMappingCounts;
    QHash<QString, QHash<QString, QString>> automaticMappingValues;
    TextConsensus carNumberConsensus;
    TextConsensus carClassConsensus;
    TextConsensus eventConsensus;
    TextConsensus seriesConsensus;
    TextConsensus trackNameConsensus;
    TextConsensus trackSlugConsensus;
    QHash<QString, TextConsensus> driverNameConsensus;
    QSet<QString> detectedDriverIds;
    int metadataSourceCount = 0;
    int sourceRecordingCount = 0;
    for (const auto& session : sessions_) {
        if (!session) continue;
        const QString relative =
            QDir(canonical).relativeFilePath(session->path());
        if (relative == QStringLiteral("..") ||
            relative.startsWith(QStringLiteral("../")))
            continue;

        ++metadataSourceCount;
        QVariantMap effectiveMetadata = fileMetadata_.value(session->path());
        omatrack::track_metadata::merge(
            &effectiveMetadata, recordingMetadata_.value(session->path()));
        const auto metadataOr = [&](const QStringList& path,
                                    const QString& fallback) {
            const QString value = nestedText(effectiveMetadata, path);
            return value.isEmpty() ? fallback : value;
        };
        carNumberConsensus.add(
            metadataOr({QStringLiteral("car"), QStringLiteral("number")},
                       session->carNumber()));
        carClassConsensus.add(
            metadataOr({QStringLiteral("car"), QStringLiteral("class")},
                       session->carClass()));
        eventConsensus.add(
            nestedText(effectiveMetadata, {QStringLiteral("event")}));
        seriesConsensus.add(
            nestedText(effectiveMetadata, {QStringLiteral("series")}));

        QString trackSlug =
            nestedText(effectiveMetadata,
                       {QStringLiteral("track"), QStringLiteral("slug")})
                .toLower();
        if (trackSlug.isEmpty())
            trackSlug =
                trackAssignments_.value(trackAssignmentKey(session.get()))
                    .trimmed()
                    .toLower();
        if (trackSlug.isEmpty()) trackSlug = detectedAtlasSlug(session.get());
        trackSlugConsensus.add(trackSlug);

        QString trackName =
            nestedText(effectiveMetadata,
                       {QStringLiteral("track"), QStringLiteral("name")});
        if (trackName.isEmpty() && !trackSlug.isEmpty())
            trackName = atlasTracks_.value(trackSlug)
                            .value(QStringLiteral("name"))
                            .toString(trackSlug);
        if (trackName.isEmpty()) trackName = session->venue();
        trackNameConsensus.add(trackName);

        const QString driverId = normalizedDriverId(session->driverId());
        if (!driverId.isEmpty()) {
            detectedDriverIds.insert(driverId);
            QString driverName = driverNameForId(effectiveMetadata, driverId);
            if (driverName.isEmpty())
                driverName = driverMappings_.value(session->driverMappingKey())
                                 .trimmed();
            driverNameConsensus[driverId].add(driverName);
        }

        if (!session->isVideo() || session->sourceChannels().isEmpty())
            continue;
        ++sourceRecordingCount;
        for (auto it = session->automaticChannelMappings().cbegin();
             it != session->automaticChannelMappings().cend(); ++it) {
            const QString normalized = it.value().toCaseFolded();
            ++automaticMappingCounts[it.key()][normalized];
            automaticMappingValues[it.key()][normalized] = it.value();
        }
        for (const SourceChannelSummary& source : session->sourceChannels()) {
            const QString key = source.name.toCaseFolded();
            auto existing = aggregatedChannels.find(key);
            if (existing == aggregatedChannels.end()) {
                aggregatedChannels.insert(key, source);
                continue;
            }
            ++existing->recordingCount;
            if (existing->unit.isEmpty()) existing->unit = source.unit;
            if (!(existing->frequencyHz > 0.0))
                existing->frequencyHz = source.frequencyHz;
            for (const QString& example : source.examples) {
                if (!existing->examples.contains(example))
                    existing->examples.append(example);
                if (existing->examples.size() == 5) break;
            }
        }
    }
    QVector<SourceChannelSummary> sourceChannels;
    sourceChannels.reserve(aggregatedChannels.size());
    for (auto it = aggregatedChannels.cbegin(); it != aggregatedChannels.cend();
         ++it)
        sourceChannels.append(it.value());
    QHash<QString, QString> automaticMappings;
    for (auto field = automaticMappingCounts.cbegin();
         field != automaticMappingCounts.cend(); ++field) {
        QString selected;
        int selectedCount = 0;
        for (auto candidate = field.value().cbegin();
             candidate != field.value().cend(); ++candidate) {
            const QString value =
                automaticMappingValues[field.key()].value(candidate.key());
            if (candidate.value() > selectedCount ||
                (candidate.value() == selectedCount &&
                 value.compare(selected, Qt::CaseInsensitive) < 0)) {
                selected = value;
                selectedCount = candidate.value();
            }
        }
        if (!selected.isEmpty())
            automaticMappings.insert(field.key(), selected);
    }
    const QVariantList allChannelRows =
        metadataChannelRows(metadata, inheritedMetadata, sourceChannels,
                            automaticMappings, trackFiles);
    QStringList detectedIds(detectedDriverIds.cbegin(),
                            detectedDriverIds.cend());
    QVariantMap detectedDriverNames;
    for (auto it = driverNameConsensus.cbegin();
         it != driverNameConsensus.cend(); ++it) {
        const QString name = it.value().confidentValue();
        if (!name.isEmpty()) detectedDriverNames.insert(it.key(), name);
    }
    const QVariantList driverRows =
        metadataDriverRows(metadata, inheritedMetadata, detectedIds,
                           detectedDriverNames, trackFiles);
    const QString folderName = QFileInfo(canonical).fileName().isEmpty()
                                   ? canonical
                                   : QFileInfo(canonical).fileName();

    return QVariantMap{
        {QStringLiteral("path"), canonical},
        {QStringLiteral("folderScope"), true},
        {QStringLiteral("fileName"), folderName},
        {QStringLiteral("folder"), canonical},
        {QStringLiteral("sidecarPath"), targetSidecarPath},
        {QStringLiteral("inheritedSidecarPath"),
         inheritedPaths.isEmpty() ? QString() : inheritedPaths.constLast()},
        {QStringLiteral("trackFileCount"), trackFiles.size()},
        {QStringLiteral("sourceChannelCount"), sourceChannels.size()},
        {QStringLiteral("sourceRecordingCount"), sourceRecordingCount},
        {QStringLiteral("metadataSourceCount"), metadataSourceCount},
        {QStringLiteral("suggestedCarNumber"),
         carNumberConsensus.confidentValue()},
        {QStringLiteral("suggestedCarClass"),
         carClassConsensus.confidentValue()},
        {QStringLiteral("suggestedEvent"), eventConsensus.confidentValue()},
        {QStringLiteral("suggestedSeries"), seriesConsensus.confidentValue()},
        {QStringLiteral("suggestedTrackName"),
         trackNameConsensus.confidentValue()},
        {QStringLiteral("suggestedTrackSlug"),
         trackSlugConsensus.confidentValue()},
        {QStringLiteral("driverChannel"),
         channelRow(allChannelRows, QStringLiteral("driver_id"))},
        {QStringLiteral("driverMappings"), driverRows},
        {QStringLiteral("carNumber"),
         nestedText(metadata,
                    {QStringLiteral("car"), QStringLiteral("number")})},
        {QStringLiteral("inheritedCarNumber"),
         nestedText(inheritedMetadata,
                    {QStringLiteral("car"), QStringLiteral("number")})},
        {QStringLiteral("carClass"),
         nestedText(metadata,
                    {QStringLiteral("car"), QStringLiteral("class")})},
        {QStringLiteral("inheritedCarClass"),
         nestedText(inheritedMetadata,
                    {QStringLiteral("car"), QStringLiteral("class")})},
        {QStringLiteral("event"),
         nestedText(metadata, {QStringLiteral("event")})},
        {QStringLiteral("inheritedEvent"),
         nestedText(inheritedMetadata, {QStringLiteral("event")})},
        {QStringLiteral("series"),
         nestedText(metadata, {QStringLiteral("series")})},
        {QStringLiteral("inheritedSeries"),
         nestedText(inheritedMetadata, {QStringLiteral("series")})},
        {QStringLiteral("trackName"),
         nestedText(metadata,
                    {QStringLiteral("track"), QStringLiteral("name")})},
        {QStringLiteral("inheritedTrackName"),
         nestedText(inheritedMetadata,
                    {QStringLiteral("track"), QStringLiteral("name")})},
        {QStringLiteral("trackSlug"),
         nestedText(metadata,
                    {QStringLiteral("track"), QStringLiteral("slug")})},
        {QStringLiteral("inheritedTrackSlug"),
         nestedText(inheritedMetadata,
                    {QStringLiteral("track"), QStringLiteral("slug")})},
        {QStringLiteral("channels"),
         channelRowsWithout(allChannelRows, QStringLiteral("driver_id"))},
    };
}

QVariantMap TelemetryStore::videoMetadata(const QString& videoPath) const {
    const QString canonical = canonicalInputPath(videoPath);
    if (canonical.isEmpty() || !isVideoPath(canonical)) return {};

    const SessionHandle* session = findSession(canonical);
    const QVariantMap metadata = recordingMetadata_.value(canonical);
    QStringList inheritedPaths;
    const QString directory = QFileInfo(canonical).absolutePath();
    const QVariantMap inheritedMetadata =
        omatrack::track_metadata::readHierarchy(directory, true,
                                                &inheritedPaths);

    const QStringList trackFiles =
        trackMetadataPaths(trackMetadataPaths_, directory);
    const QVector<SourceChannelSummary> sourceChannels =
        session ? session->sourceChannels() : QVector<SourceChannelSummary>();
    const QHash<QString, QString> automaticMappings =
        session ? session->automaticChannelMappings()
                : QHash<QString, QString>();
    const QVariantList allChannelRows =
        metadataChannelRows(metadata, inheritedMetadata, sourceChannels,
                            automaticMappings, trackFiles);
    QStringList detectedDriverIds;
    QVariantMap detectedDriverNames;
    if (session) {
        const QString driverId = normalizedDriverId(session->driverId());
        if (!driverId.isEmpty()) {
            detectedDriverIds.append(driverId);
            const QString driverName = driverDisplay(session);
            if (!driverName.isEmpty() &&
                !driverName.startsWith(QStringLiteral("Driver id ")) &&
                driverName != QStringLiteral("Unknown driver"))
                detectedDriverNames.insert(driverId, driverName);
        }
    }
    const QVariantList driverRows =
        metadataDriverRows(metadata, inheritedMetadata, detectedDriverIds,
                           detectedDriverNames, trackFiles);

    return QVariantMap{
        {QStringLiteral("path"), canonical},
        {QStringLiteral("folderScope"), false},
        {QStringLiteral("fileName"), QFileInfo(canonical).fileName()},
        {QStringLiteral("folder"), QFileInfo(canonical).absolutePath()},
        {QStringLiteral("sidecarPath"),
         inheritedPaths.isEmpty() ? QString() : inheritedPaths.constLast()},
        {QStringLiteral("trackFileCount"), trackFiles.size()},
        {QStringLiteral("sourceChannelCount"), sourceChannels.size()},
        {QStringLiteral("sourceRecordingCount"),
         sourceChannels.isEmpty() ? 0 : 1},
        {QStringLiteral("driverChannel"),
         channelRow(allChannelRows, QStringLiteral("driver_id"))},
        {QStringLiteral("driverMappings"), driverRows},
        {QStringLiteral("carNumber"),
         nestedText(metadata,
                    {QStringLiteral("car"), QStringLiteral("number")})},
        {QStringLiteral("inheritedCarNumber"),
         nestedText(inheritedMetadata,
                    {QStringLiteral("car"), QStringLiteral("number")})},
        {QStringLiteral("carClass"),
         nestedText(metadata,
                    {QStringLiteral("car"), QStringLiteral("class")})},
        {QStringLiteral("inheritedCarClass"),
         nestedText(inheritedMetadata,
                    {QStringLiteral("car"), QStringLiteral("class")})},
        {QStringLiteral("event"),
         nestedText(metadata, {QStringLiteral("event")})},
        {QStringLiteral("inheritedEvent"),
         nestedText(inheritedMetadata, {QStringLiteral("event")})},
        {QStringLiteral("series"),
         nestedText(metadata, {QStringLiteral("series")})},
        {QStringLiteral("inheritedSeries"),
         nestedText(inheritedMetadata, {QStringLiteral("series")})},
        {QStringLiteral("trackName"),
         nestedText(metadata,
                    {QStringLiteral("track"), QStringLiteral("name")})},
        {QStringLiteral("inheritedTrackName"),
         nestedText(inheritedMetadata,
                    {QStringLiteral("track"), QStringLiteral("name")})},
        {QStringLiteral("trackSlug"),
         nestedText(metadata,
                    {QStringLiteral("track"), QStringLiteral("slug")})},
        {QStringLiteral("inheritedTrackSlug"),
         nestedText(inheritedMetadata,
                    {QStringLiteral("track"), QStringLiteral("slug")})},
        {QStringLiteral("channels"),
         channelRowsWithout(allChannelRows, QStringLiteral("driver_id"))},
    };
}

bool TelemetryStore::saveFolderMetadata(const QString& folderPath,
                                        const QVariantMap& metadata) {
    const QString canonical = canonicalDirectoryPath(folderPath);
    if (canonical.isEmpty()) return false;

    QString error;
    const QVariantMap document = metadataDocument(metadata);
    if (!omatrack::track_metadata::update(canonical, document, &error)) {
        qWarning().noquote() << error;
        return false;
    }
    const QString sidecarPath = omatrack::track_metadata::filePath(canonical);
    if (!trackMetadataPaths_.contains(sidecarPath)) {
        trackMetadataPaths_.append(sidecarPath);
        std::sort(trackMetadataPaths_.begin(), trackMetadataPaths_.end());
    }

    for (auto it = fileMetadata_.begin(); it != fileMetadata_.end(); ++it) {
        const QString relative = QDir(canonical).relativeFilePath(it.key());
        if (relative == QStringLiteral("..") ||
            relative.startsWith(QStringLiteral("../")))
            continue;
        it.value() = omatrack::track_metadata::readHierarchy(
            QFileInfo(it.key()).absolutePath());
    }

    auto isDescendantVideo = [&](const SessionHandle* session) {
        if (!session || !session->isVideo()) return false;
        const QString relative =
            QDir(canonical).relativeFilePath(session->path());
        return relative != QStringLiteral("..") &&
               !relative.startsWith(QStringLiteral("../"));
    };
    const bool reloadPrimary =
        primaryLap_ >= 0 && isDescendantVideo(primarySession_);
    const bool reloadCompare =
        compareLap_ >= 0 && isDescendantVideo(compareSession_);
    const int primaryLap = primaryLap_;
    const int compareLap = compareLap_;
    for (const auto& session : sessions_)
        if (isDescendantVideo(session.get())) session->clearUnifiedCache();
    if (reloadPrimary) requestLapLoad(primarySession_, primaryLap, false);
    if (reloadCompare) requestLapLoad(compareSession_, compareLap, true);

    emit videoMetadataChanged(canonical);
    emit sessionsChanged();
    emit selectionChanged();
    return true;
}

bool TelemetryStore::saveVideoMetadata(const QString& videoPath,
                                       const QVariantMap& metadata) {
    const QString canonical = canonicalInputPath(videoPath);
    if (canonical.isEmpty() || !isVideoPath(canonical)) return false;

    const QVariantMap document = metadataDocument(metadata);
    if (document.isEmpty())
        recordingMetadata_.remove(canonical);
    else
        recordingMetadata_.insert(canonical, document);
    savePreferences();

    SessionHandle* session = findSession(canonical);
    if (session) {
        const bool reloadPrimary =
            session == primarySession_ && primaryLap_ >= 0;
        const bool reloadCompare =
            session == compareSession_ && compareLap_ >= 0;
        const int primaryLap = primaryLap_;
        const int compareLap = compareLap_;
        session->clearUnifiedCache();
        if (reloadPrimary) requestLapLoad(session, primaryLap, false);
        if (reloadCompare) requestLapLoad(session, compareLap, true);
    }
    emit videoMetadataChanged(canonical);
    emit sessionsChanged();
    emit selectionChanged();
    return true;
}

void TelemetryStore::setSessionTrackAssignment(const QString& sessionKey,
                                               const QString& atlasSlug) {
    SessionHandle* session = findSession(sessionKey);
    if (!session) return;
    const QString slug = atlasSlug.trimmed().toLower();
    if (!slug.isEmpty() && !atlasTracks_.contains(slug)) return;

    const QFileInfo selectedInfo(session->path());
    const QString selectedDirectory = selectedInfo.absolutePath();
    QDir selectedParentDir(selectedDirectory);
    selectedParentDir.cdUp();
    const QString selectedParent = selectedParentDir.absolutePath();
    const QDate selectedDate =
        QDate::fromString(session->date(), QStringLiteral("dd/MM/yyyy"));
    QSet<QDate> nearbyDates;
    if (selectedDate.isValid()) {
        for (const auto& candidate : sessions_) {
            const QFileInfo candidateInfo(candidate->path());
            const QString candidateDirectory = candidateInfo.absolutePath();
            QDir candidateParentDir(candidateDirectory);
            candidateParentDir.cdUp();
            const bool sameEventPath =
                candidateDirectory == selectedDirectory ||
                candidateParentDir.absolutePath() == selectedParent;
            if (!sameEventPath) continue;
            const QDate candidateDate = QDate::fromString(
                candidate->date(), QStringLiteral("dd/MM/yyyy"));
            if (candidateDate.isValid()) nearbyDates.insert(candidateDate);
        }
    }

    QSet<QDate> eventDates;
    if (selectedDate.isValid()) {
        eventDates.insert(selectedDate);
        bool expanded = true;
        while (expanded) {
            expanded = false;
            for (const QDate& candidateDate : nearbyDates) {
                if (eventDates.contains(candidateDate)) continue;
                bool adjacent = false;
                for (const QDate& eventDate : eventDates) {
                    if (std::abs(eventDate.daysTo(candidateDate)) == 1) {
                        adjacent = true;
                        break;
                    }
                }
                if (!adjacent) continue;
                eventDates.insert(candidateDate);
                expanded = true;
                if (expanded) break;
            }
        }
    }

    QStringList keys;
    if (eventDates.isEmpty()) {
        keys.append(trackAssignmentKey(session));
    } else {
        for (const QDate& eventDate : eventDates)
            keys.append(eventDate.toString(Qt::ISODate));
    }
    for (const QString& key : keys) {
        if (slug.isEmpty())
            trackAssignments_.remove(key);
        else
            trackAssignments_.insert(key, slug);
    }
    // Remove the obsolete folder-scoped value once this event is edited.
    trackAssignments_.remove(selectedDirectory);
    savePreferences();
    if (primarySession_ && keys.contains(trackAssignmentKey(primarySession_))) {
        loadCornersForPrimary();
        emit cornersChanged();
        emit selectionChanged();
    }
    emit sessionsChanged();
}

QVariantList TelemetryStore::trackGroups() const {
    QVariantList tracks;
    QStringList trackNames;
    QMap<QString, QHash<QString, QStringList>> dateSessions;

    for (const auto& session : sessions_) {
        const QString resolvedTrack = displayTrack(session.get());
        const QString track =
            resolvedTrack.isEmpty() ? QStringLiteral("Unknown") : resolvedTrack;
        if (closedTracks_.contains(track)) continue;
        const QString date = session->date().isEmpty()
                                 ? QStringLiteral("Unknown")
                                 : session->date();
        if (!dateSessions.contains(track)) trackNames.append(track);
        dateSessions[track][date].append(session->stem() + "\n" +
                                         session->sessionKey());
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
                const QVariantMap metadata = configuredRecordingMetadataForPath(
                    session->path(), recordingMetadata_);
                auto metadataOr = [&](const QStringList& path,
                                      const QString& fallback) {
                    const QString value = nestedText(metadata, path);
                    return value.isEmpty() ? fallback : value;
                };
                sessionMaps.append(QVariantMap{
                    {"stem", stem},
                    {"key", key},
                    {"mappingKey", session->driverMappingKey()},
                    {"driver", driverDisplay(session)},
                    {"driverId", session->driverId()},
                    {"carNumber", metadataOr({QStringLiteral("car"),
                                              QStringLiteral("number")},
                                             session->carNumber())},
                    {"carClass", metadataOr({QStringLiteral("car"),
                                             QStringLiteral("class")},
                                            session->carClass())},
                    {"vehicle", session->vehicle()},
                    {"sessionTime", session->sessionTime()},
                    {"bestTime", session->bestLapTime()},
                    {"bestTimeMs", bestTimeMs},
                    {"isVideo", session->isVideo()}});
            }
            std::sort(sessionMaps.begin(), sessionMaps.end(),
                      [](const QVariantMap& a, const QVariantMap& b) {
                          const QTime at = QTime::fromString(
                              a.value("sessionTime").toString(), "HH:mm:ss");
                          const QTime bt = QTime::fromString(
                              b.value("sessionTime").toString(), "HH:mm:ss");
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
                driverBest[row.value("mappingKey").toString()] = std::min(
                    driverBest.value(row.value("mappingKey").toString(),
                                     std::numeric_limits<double>::max()),
                    best);
                dayBest = std::min(dayBest, best);
            }
            QVariantList sessions;
            for (QVariantMap row : sessionMaps) {
                const QString mappingKey = row.value("mappingKey").toString();
                const double best = row.value("bestTimeMs").toDouble();
                row.insert(
                    "isDriverBest",
                    best > 0.0 &&
                        qFuzzyCompare(best + 1.0,
                                      driverBest.value(mappingKey) + 1.0));
                row.insert(
                    "isDayBest",
                    best > 0.0 && qFuzzyCompare(best + 1.0, dayBest + 1.0));
                sessions.append(row);
            }
            dates.append(
                QVariantMap{{"date", dateName}, {"sessions", sessions}});
        }
        tracks.append(QVariantMap{{"track", trackName}, {"dates", dates}});
    }
    return tracks;
}

QVector<CornerZone> TelemetryStore::atlasCornersForPrimary() const {
    QVector<CornerZone> result;
    if (!primarySession_ || primaryLap_ < 0 || atlasTracks_.isEmpty())
        return result;

    const QString assignedSlug = resolvedTrackSlug(primarySession_);
    const QString wanted = normalizeAtlasName(
        assignedSlug.isEmpty() ? primarySession_->track() : assignedSlug);
    if (wanted.isEmpty()) return result;

    QJsonObject matchedTrack;
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

    const QString labelDefault =
        matchedLayout.value(QStringLiteral("label_default"))
            .toString(QStringLiteral("numbered"));
    auto fractionAtDistance = [&](double normalizedDistance) {
        if (!unified || unified->distance.size() < 2 || lapLength <= 0.0)
            return qBound(0.0, normalizedDistance, 1.0);
        const double target = qBound(0.0, normalizedDistance, 1.0) * lapLength;
        const auto it = std::lower_bound(unified->distance.begin(),
                                         unified->distance.end(), target);
        const int hi = std::clamp(int(it - unified->distance.begin()), 0,
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
            fractionAtDistance(range.value(QStringLiteral("start")).toDouble());
        corner.end =
            fractionAtDistance(range.value(QStringLiteral("end")).toDouble());
        if (!corner.name.isEmpty() && corner.end > corner.start)
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

    const QString assignedSlug = resolvedTrackSlug(primarySession_);
    const QString cornerTrack =
        assignedSlug.isEmpty() ? primarySession_->track() : assignedSlug;
    migrateLegacyCorners(cornerTrack);
    // A local edit wins over Track Atlas: the override lives in omatrack.yml
    // under the track key and is written the moment the user changes a zone.
    const QVariantList overrides =
        YamlConfig::instance().value(cornerConfigPath(cornerTrack)).toList();
    if (overrides.isEmpty()) return;
    QVector<CornerZone> loaded;
    for (const QVariant& entry : overrides) {
        const QVariantMap zone = entry.toMap();
        CornerZone corner;
        corner.name = zone.value(QStringLiteral("name")).toString().trimmed();
        if (corner.name.isEmpty()) continue;
        corner.start =
            qBound(0.0, zone.value(QStringLiteral("start")).toDouble(), 1.0);
        corner.end = qBound(corner.start,
                            zone.value(QStringLiteral("end")).toDouble(), 1.0);
        loaded.append(corner);
    }
    corners_ = loaded;
}

// ── selection ───────────────────────────────────────────────────────

void TelemetryStore::setPrimaryLapLoading(bool loading) {
    if (primaryLapLoading_ == loading) return;
    primaryLapLoading_ = loading;
    emit lapLoadingChanged();
}

void TelemetryStore::setCompareLapLoading(bool loading) {
    if (compareLapLoading_ == loading) return;
    compareLapLoading_ = loading;
    emit lapLoadingChanged();
}

void TelemetryStore::requestLapLoad(SessionHandle* session, int lapId,
                                    bool compare) {
    if (!session || lapId < 0) return;
    const std::shared_ptr<const UnifiedLap> cached = session->unifiedLap(lapId);
    if (cached) {
        if (compare) {
            ++compareLoadGeneration_;
            setCompareLapLoading(false);
            setCompare(session, lapId);
        } else {
            ++primaryLoadGeneration_;
            setPrimaryLapLoading(false);
            setPrimary(session, lapId);
        }
        return;
    }

    const LapEntry* wanted = nullptr;
    for (const LapEntry& lap : session->laps()) {
        if (lap.lapId == lapId) {
            wanted = &lap;
            break;
        }
    }
    if (!wanted) return;

    quint64 generation = 0;
    if (compare) {
        generation = ++compareLoadGeneration_;
        setCompareLapLoading(true);
    } else {
        generation = ++primaryLoadGeneration_;
        setPrimaryLapLoading(true);
    }
    const QString path = session->path();
    const QString sessionKey = session->sessionKey();
    const LapEntry lap = *wanted;
    const QVariantMap metadata =
        recordingMetadataForPath(path, recordingMetadata_);
    auto* watcher =
        new QFutureWatcher<std::shared_ptr<SessionLapLoadResult>>(this);
    connect(
        watcher,
        &QFutureWatcher<std::shared_ptr<SessionLapLoadResult>>::finished, this,
        [this, watcher, compare, generation]() {
            std::shared_ptr<SessionLapLoadResult> result = watcher->result();
            watcher->deleteLater();
            const quint64 currentGeneration =
                compare ? compareLoadGeneration_ : primaryLoadGeneration_;
            if (generation != currentGeneration) return;

            SessionHandle* session = findSession(result->sessionKey);
            if (!session || !result->error.isEmpty() || !result->source ||
                !result->unified) {
                qWarning() << "Unable to load lap" << result->sessionKey
                           << result->lapId << result->error;
                if (compare)
                    setCompareLapLoading(false);
                else
                    setPrimaryLapLoading(false);
                return;
            }
            session->adoptLoadedLap(result->lapId, std::move(result->source),
                                    std::move(result->unified),
                                    result->driverId, result->forceDriverId);
            if (compare) {
                setCompare(session, result->lapId);
                setCompareLapLoading(false);
            } else {
                setPrimary(session, result->lapId);
                setPrimaryLapLoading(false);
            }
        });
    watcher->setFuture(QtConcurrent::run([path, sessionKey, lap, metadata]() {
        return loadSessionLap(path, sessionKey, lap, metadata);
    }));
}

void TelemetryStore::setPrimary(SessionHandle* session, int lapId) {
    const bool sessionChanged = primarySession_ != session;
    primarySession_ = session;
    primaryLap_ = lapId;
    deltaCacheValid_ = false;
    damperAlignmentValid_ = false;
    invalidateComparisonAlignment();
    rebuildComparisonAlignment();
    lastPrimaryKey_ = session ? session->sessionKey() : QString();
    lastPrimaryLap_ = session ? lapId : -1;
    savePreferences();
    extraChannelCache_.clear();
    if (sessionChanged) setReferenceAlignment(0.0);
    cursorFrac_ = 0.0;
    loadCornersForPrimary();
    emit selectionChanged();
    emit cornersChanged();
    emit videoTimeChanged();
}

void TelemetryStore::setCompare(SessionHandle* session, int lapId) {
    const bool sessionChanged = compareSession_ != session;
    lastCompareKey_ = session ? session->sessionKey() : QString();
    lastCompareLap_ = session ? lapId : -1;
    savePreferences();
    compareSession_ = session;
    compareLap_ = lapId;
    deltaCacheValid_ = false;
    damperAlignmentValid_ = false;
    invalidateComparisonAlignment();
    rebuildComparisonAlignment();
    if (sessionChanged) setReferenceAlignment(0.0);
    emit selectionChanged();
}

void TelemetryStore::selectLap(const QString& sessionKey, int lapId) {
    SessionHandle* session = findSession(sessionKey);
    if (!session) return;
    if (primarySession_ == session && primaryLap_ == lapId) return;
    requestLapLoad(session, lapId, false);
}

void TelemetryStore::compareLap(const QString& sessionKey, int lapId) {
    SessionHandle* session = findSession(sessionKey);
    if (!session) return;
    if (primarySession_ == session && primaryLap_ == lapId) return;
    requestLapLoad(session, lapId, true);
}

void TelemetryStore::clearCompare() {
    ++compareLoadGeneration_;
    setCompareLapLoading(false);
    compareSession_ = nullptr;
    compareLap_ = -1;
    lastCompareKey_.clear();
    lastCompareLap_ = -1;
    deltaCacheValid_ = false;
    damperAlignmentValid_ = false;
    invalidateComparisonAlignment();
    setReferenceAlignment(0.0);
    savePreferences();
    emit selectionChanged();
    emit videoTimeChanged();
}

void TelemetryStore::clearPrimary() {
    ++primaryLoadGeneration_;
    ++compareLoadGeneration_;
    setPrimaryLapLoading(false);
    setCompareLapLoading(false);
    primarySession_ = nullptr;
    primaryLap_ = -1;
    compareSession_ = nullptr;
    compareLap_ = -1;
    lastPrimaryKey_.clear();
    lastPrimaryLap_ = -1;
    lastCompareKey_.clear();
    lastCompareLap_ = -1;
    deltaCacheValid_ = false;
    damperAlignmentValid_ = false;
    invalidateComparisonAlignment();
    corners_.clear();
    savePreferences();
    emit selectionChanged();
    emit videoTimeChanged();
    emit cornersChanged();
}

QVariantList TelemetryStore::lapsForSession(const QString& sessionKey) const {
    QVariantList out;
    SessionHandle* s = findSession(sessionKey);
    if (!s) return out;
    for (const LapEntry& l : s->laps()) {
        out.append(
            QVariantMap{{QStringLiteral("lapId"), l.lapId},
                        {QStringLiteral("label"), l.label},
                        {QStringLiteral("timeText"), l.timeText},
                        {QStringLiteral("timeMs"), l.timeMs},
                        {QStringLiteral("startTime"), l.startTime},
                        {QStringLiteral("isFastest"), l.isFastest},
                        {QStringLiteral("isComplete"), l.isComplete},
                        {QStringLiteral("isPitLap"), l.isPitLap},
                        {QStringLiteral("countsForBest"), l.countsForBest()}});
    }
    return out;
}

// ── navigation ──────────────────────────────────────────────────────

void TelemetryStore::setCursorFrac(double v) {
    v = qBound(0.0, v, 1.0);
    if (qFuzzyCompare(v, cursorFrac_)) return;
    cursorFrac_ = v;
    emit cursorFracChanged();
    emit videoTimeChanged();
}

void TelemetryStore::setChannelHeight(int v) {
    v = qBound(40, v, 400);
    if (v == channelHeight_) return;
    channelHeight_ = v;
    savePreferences();
    emit channelHeightChanged();
}

void TelemetryStore::setVideoMuted(bool muted) {
    if (muted == videoMuted_) return;
    videoMuted_ = muted;
    savePreferences();
    emit videoMutedChanged();
}

void TelemetryStore::setViewStart(double v) {
    v = qBound(0.0, v, 1.0);
    if (qFuzzyCompare(viewStart_ + 1.0, v + 1.0)) return;
    viewStart_ = v;
    emit viewChanged();
}
void TelemetryStore::setViewEnd(double v) {
    v = qBound(0.0, v, 1.0);
    if (qFuzzyCompare(viewEnd_ + 1.0, v + 1.0)) return;
    viewEnd_ = v;
    emit viewChanged();
}

void TelemetryStore::zoomAt(double anchorFrac, double factor) {
    double span = viewSpan();
    double newSpan = qBound(0.002, span * factor, 1.0);
    double anchor = qBound(0.0, anchorFrac, 1.0);
    double fracOfView = (anchor - viewStart_) / span;
    double ns = anchor - fracOfView * newSpan;
    double ne = ns + newSpan;
    if (ns < 0) {
        ne -= ns;
        ns = 0;
    }
    if (ne > 1) {
        ns -= (ne - 1);
        ne = 1;
        ns = qBound(0.0, ns, 1.0);
    }
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

void TelemetryStore::seekCursorSeconds(double seconds) {
    const UnifiedLap* lap = primaryUnified();
    if (!lap || lap->time.empty() || lap->time.back() <= 0.0) return;
    setCursorFrac(cursorFrac_ + seconds / lap->time.back());
}

void TelemetryStore::setCursorFromVideoTime(double mediaTime) {
    if (!std::isfinite(mediaTime) || !primarySession_ ||
        !primarySession_->isVideo() || primaryLap_ < 0)
        return;
    const omatrack::TelemetrySource* source = primarySession_->source();
    if (!source) return;
    const UnifiedLap* unified = primaryUnified();
    if (!unified || unified->time.size() < 2) return;
    for (const LapEntry& lap : primarySession_->laps()) {
        if (lap.lapId != primaryLap_) continue;
        const double relativeTime =
            mediaTime - source->mediaTimeOffsetSec() - lap.startTime;
        double fraction = 0.0;
        if (relativeTime >= unified->time.back()) {
            fraction = 1.0;
        } else if (relativeTime > unified->time.front()) {
            const auto upper = std::lower_bound(
                unified->time.begin(), unified->time.end(), relativeTime);
            const size_t high = size_t(upper - unified->time.begin());
            const size_t low = high - 1;
            const double span = unified->time[high] - unified->time[low];
            const double local =
                span > 0.0 ? (relativeTime - unified->time[low]) / span : 0.0;
            fraction = (double(low) + local) / double(unified->time.size() - 1);
        }
        if (qFuzzyCompare(fraction, cursorFrac_)) return;
        cursorFrac_ = fraction;
        emit cursorFracChanged();
        return;
    }
}

void TelemetryStore::jumpToFraction(double frac) { setCursorFrac(frac); }

void TelemetryStore::setReferenceAlignment(double fraction) {
    fraction = qBound(-0.15, fraction, 0.15);
    if (qFuzzyCompare(referenceAlignment_ + 1.0, fraction + 1.0)) return;
    referenceAlignment_ = fraction;
    emit referenceAlignmentChanged();
}

void TelemetryStore::resetReferenceAlignment() { setReferenceAlignment(0.0); }

double TelemetryStore::referenceAlignmentSeconds() const {
    const UnifiedLap* primary = primaryUnified();
    if (!primary || primary->time.empty()) return 0.0;
    return referenceAlignment_ * primary->time.back();
}

const DamperAlignment& TelemetryStore::damperAlignment() const {
    if (damperAlignmentValid_) return damperAlignment_;
    damperAlignmentValid_ = true;
    damperAlignment_ = DamperAlignment{};

    const UnifiedLap* primary = primaryUnified();
    const UnifiedLap* compare = compareUnified();
    if (!primary || !compare) return damperAlignment_;

    // Front axle mean; a single-corner logger still produces a usable trace.
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
    std::vector<double> primaryDamper = frontDamper(*primary);
    std::vector<double> compareDamper = frontDamper(*compare);
    if (primaryDamper.size() < 2 || compareDamper.size() < 2)
        return damperAlignment_;

    // Both laps share one vertical scale so a shift reads as a shift and not
    // as a change in travel.
    double minimum = 1e18;
    double maximum = -1e18;
    for (double value : primaryDamper) {
        minimum = std::min(minimum, value);
        maximum = std::max(maximum, value);
    }
    for (double value : compareDamper) {
        minimum = std::min(minimum, value);
        maximum = std::max(maximum, value);
    }
    if (!(maximum > minimum)) {
        minimum -= 1.0;
        maximum += 1.0;
    }

    damperAlignment_.primary = std::move(primaryDamper);
    damperAlignment_.compare = std::move(compareDamper);
    damperAlignment_.minimum = minimum;
    damperAlignment_.maximum = maximum;
    return damperAlignment_;
}

bool TelemetryStore::hasDamperAlignment() const {
    return damperAlignment().valid();
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
    QVector<std::pair<int, int>> zones;
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
    QVector<std::pair<int, int>> merged;
    for (const auto& z : zones) {
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

// One-time import of the pre-YAML per-track corner CSV so existing local
// edits survive the move to omatrack.yml.
void TelemetryStore::migrateLegacyCorners(const QString& track) {
    const QStringList path = cornerConfigPath(track);
    YamlConfig& config = YamlConfig::instance();
    if (!config.value(path).toList().isEmpty()) return;
    QString safeName = track.toLower();
    safeName.replace(' ', '_').replace('-', '_');
    QFile legacy;
    const QStringList roots{
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation),
        legacyAppDataPath()};
    for (const QString& root : roots) {
        legacy.setFileName(root + QStringLiteral("/") + safeName +
                           QStringLiteral(".csv"));
        if (legacy.open(QIODevice::ReadOnly)) break;
    }
    if (!legacy.isOpen()) return;
    QVariantList zones;
    const auto lines =
        QString::fromUtf8(legacy.readAll()).split('\n', Qt::SkipEmptyParts);
    for (int i = 1; i < lines.size(); ++i) {
        const auto parts = lines[i].split(',');
        if (parts.size() < 3) continue;
        const QString name = parts[0].trimmed();
        if (name.isEmpty()) continue;
        zones.append(QVariantMap{{QStringLiteral("name"), name},
                                 {QStringLiteral("start"), parts[1].trimmed()},
                                 {QStringLiteral("end"), parts[2].trimmed()}});
    }
    if (zones.isEmpty()) return;
    config.setValue(path, zones);
    config.save();
}

// omatrack.yml path for one track's corner override.
QStringList TelemetryStore::cornerConfigPath(const QString& track) {
    QString key = track.toLower();
    key.replace(' ', '_').replace('-', '_');
    key.remove(QRegularExpression(QStringLiteral("[^a-z0-9_]")));
    if (key.isEmpty()) key = QStringLiteral("unknown");
    return {QStringLiteral("tracks"), key, QStringLiteral("corners")};
}

void TelemetryStore::saveCorners() {
    if (!primarySession_) return;
    auto sorted = corners_;
    std::sort(sorted.begin(), sorted.end(),
              [](const CornerZone& a, const CornerZone& b) {
                  return a.start < b.start;
              });
    QVariantList zones;
    zones.reserve(sorted.size());
    for (const CornerZone& corner : sorted)
        zones.append(QVariantMap{
            {QStringLiteral("name"), corner.name},
            {QStringLiteral("start"), QString::number(corner.start, 'f', 6)},
            {QStringLiteral("end"), QString::number(corner.end, 'f', 6)}});
    YamlConfig& config = YamlConfig::instance();
    const QString assignedSlug = resolvedTrackSlug(primarySession_);
    const QString cornerTrack =
        assignedSlug.isEmpty() ? primarySession_->track() : assignedSlug;
    config.setValue(cornerConfigPath(cornerTrack), zones);
    config.save();
}
QVariantList TelemetryStore::cornerList() const {
    QVariantList out;
    for (const auto& c : corners_)
        out.append(
            QVariantMap{{"name", c.name}, {"start", c.start}, {"end", c.end}});
    return out;
}

QVariantList TelemetryStore::cornerComparison() const {
    QVariantList out;
    const UnifiedLap* primary = primaryUnified();
    if (!primary || primary->size() < 2 || primary->distance.size() < 2 ||
        primary->time.size() < 2)
        return out;

    auto sample = [](const std::vector<double>& values, double fraction) {
        if (values.empty()) return 0.0;
        const double position =
            qBound(0.0, fraction, 1.0) * (values.size() - 1);
        const int lo =
            std::clamp(int(std::floor(position)), 0, int(values.size()) - 1);
        const int hi = std::min(lo + 1, int(values.size()) - 1);
        return values[lo] + (values[hi] - values[lo]) * (position - lo);
    };
    auto frontDamper = [](const UnifiedLap& lap) {
        const size_t count = std::min(lap.damperFL.size(), lap.damperFR.size());
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

    auto stats = [&](const UnifiedLap& lap, const CornerZone& corner,
                     CornerGraphSeries& graphSeries) {
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
                maxSteering = std::max(maxSteering, std::fabs(lap.steering[i]));
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
            const int approachSamples = std::max(3, (apexIndex - first) / 6);
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
        const double noiseFloor = qBound(0.015, minThrottle + 0.015, 0.08);
        int pickupStart = first;
        for (int i = first; i <= finish && i < int(lap.throttle.size()); ++i) {
            if (lap.throttle[i] <= noiseFloor) {
                pickupStart = i;
                break;
            }
        }
        const double applicationTarget = qBound(0.10, minThrottle + 0.08, 0.30);
        int targetIndex = -1;
        for (int i = pickupStart;
             i + 3 <= finish && i + 3 < int(lap.throttle.size()); ++i) {
            bool sustained = true;
            for (int j = 0; j < 4; ++j)
                sustained =
                    sustained && lap.throttle[i + j] >= applicationTarget;
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
            for (int i = pickupStart + 1;
                 i <= finish && i < int(lap.throttle.size()); ++i) {
                if (lap.throttle[i] > noiseFloor &&
                    lap.throttle[i] > lap.throttle[i - 1] + 0.005) {
                    throttleIndex = i;
                    break;
                }
            }
        }
        auto distanceFromStart = [&](int index) {
            return index < int(lap.distance.size())
                       ? lap.distance[index] - startDistance
                       : 0.0;
        };
        const double turnInPoint = distanceFromStart(turnInIndex);
        const double apexPoint = distanceFromStart(apexIndex);
        const double throttlePoint = distanceFromStart(throttleIndex);

        // Corner traces are read in context: show at least 500 m around the
        // zone, with approach ahead of it and a longer exit behind it. The
        // corner itself stays identifiable through cornerStart/EndPosition.
        const double lapStart = lap.distance.front();
        const double lapEnd = lap.distance.back();
        const double cornerEndDistance =
            finish < int(lap.distance.size()) ? lap.distance[finish] : lapEnd;
        const double cornerLength =
            std::max(1.0, cornerEndDistance - startDistance);
        const double base = std::max(cornerLength, 500.0);
        const double windowStart =
            std::max(lapStart, startDistance - base * 0.35);
        const double windowEnd =
            std::min(lapEnd, cornerEndDistance + base * 0.55);
        const double windowMeters = std::max(1.0, windowEnd - windowStart);
        auto fractionAt = [&](double distance) {
            if (lap.distance.size() < 2) return 0.0;
            if (distance <= lap.distance.front()) return 0.0;
            if (distance >= lap.distance.back()) return 1.0;
            const auto it = std::lower_bound(lap.distance.begin(),
                                             lap.distance.end(), distance);
            const int hi = int(it - lap.distance.begin());
            const int lo = std::max(0, hi - 1);
            const double span = lap.distance[hi] - lap.distance[lo];
            const double local =
                span > 0.0 ? (distance - lap.distance[lo]) / span : 0.0;
            return (lo + local) / double(lap.distance.size() - 1);
        };
        auto windowPosition = [&](double distanceFromCornerStart) {
            return (startDistance + distanceFromCornerStart - windowStart) /
                   windowMeters;
        };
        const double turnInPosition = windowPosition(turnInPoint);
        const double apexPosition = windowPosition(apexPoint);
        const double throttlePosition = windowPosition(throttlePoint);
        const double cornerStartPosition = windowPosition(0.0);
        const double cornerEndPosition = windowPosition(cornerLength);

        // Fixed grid across the context window so primary and compare are
        // directly overlayable, and so the renderer can walk them without a
        // per-point QVariant.
        constexpr int seriesPoints = 200;
        CornerGraphSeries series;
        series.speed.reserve(seriesPoints);
        series.throttle.reserve(seriesPoints);
        series.brake.reserve(seriesPoints);
        series.steering.reserve(seriesPoints);
        series.maxBrake = maxBrake;
        for (int i = 0; i < seriesPoints; ++i) {
            const double fraction =
                fractionAt(windowStart +
                           windowMeters * double(i) / double(seriesPoints - 1));
            series.speed.push_back(sample(lap.speed, fraction));
            series.throttle.push_back(sample(lap.throttle, fraction));
            series.brake.push_back(sample(lap.brake, fraction));
            series.steering.push_back(sample(lap.steering, fraction));
        }
        graphSeries = std::move(series);

        const double entrySpeed = sample(lap.speed, corner.start);
        const double exitSpeed = sample(lap.speed, corner.end);
        return QVariantMap{
            {"entrySpeed", entrySpeed},
            {"apexSpeed", apex},
            {"exitSpeed", exitSpeed},
            {"speedDrop", entrySpeed - apex},
            {"speedGain", exitSpeed - apex},
            {"time",
             sample(lap.time, corner.end) - sample(lap.time, corner.start)},
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
            {"cornerStartPosition", cornerStartPosition},
            {"cornerEndPosition", cornerEndPosition},
            {"contextWindowMeters", windowMeters},
            {"cornerLengthMeters", cornerLength},
        };
    };
    auto fractionAtDistance = [](const UnifiedLap& lap, double distance) {
        if (lap.distance.size() < 2) return 0.0;
        if (distance <= lap.distance.front()) return 0.0;
        if (distance >= lap.distance.back()) return 1.0;
        const auto it = std::lower_bound(lap.distance.begin(),
                                         lap.distance.end(), distance);
        const int hi = int(it - lap.distance.begin());
        const int lo = hi - 1;
        const double span = lap.distance[hi] - lap.distance[lo];
        const double local =
            span > 0.0 ? (distance - lap.distance[lo]) / span : 0.0;
        return (lo + local) / double(lap.distance.size() - 1);
    };

    const UnifiedLap* compare = compareUnified();
    if (compare && (compare->distance.size() < 2 || compare->time.size() < 2))
        compare = nullptr;

    const std::vector<double> primaryDamper = frontDamper(*primary);
    const std::vector<double> compareDamper =
        compare ? frontDamper(*compare) : std::vector<double>();
    auto damperWindow = [](const UnifiedLap& lap,
                           const std::vector<double>& damper,
                           const CornerZone& corner,
                           std::vector<double>& windowSeries) {
        QVariantMap result;
        const int last =
            std::min(int(lap.size()), int(lap.distance.size())) - 1;
        if (last < 2 || damper.size() < 2) return result;
        const int cornerStart =
            std::clamp(int(std::floor(corner.start * last)), 0, last);
        const int cornerEnd =
            std::clamp(int(std::ceil(corner.end * last)), cornerStart, last);
        const double startDistance = lap.distance[cornerStart];
        const double windowStart =
            std::max(lap.distance.front(), startDistance - 300.0);
        const double windowEnd = lap.distance[cornerEnd];
        const auto lowerIndex = [&](double distance) {
            const auto it =
                std::lower_bound(lap.distance.begin(),
                                 lap.distance.begin() + last + 1, distance);
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
        for (int i = first + 1; i < finish && i < int(damper.size()) - 1; ++i) {
            const double score = scoreAt(i);
            if (score >= scoreAt(i - 1) && score >= scoreAt(i + 1) &&
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
        windowSeries.clear();
        windowSeries.reserve(points);
        const double span = std::max(1.0, windowEnd - windowStart);
        for (int i = 0; i < points; ++i) {
            const double distance =
                windowStart + span * double(i) / double(points - 1);
            const double value = valueAtDistance(distance);
            samples.append(value);
            windowSeries.push_back(value);
        }
        result.insert(QStringLiteral("series"), samples);
        result.insert(QStringLiteral("windowMeters"), span);
        result.insert(QStringLiteral("cornerStartMeters"),
                      startDistance - windowStart);
        result.insert(QStringLiteral("peakDistance"), lap.distance[peak]);
        result.insert(QStringLiteral("peakScore"), peakScore);
        return result;
    };

    cornerGraphs_.clear();
    cornerGraphs_.reserve(corners_.size());
    for (const CornerZone& corner : corners_) {
        CornerGraph graph;
        const QVariantMap primaryStats = stats(*primary, corner, graph.primary);
        graph.zoneStart = primaryStats.value("cornerStartPosition").toDouble();
        graph.zoneEnd = primaryStats.value("cornerEndPosition").toDouble();
        graph.turnIn = primaryStats.value("turnInPosition").toDouble();
        graph.apex = primaryStats.value("apexPosition").toDouble();
        graph.pickup = primaryStats.value("throttlePosition").toDouble();
        graph.windowMeters =
            primaryStats.value("contextWindowMeters").toDouble();
        graph.zoneMeters = primaryStats.value("cornerLengthMeters").toDouble();
        const QVariantMap primaryDamperData =
            damperWindow(*primary, primaryDamper, corner, graph.damper.primary);
        const bool hasPrimaryDamper =
            primaryDamperData.contains(QStringLiteral("series"));
        graph.damper.windowMeters =
            primaryDamperData.value(QStringLiteral("windowMeters")).toDouble();
        graph.damper.cornerStartMeters =
            primaryDamperData.value(QStringLiteral("cornerStartMeters"))
                .toDouble();
        QVariantMap row{
            {"name", corner.name},
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
            {"cornerStartPosition", primaryStats.value("cornerStartPosition")},
            {"cornerEndPosition", primaryStats.value("cornerEndPosition")},
            {"contextWindowMeters", primaryStats.value("contextWindowMeters")},
            {"cornerLengthMeters", primaryStats.value("cornerLengthMeters")},
            {"damperPrimarySeries", primaryDamperData.value("series")},
            {"damperWindowMeters", primaryDamperData.value("windowMeters")},
            {"damperCornerStartMeters",
             primaryDamperData.value("cornerStartMeters")},
            {"damperPeakPrimary", primaryDamperData.value("peakDistance")},
            {"damperCompareSeries", QVariantList{}},
            {"damperAlignment", 0.0},
            {"damperAlignmentValid", false},
            {"hasCompare", compare != nullptr}};

        if (compare) {
            const double startDistance =
                sample(primary->distance, corner.start);
            const double endDistance = sample(primary->distance, corner.end);
            CornerZone compareCorner = corner;
            compareCorner.start = fractionAtDistance(*compare, startDistance);
            compareCorner.end = fractionAtDistance(*compare, endDistance);
            const QVariantMap compareStats =
                stats(*compare, compareCorner, graph.compare);
            graph.compareTurnIn =
                compareStats.value("turnInPosition").toDouble();
            graph.compareApex = compareStats.value("apexPosition").toDouble();
            graph.comparePickup =
                compareStats.value("throttlePosition").toDouble();
            const QVariantMap compareDamperData = damperWindow(
                *compare, compareDamper, compareCorner, graph.damper.compare);
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
            row.insert(QStringLiteral("damperAlignment"), damperAlignment);
            row.insert(QStringLiteral("damperAlignmentValid"),
                       damperAlignmentValid);
            row.insert(QStringLiteral("damperPeakCompare"),
                       compareDamperData.value("peakDistance"));

            const double timeDelta = primaryStats.value("time").toDouble() -
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
                50.0 - timeDelta * 40.0 + exitDelta * 0.8 + apexDelta * 0.35,
                100.0);

            QStringList notes;
            if (timeDelta > 0.03)
                notes << QStringLiteral(
                             "Reference gains %1s through the corner")
                             .arg(timeDelta, 0, 'f', 3);
            else if (timeDelta < -0.03)
                notes << QStringLiteral("Primary gains %1s through the corner")
                             .arg(-timeDelta, 0, 'f', 3);
            if (exitDelta < -2.0)
                notes << QStringLiteral("Reference exits %1 km/h faster")
                             .arg(-exitDelta, 0, 'f', 1);
            if (apexDelta < -2.0)
                notes << QStringLiteral(
                             "Reference carries %1 km/h more at apex")
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

            row.insert("compareEntrySpeed", compareStats.value("entrySpeed"));
            row.insert("compareApexSpeed", compareStats.value("apexSpeed"));
            row.insert("compareExitSpeed", compareStats.value("exitSpeed"));
            row.insert("compareTime", compareStats.value("time"));
            row.insert("compareMinGear", compareStats.value("minGear"));
            row.insert("compareMaxSteering", compareStats.value("maxSteering"));
            row.insert("compareMaxBrake", compareStats.value("maxBrake"));
            row.insert("compareMinThrottle", compareStats.value("minThrottle"));
            row.insert("compareBrakePoint", compareStats.value("brakePoint"));
            row.insert("compareLiftPoint", compareStats.value("liftPoint"));
            row.insert("compareTurnInPosition",
                       compareStats.value("turnInPosition"));
            row.insert("compareApexPosition",
                       compareStats.value("apexPosition"));
            row.insert("compareThrottlePosition",
                       compareStats.value("throttlePosition"));
            row.insert("compareTurnInPoint", compareStats.value("turnInPoint"));
            row.insert("compareApexPoint", compareStats.value("apexPoint"));
            row.insert("compareThrottlePoint",
                       compareStats.value("throttlePoint"));
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
        cornerGraphs_.push_back(std::move(graph));
        out.append(row);
    }
    return out;
}

const CornerGraph* TelemetryStore::cornerGraph(int index) const {
    if (index < 0 || index >= int(cornerGraphs_.size())) return nullptr;
    return &cornerGraphs_[size_t(index)];
}

int TelemetryStore::addCorner(double start, double end) {
    start = qBound(0.0, start, 1.0);
    end = qBound(start, end, 1.0);
    if (end - start < 0.001) return -1;
    int number = 1;
    while (std::any_of(
        corners_.cbegin(), corners_.cend(), [number](const CornerZone& corner) {
            return corner.name.compare(QStringLiteral("Turn %1").arg(number),
                                       Qt::CaseInsensitive) == 0;
        }))
        ++number;
    CornerZone corner;
    corner.name = QStringLiteral("Turn %1").arg(number);
    corner.start = start;
    corner.end = end;
    corners_.append(corner);
    std::sort(corners_.begin(), corners_.end(),
              [](const CornerZone& a, const CornerZone& b) {
                  return a.start < b.start;
              });
    const int index = int(std::find_if(corners_.cbegin(), corners_.cend(),
                                       [&corner](const CornerZone& candidate) {
                                           return candidate.name == corner.name;
                                       }) -
                          corners_.cbegin());
    emit cornersChanged();
    saveCorners();
    return index;
}

void TelemetryStore::deleteCorner(int index) {
    if (index < 0 || index >= corners_.size()) return;
    corners_.removeAt(index);
    emit cornersChanged();
    saveCorners();
}

void TelemetryStore::setCornerName(int index, const QString& name) {
    if (index < 0 || index >= corners_.size()) return;
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty() || trimmed == corners_[index].name) return;
    // Corner labels are single-line UI text; YAML safely quotes punctuation.
    QString sanitized = trimmed;
    sanitized.replace('\r', ' ').replace('\n', ' ');
    corners_[index].name = sanitized;
    emit cornersChanged();
    saveCorners();
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
    if (channelVisible_.contains(key)) return channelVisible_.value(key);
    if (key.startsWith(QStringLiteral("raw:"))) {
        const bool visible = yamlBool(
            YamlConfig::instance().value(
                {QStringLiteral("channels"), key, QStringLiteral("visible")}),
            false);
        channelVisible_.insert(key, visible);
        return visible;
    }
    return false;
}

void TelemetryStore::setChannelVisible(const QString& key, bool visible) {
    if (channelVisible(key) == visible) return;
    channelVisible_[key] = visible;
    if (key.startsWith(QStringLiteral("raw:"))) {
        YamlConfig& config = YamlConfig::instance();
        config.setValue(
            {QStringLiteral("channels"), key, QStringLiteral("visible")},
            visible);
        config.save();
    } else {
        savePreferences();
    }
    emit channelConfigChanged();
}
const std::vector<double>* TelemetryStore::extraChannelData(
    const QString& key, bool reference) const {
    if (!key.startsWith(QStringLiteral("raw:"))) return nullptr;
    SessionHandle* session = reference ? compareSession_ : primarySession_;
    const int lapId = reference ? compareLap_ : primaryLap_;
    if (!session || lapId < 0) return nullptr;

    const QString cacheKey = session->sessionKey() + QStringLiteral("|") +
                             QString::number(lapId) + QStringLiteral("|") + key;
    auto cached = extraChannelCache_.constFind(cacheKey);
    if (cached != extraChannelCache_.cend()) return cached.value().get();

    const omatrack::TelemetrySource* source = session->source();
    if (!source) return nullptr;
    const QString rawName = key.mid(4);
    int channelIndex = -1;
    const auto& channels = source->channels();
    for (int index = 0; index < int(channels.size()); ++index) {
        if (QString::fromStdString(channels[size_t(index)].name) == rawName) {
            channelIndex = index;
            break;
        }
    }
    if (channelIndex < 0) return nullptr;

    const LapEntry* lap = nullptr;
    for (const LapEntry& candidate : session->laps()) {
        if (candidate.lapId == lapId) {
            lap = &candidate;
            break;
        }
    }
    if (!lap || lap->endTime <= lap->startTime) return nullptr;
    const int sampleCount =
        int(std::floor((lap->endTime - lap->startTime) * 50.0)) + 1;
    auto values =
        std::make_shared<std::vector<double>>(size_t(sampleCount), 0.0);
    for (int sample = 0; sample < sampleCount; ++sample) {
        double value = 0.0;
        const double time = lap->startTime + double(sample) / 50.0;
        if (source->sampleAt(size_t(channelIndex), time, &value))
            (*values)[size_t(sample)] = value;
        else if (sample > 0)
            (*values)[size_t(sample)] = (*values)[size_t(sample - 1)];
    }
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
        const omatrack::TelemetrySource* source = primarySession_->source();
        if (source) {
            for (const omatrack::RawChannel& channel : source->channels()) {
                if (channel.samples.size() < 2) continue;
                const QString key = QStringLiteral("raw:") +
                                    QString::fromStdString(channel.name);
                out.append(
                    QVariantMap{{"key", key},
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
        const QColor parsed(
            YamlConfig::instance()
                .value(
                    {QStringLiteral("channels"), key, QStringLiteral("color")},
                    defaultChannelColor(key).name(QColor::HexRgb))
                .toString());
        const QColor result =
            parsed.isValid() ? parsed : defaultChannelColor(key);
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
        YamlConfig& config = YamlConfig::instance();
        config.setValue(
            {QStringLiteral("channels"), key, QStringLiteral("color")},
            parsed.name(QColor::HexRgb));
        config.save();
    } else {
        savePreferences();
    }
    emit channelConfigChanged();
}

double TelemetryStore::channelWeight(const QString& key) const {
    if (channelWeights_.contains(key)) return channelWeights_.value(key);
    if (key.startsWith(QStringLiteral("raw:"))) {
        const double value = qBound(0.5,
                                    YamlConfig::instance()
                                        .value({QStringLiteral("channels"), key,
                                                QStringLiteral("weight")},
                                               1.0)
                                        .toDouble(),
                                    2.0);
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
        YamlConfig& config = YamlConfig::instance();
        config.setValue(
            {QStringLiteral("channels"), key, QStringLiteral("weight")},
            weight);
        config.save();
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
        out.append(QVariantMap{{"key", key},
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
    emit selectionChanged();
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
        out.append(
            QVariantMap{{"detected", it.key()}, {"display", it.value()}});
    return out;
}

void TelemetryStore::setDriverAlias(const QString& detected,
                                    const QString& display) {
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

const omatrack::UnifiedLap* TelemetryStore::primaryUnified() const {
    if (!primarySession_ || primaryLap_ < 0) return nullptr;
    auto u = primarySession_->unifiedLap(primaryLap_);
    return u ? u.get() : nullptr;
}

const omatrack::UnifiedLap* TelemetryStore::compareUnified() const {
    if (!compareSession_ || compareLap_ < 0) return nullptr;
    auto u = compareSession_->unifiedLap(compareLap_);
    return u ? u.get() : nullptr;
}

QVariantMap TelemetryStore::cursorReadout() const {
    QVariantMap out;
    const UnifiedLap* u = primaryUnified();
    if (!u || u->size() < 1) {
        out["dist"] = 0.0;
        out["time"] = 0.0;
        out["speed"] = 0.0;
        out["gear"] = 0;
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
    out["gear"] = u->gear.empty()
                      ? 0
                      : u->gear[qBound(0.0, frac, 1.0) * (u->gear.size() - 1)];
    out["corner"] = cornerNameAt(frac);
    // Δ vs compare lap (array from the shared cached deltaTrace())
    const QVector<double>& d = deltaTrace();
    if (!d.isEmpty()) out["delta"] = sampleQtAt(d, frac);
    return out;
}

const QVector<double>& TelemetryStore::deltaTrace() const {
    if (deltaCacheValid_) return deltaCache_;
    deltaCache_.clear();
    deltaCacheValid_ = true;

    const UnifiedLap* primary = primaryUnified();
    const UnifiedLap* compare = compareUnified();
    if (!primary || !compare || primary->size() < 3 || compare->size() < 3 ||
        primary->time.size() < 2 || compare->time.size() < 2)
        return deltaCache_;

    // One track-station map feeds delta, comparison traces, cursor readouts,
    // and video. GPS anchors correct the fused distance model only where the
    // receiver reports a good fix; speed propagation bridges the gaps.
    const int count = int(primary->size());
    deltaCache_.resize(count);
    double base = 0.0;
    for (int i = 0; i < count; ++i) {
        const double fraction = double(i) / double(count - 1);
        const double referenceTime = compareTimeForPrimaryFraction(fraction);
        if (referenceTime < 0.0) {
            deltaCache_.clear();
            return deltaCache_;
        }
        const double raw = primary->time[size_t(i)] - referenceTime;
        if (i == 0) base = raw;
        deltaCache_[i] = raw - base;
    }
    return deltaCache_;
}

// ── labels ──────────────────────────────────────────────────────────

QString TelemetryStore::primaryLabel() const {
    if (!primarySession_) return QString();
    QString s = displayTrack(primarySession_);
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

QString TelemetryStore::primaryDriverName() const {
    return primarySession_ ? driverDisplay(primarySession_) : QString();
}

QString TelemetryStore::primaryDriverMappingKey() const {
    return primarySession_ ? primarySession_->driverMappingKey() : QString();
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

QString TelemetryStore::roomName() const { return primaryLabel(); }

QString TelemetryStore::primarySessionKey() const {
    return primarySession_ ? primarySession_->sessionKey() : QString();
}

QString TelemetryStore::compareSessionKey() const {
    return compareSession_ ? compareSession_->sessionKey() : QString();
}

QUrl TelemetryStore::primaryVideoSource() const {
    if (!primarySession_ || !primarySession_->isVideo()) return {};
    return QUrl::fromLocalFile(primarySession_->path());
}

double TelemetryStore::primaryVideoTime() const {
    if (!primarySession_ || !primarySession_->isVideo() || primaryLap_ < 0)
        return 0.0;
    auto* session = const_cast<SessionHandle*>(primarySession_);
    const omatrack::TelemetrySource* source = session->source();
    const UnifiedLap* unified = primaryUnified();
    if (!source || !unified || unified->time.empty()) return 0.0;
    const double position =
        qBound(0.0, cursorFrac_, 1.0) * double(unified->time.size() - 1);
    const size_t low = size_t(std::floor(position));
    const size_t high = std::min(low + 1, unified->time.size() - 1);
    const double relativeTime =
        unified->time[low] +
        (unified->time[high] - unified->time[low]) * (position - double(low));
    for (const LapEntry& lap : session->laps()) {
        if (lap.lapId == primaryLap_)
            return source->mediaTimeOffsetSec() + lap.startTime + relativeTime;
    }
    return 0.0;
}

QUrl TelemetryStore::compareVideoSource() const {
    if (!compareSession_ || !compareSession_->isVideo()) return {};
    return QUrl::fromLocalFile(compareSession_->path());
}

void TelemetryStore::invalidateComparisonAlignment() {
    comparisonAlignmentTime_.clear();
    comparisonAlignmentFraction_.clear();
    comparisonAlignmentBasis_.clear();
    comparisonGpsAnchors_ = 0;
}

void TelemetryStore::rebuildComparisonAlignment() {
    comparisonAlignmentTime_.clear();
    comparisonAlignmentFraction_.clear();
    comparisonAlignmentBasis_.clear();
    comparisonGpsAnchors_ = 0;

    const UnifiedLap* primary = primaryUnified();
    const UnifiedLap* compare = compareUnified();
    if (!primary || !compare || primary->time.size() < 2 ||
        compare->time.size() < 2)
        return;

    // Comparison alignment is static for the selected lap pair. Build it on
    // selection, never from a paint or cursor-readout call.
    ComparisonAlignmentResult result =
        computeComparisonAlignment(*primary, *compare);
    comparisonAlignmentTime_ = std::move(result.time);
    comparisonAlignmentFraction_ = std::move(result.fraction);
    comparisonAlignmentBasis_ = std::move(result.basis);
    comparisonGpsAnchors_ = result.gpsAnchors;
}

QString TelemetryStore::comparisonAlignmentBasis() const {
    return comparisonAlignmentBasis_;
}

QString TelemetryStore::comparisonAlignmentConfidence() const {
    return comparisonAlignmentConfidenceLabel(comparisonAlignmentBasis_,
                                              comparisonGpsAnchors_);
}

int TelemetryStore::comparisonGpsAnchors() const {
    return comparisonGpsAnchors_;
}

double TelemetryStore::compareTimeForPrimaryFraction(double fraction) const {
    const UnifiedLap* primary = primaryUnified();
    if (!primary ||
        comparisonAlignmentTime_.size() != qsizetype(primary->time.size()) ||
        comparisonAlignmentTime_.isEmpty())
        return -1.0;
    const double position = std::clamp(fraction, 0.0, 1.0) *
                            double(comparisonAlignmentTime_.size() - 1);
    const qsizetype low = qsizetype(std::floor(position));
    const qsizetype high =
        std::min(low + 1, comparisonAlignmentTime_.size() - 1);
    return comparisonAlignmentTime_[low] +
           (comparisonAlignmentTime_[high] - comparisonAlignmentTime_[low]) *
               (position - double(low));
}

double TelemetryStore::compareFractionForPrimaryFraction(
    double fraction) const {
    if (comparisonAlignmentFraction_.size() < 2) return 0.0;
    const double position = std::clamp(fraction, 0.0, 1.0) *
                            double(comparisonAlignmentFraction_.size() - 1);
    const qsizetype low = qsizetype(std::floor(position));
    const qsizetype high =
        std::min(low + 1, comparisonAlignmentFraction_.size() - 1);
    return comparisonAlignmentFraction_[low] +
           (comparisonAlignmentFraction_[high] -
            comparisonAlignmentFraction_[low]) *
               (position - double(low));
}

double TelemetryStore::compareVideoTime() const {
    if (!compareSession_ || !compareSession_->isVideo() || compareLap_ < 0)
        return 0.0;
    const SessionHandle* session = compareSession_;
    const omatrack::TelemetrySource* source = session->source();
    const UnifiedLap* primary = primaryUnified();
    if (!source || !primary || primary->time.size() < 2) return 0.0;

    const double relativeTime =
        compareTimeForPrimaryFraction(qBound(0.0, cursorFrac_, 1.0));
    if (relativeTime < 0.0) return 0.0;
    for (const LapEntry& lap : session->laps()) {
        if (lap.lapId == compareLap_)
            return source->mediaTimeOffsetSec() + lap.startTime + relativeTime;
    }
    return 0.0;
}

double TelemetryStore::comparisonVideoRate() const {
    const UnifiedLap* primary = primaryUnified();
    if (!primary ||
        comparisonAlignmentTime_.size() != qsizetype(primary->time.size()) ||
        primary->time.size() < 3)
        return 1.0;
    const size_t center = size_t(std::llround(
        qBound(0.0, cursorFrac_, 1.0) * double(primary->time.size() - 1)));
    const size_t radius = size_t(std::max(5, primary->sampleRate / 2));
    const size_t low = center > radius ? center - radius : 0;
    const size_t high = std::min(center + radius, primary->time.size() - 1);
    const double primarySpan = primary->time[high] - primary->time[low];
    if (primarySpan <= 0.0) return 1.0;
    const double compareSpan = comparisonAlignmentTime_[qsizetype(high)] -
                               comparisonAlignmentTime_[qsizetype(low)];
    return std::clamp(compareSpan / primarySpan, 0.5, 2.0);
}
double TelemetryStore::sessionStartUnixTime() const { return 0.0; }

bool TelemetryStore::hasGlobalTime() const { return false; }
