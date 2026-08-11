#include "TelemetryStore.h"

#include "ComparisonAlignment.h"
#include "SessionMetadataCache.h"
#include "TrackMetadata.h"
#include "TrackAtlasSpatial.h"
#include "core/CornerAnalysis.h"
#include "core/TelemetryEngine.h"
#include "YamlConfig.h"
#include "RemoteCache.h"

#include <QCoreApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDebug>
#include <QDesktopServices>
#include <QElapsedTimer>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLocale>
#include <QJSEngine>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScopeGuard>
#include <QSettings>
#include <QSslError>
#include <QStandardPaths>
#include <QTextStream>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <utility>

namespace {
constexpr qint64 kTrackAtlasMaxAgeSeconds = 24 * 60 * 60;
constexpr int kTrackAtlasCheckIntervalMs = 6 * 60 * 60 * 1000;
constexpr int kMaximumRecentFiles = 6;
const QUrl kTrackAtlasUrl(QStringLiteral(
    "https://raw.githubusercontent.com/tobi/track-atlas/main/tracks.jsonl"));
const QString kTrackAtlasRawBase = QStringLiteral(
    "https://raw.githubusercontent.com/tobi/track-atlas/main/tracks/");
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

// One list, shared with the sync engine, which has to agree about what
// streams rather than downloads.
bool isVideoPath(const QString& path) { return omatrack::isVideoFile(path); }

/// A zero-byte stand-in for a video a connection streams instead of caching.
///
/// Nothing can be read out of it, so nothing should try: the readers below
/// check this before opening a file rather than reporting a corrupt one.
bool isStreamStub(const QString& path) {
    if (!isVideoPath(path)) return false;
    const QFileInfo info(path);
    return info.isFile() && info.size() == 0;
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

QString atlasGeometryKey(const QString& trackSlug, const QString& layoutId) {
    return trackSlug + QLatin1Char('/') + layoutId;
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

QString sidebarPinKind(const QString& role, const QString& path) {
    if (role == QStringLiteral("source") || role == QStringLiteral("folder"))
        return QStringLiteral("folder");
    if (role == QStringLiteral("file") && isVideoPath(path))
        return QStringLiteral("video");
    return {};
}

QString normalizedSidebarPinPath(const QString& path) {
    if (path.trimmed().isEmpty()) return {};
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
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
    QVariantMap folder;
    insertText(&folder, QStringLiteral("name"),
               metadata.value(QStringLiteral("folderName")));
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
    if (!folder.isEmpty()) document.insert(QStringLiteral("folder"), folder);
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
        const int sequentialNumber = lap.complete ? ++lapNumber : lapNumber;
        entry.label = lap.complete
                          ? QStringLiteral("L%1").arg(
                                lap.sourceNumber.value_or(sequentialNumber))
                      : i == 0                   ? QStringLiteral("Out")
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
        TelemetrySource::openIndex(path_.toStdString());
    if (!source) return false;
    captureSourceChannels(*source);
    populateLaps(source->detectLaps());
    applyEventDriverId(source->detectDriverId());
    captureGpsLocation(*source);
    return true;
}

bool SessionHandle::loadChannelSummaryForIndex() {
    std::unique_ptr<TelemetrySource> source =
        TelemetrySource::openIndex(path_.toStdString());
    if (!source) return false;
    captureSourceChannels(*source);
    return !sourceChannels_.isEmpty();
}

bool SessionHandle::loadSummaryForOpen(QString* errorString) {
    if (errorString) errorString->clear();
    std::string error;
    std::unique_ptr<TelemetrySource> source =
        TelemetrySource::open(path_.toStdString(), &error);
    if (!source) {
        if (errorString)
            *errorString =
                error.empty()
                    ? QStringLiteral("Unable to read embedded telemetry")
                    : QString::fromStdString(error);
        return false;
    }
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
    videoPresentationOffsetSec_ = source->videoPresentationOffsetSec();
    unifiedCache_.insert(lapId, std::move(unified));
    // source (decoded raw channel arrays, ~300 MB per session) is freed when
    // it goes out of scope here. The UnifiedLap is what rendering and analysis
    // use; extraChannelData() re-opens the file on demand for raw channels.
}

struct SessionScanResult {
    QVariantList fileSources;
    QStringList discoveredFilePaths;
    QHash<QString, QString> folderDisplayNames;
    QStringList trackMetadataPaths;
    QHash<QString, QString> remoteSourceNames;
    QHash<QString, QString> locationStatuses;
    QHash<QString, int> locationFileCounts;
    qint64 elapsedMs = 0;
};

struct SidebarMetadataResult {
    QString path;
    std::unique_ptr<SessionHandle> handle;
    QVariantMap metadata;
    bool unsupportedVideo = false;
};

struct FolderChannelSample {
    QVector<SourceChannelSummary> channels;
    QHash<QString, QString> automaticMappings;
    int candidateCount = 0;
    int recordingCount = 0;
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
struct CornerConsistencyLoadResult {
    QString key;
    QString sessionKey;
    int lapCount = 0;
    int validLapCount = 0;
    std::vector<double> brakePoints;
    QString error;
};
struct SessionConfidenceLoadResult {
    QString key;
    int lapCount = 0;
    QHash<QString, TraceConfidenceBand> bands;
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
    QString error;
};

namespace {
struct IndexedSession {
    std::unique_ptr<SessionHandle> handle;
    bool unsupportedVideo = false;
};

IndexedSession indexSession(const QString& path, SessionMetadataCache& cache,
                            int* cacheHits = nullptr,
                            int* cacheMisses = nullptr) {
    // A streamed video holds no bytes locally, so there is no embedded
    // telemetry to find and no point in a parser saying so. It plays; it just
    // does not carry laps of its own.
    if (isStreamStub(path)) {
        IndexedSession result;
        result.unsupportedVideo = true;
        return result;
    }
    const QString fingerprint = SessionMetadataCache::fingerprint(path);
    const SessionMetadataCache::Lookup cached = cache.lookup(fingerprint);
    if (cached.found && !cached.supported) {
        if (cacheHits) ++*cacheHits;
        IndexedSession result;
        result.unsupportedVideo = isVideoPath(path);
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
        result.unsupportedVideo = isVideoPath(path);
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

void mergeChannelSample(FolderChannelSample* sample,
                        const SessionHandle& session) {
    if (!sample || session.sourceChannels().isEmpty()) return;
    ++sample->recordingCount;
    QHash<QString, int> channelIndexes;
    for (int index = 0; index < sample->channels.size(); ++index)
        channelIndexes.insert(sample->channels.at(index).name.toCaseFolded(),
                              index);
    for (const SourceChannelSummary& source : session.sourceChannels()) {
        const QString key = source.name.toCaseFolded();
        const auto existing = channelIndexes.constFind(key);
        if (existing == channelIndexes.cend()) {
            sample->channels.append(source);
            channelIndexes.insert(key, sample->channels.size() - 1);
            continue;
        }
        SourceChannelSummary& merged = sample->channels[*existing];
        ++merged.recordingCount;
        if (merged.unit.isEmpty()) merged.unit = source.unit;
        if (!(merged.frequencyHz > 0.0))
            merged.frequencyHz = source.frequencyHz;
        for (const QString& example : source.examples) {
            if (!merged.examples.contains(example))
                merged.examples.append(example);
            if (merged.examples.size() == 5) break;
        }
    }
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
std::shared_ptr<CornerConsistencyLoadResult> loadCornerConsistency(
    const QString& path, const QString& sessionKey, const QString& key,
    const QVector<LapEntry>& laps, const QVariantMap& metadata,
    double startDistance, double endDistance) {
    auto result = std::make_shared<CornerConsistencyLoadResult>();
    result->key = key;
    result->sessionKey = sessionKey;
    result->lapCount = laps.size();

    std::string error;
    const std::unique_ptr<TelemetrySource> source =
        TelemetrySource::open(path.toStdString(), &error);
    if (!source) {
        result->error = error.empty()
                            ? QStringLiteral("Unable to open telemetry source")
                            : QString::fromStdString(error);
        return result;
    }

    const ChannelOverrides overrides = channelOverrides(metadata);
    const auto fractionAtDistance = [](const UnifiedLap& lap, double distance) {
        if (lap.distance.size() < 2) return 0.0;
        if (distance <= lap.distance.front()) return 0.0;
        if (distance >= lap.distance.back()) return 1.0;
        const auto it = std::lower_bound(lap.distance.begin(),
                                         lap.distance.end(), distance);
        const int hi = int(it - lap.distance.begin());
        const int lo = hi - 1;
        const double span = lap.distance[size_t(hi)] - lap.distance[size_t(lo)];
        const double local =
            span > 0.0 ? (distance - lap.distance[size_t(lo)]) / span : 0.0;
        return (lo + local) / double(lap.distance.size() - 1);
    };

    for (const LapEntry& lap : laps) {
        const UnifiedLap unified =
            source->unifyLap(lap.startTime, lap.endTime, overrides);
        if (unified.size() < 3 || unified.distance.size() < 3) continue;
        const double start = fractionAtDistance(unified, startDistance);
        const double end = fractionAtDistance(unified, endDistance);
        const omatrack::CornerMetrics metrics =
            omatrack::measureCorner(unified, start, end);
        if (!metrics.valid) continue;
        ++result->validLapCount;
        if (metrics.brakeIndex >= 0)
            result->brakePoints.push_back(metrics.brakePoint);
    }
    return result;
}

enum class TraceConfidenceField {
    Speed,
    Throttle,
    DriverThrottle,
    Brake,
    Clutch,
    Steering,
    Gear,
    DamperFrontLeft,
    LongitudinalG,
};

const std::vector<double>* traceConfidenceValues(const UnifiedLap& lap,
                                                 TraceConfidenceField field) {
    switch (field) {
        case TraceConfidenceField::Speed: return &lap.speed;
        case TraceConfidenceField::Throttle: return &lap.throttle;
        case TraceConfidenceField::DriverThrottle: return &lap.driverThrottle;
        case TraceConfidenceField::Brake: return &lap.brake;
        case TraceConfidenceField::Clutch: return &lap.clutch;
        case TraceConfidenceField::Steering: return &lap.steering;
        case TraceConfidenceField::DamperFrontLeft: return &lap.damperFL;
        case TraceConfidenceField::LongitudinalG: return &lap.gForceLong;
        case TraceConfidenceField::Gear: break;
    }
    return nullptr;
}

bool traceConfidenceFieldAvailable(const UnifiedLap& lap,
                                   TraceConfidenceField field) {
    if (field == TraceConfidenceField::Gear) return lap.gear.size() >= 2;
    const std::vector<double>* values = traceConfidenceValues(lap, field);
    return values && values->size() >= 2;
}

double sampleTraceConfidence(const UnifiedLap& lap, TraceConfidenceField field,
                             double fraction) {
    fraction = std::clamp(fraction, 0.0, 1.0);
    if (field == TraceConfidenceField::Gear) {
        if (lap.gear.empty()) return std::numeric_limits<double>::quiet_NaN();
        const size_t index = std::min(
            size_t(std::llround(fraction * double(lap.gear.size() - 1))),
            lap.gear.size() - 1);
        return double(lap.gear[index]);
    }
    const std::vector<double>* values = traceConfidenceValues(lap, field);
    if (!values || values->empty())
        return std::numeric_limits<double>::quiet_NaN();
    const double position = fraction * double(values->size() - 1);
    const size_t low = size_t(std::floor(position));
    const size_t high = std::min(low + 1, values->size() - 1);
    return (*values)[low] +
           ((*values)[high] - (*values)[low]) * (position - double(low));
}

std::shared_ptr<SessionConfidenceLoadResult> loadSessionConfidence(
    const QString& path, const QString& key, const QVector<LapEntry>& laps,
    const QVariantMap& metadata,
    const std::shared_ptr<const UnifiedLap>& primary) {
    auto result = std::make_shared<SessionConfidenceLoadResult>();
    result->key = key;
    if (!primary || primary->size() < 3 || laps.isEmpty()) return result;

    std::string error;
    const std::unique_ptr<TelemetrySource> source =
        TelemetrySource::open(path.toStdString(), &error);
    if (!source) {
        result->error = error.empty()
                            ? QStringLiteral("Unable to open telemetry source")
                            : QString::fromStdString(error);
        return result;
    }

    struct FieldMatrix {
        TraceConfidenceField field;
        QString key;
        std::vector<double> samples;
        int fieldLaps = 0;
    };
    std::vector<FieldMatrix> matrices{
        {TraceConfidenceField::Speed, QStringLiteral("speed"), {}, 0},
        {TraceConfidenceField::Throttle, QStringLiteral("throttle"), {}, 0},
        {TraceConfidenceField::DriverThrottle,
         QStringLiteral("driverThrottle"),
         {},
         0},
        {TraceConfidenceField::Brake, QStringLiteral("brake"), {}, 0},
        {TraceConfidenceField::Clutch, QStringLiteral("clutch"), {}, 0},
        {TraceConfidenceField::Steering, QStringLiteral("steering"), {}, 0},
        {TraceConfidenceField::Gear, QStringLiteral("gear"), {}, 0},
        {TraceConfidenceField::DamperFrontLeft,
         QStringLiteral("damperFL"),
         {},
         0},
        {TraceConfidenceField::LongitudinalG,
         QStringLiteral("gForceLong"),
         {},
         0},
    };
    const size_t sampleCount = primary->size();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    for (FieldMatrix& matrix : matrices)
        matrix.samples.assign(size_t(laps.size()) * sampleCount, nan);

    const ChannelOverrides overrides = channelOverrides(metadata);
    for (const LapEntry& lap : laps) {
        const UnifiedLap unified =
            source->unifyLap(lap.startTime, lap.endTime, overrides);
        if (unified.size() < 3) continue;
        const ComparisonAlignmentResult alignment =
            computeComparisonAlignment(*primary, unified);
        if (alignment.fraction.size() != qsizetype(sampleCount)) continue;

        const size_t row = size_t(result->lapCount);
        for (FieldMatrix& matrix : matrices) {
            if (!traceConfidenceFieldAvailable(unified, matrix.field)) continue;
            ++matrix.fieldLaps;
            const size_t offset = row * sampleCount;
            for (size_t sample = 0; sample < sampleCount; ++sample)
                matrix.samples[offset + sample] = sampleTraceConfidence(
                    unified, matrix.field,
                    alignment.fraction[qsizetype(sample)]);
        }
        ++result->lapCount;
    }

    const auto quantile = [](const std::vector<double>& sorted, double amount) {
        const double position = amount * double(sorted.size() - 1);
        const size_t low = size_t(std::floor(position));
        const size_t high = std::min(low + 1, sorted.size() - 1);
        return sorted[low] +
               (sorted[high] - sorted[low]) * (position - double(low));
    };
    std::vector<double> values;
    values.reserve(size_t(result->lapCount));
    for (const FieldMatrix& matrix : matrices) {
        if (matrix.fieldLaps < 2) continue;
        TraceConfidenceBand band;
        band.lower.assign(sampleCount, nan);
        band.median.assign(sampleCount, nan);
        band.upper.assign(sampleCount, nan);
        band.lapCount = matrix.fieldLaps;
        for (size_t sample = 0; sample < sampleCount; ++sample) {
            values.clear();
            for (int row = 0; row < result->lapCount; ++row) {
                const double value =
                    matrix.samples[size_t(row) * sampleCount + sample];
                if (std::isfinite(value)) values.push_back(value);
            }
            if (values.size() < 2) continue;
            std::sort(values.begin(), values.end());
            band.lower[sample] = quantile(values, 0.10);
            band.median[sample] = quantile(values, 0.50);
            band.upper[sample] = quantile(values, 0.90);
        }
        result->bands.insert(matrix.key, std::move(band));
    }
    return result;
}

/// The label shown when the user has not named a location: a folder's own
/// directory name, or a server's host.
QString defaultLocationName(const LibraryLocation& location) {
    if (location.type == LocationType::Folder) {
        const QString name = QDir(location.target).dirName();
        return name.isEmpty() ? location.target : name;
    }
    const QUrl url(location.target);
    return url.host().isEmpty() ? location.target : url.host();
}

RemoteConnection connectionFor(const LibraryLocation& location) {
    RemoteConnection connection;
    connection.id = location.id;
    connection.type = location.type;
    connection.name = location.name;
    connection.target = location.target;
    connection.username = location.username;
    connection.password = location.password;
    connection.options = location.options;
    return connection;
}

/// The cache directory a connection synchronizes into.
///
/// Every caller must go through here. cacheDirectory() recomputes the id from
/// the target and username when it is not given one, so a location whose
/// stored id came from somewhere else — legacy `webdav.connections` rows kept
/// their own — would otherwise have the sync writing to one directory while
/// the scan roots and the "Open cache folder" action pointed at another.
QString cachePathFor(const LibraryLocation& location) {
    return cacheDirectory(connectionFor(location));
}

/// Resolves one location to the local directory that should be scanned for
/// it, synchronizing connections into their cache first. Returns an empty
/// path when the location cannot contribute to this scan, and always reports
/// a status line for the preferences list.
QString resolveLocationDirectory(const LibraryLocation& location,
                                 QString* status) {
    if (!location.enabled) {
        *status = QStringLiteral("Disabled");
        return {};
    }
    if (location.type == LocationType::Folder) {
        const QFileInfo info(location.target);
        if (!info.exists() || !info.isDir()) {
            *status = QStringLiteral("Folder not found");
            return {};
        }
        if (!info.isReadable()) {
            *status = QStringLiteral("Folder not readable");
            return {};
        }
        *status = QStringLiteral("Available");
        return location.target;
    }

    const RemoteSyncResult synced = syncConnection(connectionFor(location));
    *status = synced.status;
    if (!synced.success || synced.cachePath.isEmpty()) return {};
    return synced.cachePath;
}

std::shared_ptr<SessionScanResult> scanLibraryLocations(
    const QVector<LibraryLocation>& locations, const QSet<QString>& extraPaths,
    const QString&) {
    QElapsedTimer timer;
    timer.start();
    auto result = std::make_shared<SessionScanResult>();
    QSet<QString> uniquePaths = extraPaths;
    QSet<QString> metadataPaths;
    const QStringList filters{
        "*.pds", "*.PDS", "*.ld",  "*.LD",  "*.ldx",  "*.LDX",  "*.vbo",
        "*.VBO", "*.mp4", "*.MP4", "*.mov", "*.MOV",  "*.mkv",  "*.MKV",
        "*.avi", "*.AVI", "*.m4v", "*.M4V", "*.webm", "*.WEBM", "TRACK.yml"};

    for (const LibraryLocation& location : locations) {
        QString status;
        const QString directory = resolveLocationDirectory(location, &status);
        result->locationStatuses.insert(location.id, status);
        if (directory.isEmpty()) {
            result->locationFileCounts.insert(location.id, 0);
            continue;
        }
        if (location.isConnection())
            result->remoteSourceNames.insert(
                normalizedSidebarPinPath(directory),
                location.name.trimmed().isEmpty() ? location.target
                                                  : location.name.trimmed());

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
        result->locationFileCounts.insert(location.id, sourcePaths.size());

        QVariantMap source = buildFileSource(directory, sourcePaths);
        const QString sourcePath = normalizedSidebarPinPath(directory);
        const auto remoteName = result->remoteSourceNames.constFind(sourcePath);
        if (remoteName != result->remoteSourceNames.cend()) {
            source.insert(QStringLiteral("name"), remoteName.value());
            source.insert(QStringLiteral("remote"), true);
        }
        if (!location.name.trimmed().isEmpty())
            source.insert(QStringLiteral("name"), location.name.trimmed());
        result->fileSources.append(source);
    }
    result->trackMetadataPaths =
        QStringList(metadataPaths.cbegin(), metadataPaths.cend());
    std::sort(result->trackMetadataPaths.begin(),
              result->trackMetadataPaths.end());
    result->discoveredFilePaths =
        QStringList(uniquePaths.cbegin(), uniquePaths.cend());
    std::sort(result->discoveredFilePaths.begin(),
              result->discoveredFilePaths.end());
    for (const QString& path : result->trackMetadataPaths) {
        const QString name =
            nestedText(YamlConfig::readDocument(path),
                       {QStringLiteral("folder"), QStringLiteral("name")});
        if (name.isEmpty()) continue;
        const QString directory = QFileInfo(path).absolutePath();
        result->folderDisplayNames.insert(normalizedSidebarPinPath(directory),
                                          name);
        const QString canonical = canonicalDirectoryPath(directory);
        if (!canonical.isEmpty())
            result->folderDisplayNames.insert(
                normalizedSidebarPinPath(canonical), name);
    }

    result->elapsedMs = timer.elapsed();
    return result;
}

std::shared_ptr<SidebarMetadataResult> loadSidebarMetadata(
    const QString& path, const QString& cachePath) {
    auto result = std::make_shared<SidebarMetadataResult>();
    result->path = path;
    result->metadata =
        omatrack::track_metadata::readHierarchy(QFileInfo(path).absolutePath());
    SessionMetadataCache cache(cachePath);
    IndexedSession indexed = indexSession(path, cache);
    cache.save();
    result->handle = std::move(indexed.handle);
    result->unsupportedVideo = indexed.unsupportedVideo;
    return result;
}

std::shared_ptr<FileOpenResult> openIndexedFile(const QString& path,
                                                const QString& cachePath,
                                                const QVariantMap& metadata,
                                                bool expectTelemetry) {
    auto result = std::make_shared<FileOpenResult>();
    result->path = path;
    SessionMetadataCache cache(cachePath);
    IndexedSession indexed = indexSession(path, cache);
    if (!indexed.handle && expectTelemetry && isVideoPath(path) &&
        !isStreamStub(path)) {
        auto handle = std::make_unique<SessionHandle>(path);
        if (!handle->loadSummaryForOpen(&result->error)) {
            cache.save();
            return result;
        }
        const QString fingerprint = SessionMetadataCache::fingerprint(path);
        cache.store(fingerprint, path, true, handle->metadataForCache());
        indexed.handle = std::move(handle);
        indexed.unsupportedVideo = false;
    }
    if (!indexed.handle) {
        cache.save();
        result->standaloneVideo = indexed.unsupportedVideo;
        return result;
    }
    const LapEntry* lap = bestLap(*indexed.handle);
    if (!lap && indexed.handle->isVideo()) {
        if (!indexed.handle->loadSummaryForOpen(&result->error)) {
            cache.save();
            return result;
        }
        const QString fingerprint = SessionMetadataCache::fingerprint(path);
        cache.store(fingerprint, path, true,
                    indexed.handle->metadataForCache());
        lap = bestLap(*indexed.handle);
    }
    cache.save();
    if (!lap) {
        result->error =
            indexed.handle->isVideo()
                ? QStringLiteral(
                      "No usable laps were found in the embedded "
                      "AiM telemetry")
                : QStringLiteral(
                      "No usable laps were found in this telemetry file");
        return result;
    }
    result->lap =
        loadSessionLap(path, indexed.handle->sessionKey(), *lap, metadata);
    if (!result->lap->error.isEmpty()) result->error = result->lap->error;
    result->handle = std::move(indexed.handle);
    return result;
}
}  // namespace

// ── TelemetryStore ──────────────────────────────────────────────────

TelemetryStore::TelemetryStore(QObject* parent) : QObject(parent) {
    if (s_storeInstance)
        qFatal("Only one TelemetryStore may exist in a process");
    s_storeInstance = this;
    sidebarMetadataPool_.setMaxThreadCount(1);
    sidebarMetadataPool_.setThreadPriority(QThread::LowPriority);
    scanWatcher_ = new QFutureWatcher<std::shared_ptr<SessionScanResult>>(this);
    connect(scanWatcher_,
            &QFutureWatcher<std::shared_ptr<SessionScanResult>>::finished, this,
            &TelemetryStore::finishSessionScan);
    // Corner focus follows the data it points at: a corner list that no
    // longer contains the focused zone drops the focus, and a new lap
    // selection recomputes its markers.
    connect(this, &TelemetryStore::cornersChanged, this, [this]() {
        if (focusedCorner_ < 0) return;
        if (focusedCorner_ >= corners_.size())
            clearCornerFocus();
        else {
            rebuildCornerMarkers();
            requestCornerConsistency();
        }
    });
    connect(this, &TelemetryStore::selectionChanged, this, [this]() {
        invalidateTraceConfidence();
        if (traceConfidenceMode_) requestTraceConfidence();
        if (focusedCorner_ < 0) return;
        rebuildCornerMarkers();
        requestCornerConsistency();
    });
    loadPreferences();
    loadLocations();
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
    sidebarMetadataPool_.waitForDone();
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
    const QVariantList pinRows =
        config.value({QStringLiteral("sidebar"), QStringLiteral("pins")})
            .toList();
    for (const QVariant& value : pinRows) {
        const QVariantMap row = value.toMap();
        const QString kind = row.value(QStringLiteral("kind")).toString();
        const QString path = normalizedSidebarPinPath(
            row.value(QStringLiteral("path")).toString());
        if ((kind != QStringLiteral("folder") &&
             kind != QStringLiteral("video")) ||
            path.isEmpty() || sidebarPinIndex(kind, path) >= 0)
            continue;
        sidebarPins_.append(SidebarPin{kind, path});
    }

    const QStringList configuredRecentFiles =
        config.value(QStringLiteral("recent_files")).toStringList();
    for (const QString& filePath : configuredRecentFiles) {
        if (filePath.isEmpty() || recentFiles_.contains(filePath)) continue;
        recentFiles_.append(filePath);
        if (recentFiles_.size() == kMaximumRecentFiles) break;
    }
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

void TelemetryStore::loadLocations() {
    YamlConfig& config = YamlConfig::instance();
    const QVariant configured = config.value(QStringLiteral("locations"));

    // `locations` replaced the split `telemetry_dirs` list and
    // `webdav.connections` map. Fold the old shape into the new one once and
    // prune the superseded keys, so the file only ever holds one library.
    if (!configured.isValid()) {
        const QVariant legacyDirs =
            config.value(QStringLiteral("telemetry_dirs"));
        const QStringList dirs = legacyDirs.isValid()
                                     ? legacyDirs.toStringList()
                                     : QStringList{defaultTelemetryDirectory()};
        for (const QString& directory : dirs)
            appendFolderLocation(directory, /*requireExists=*/false);

        const QVariantList legacyConnections =
            config
                .value(
                    {QStringLiteral("webdav"), QStringLiteral("connections")})
                .toList();
        for (const QVariant& value : legacyConnections) {
            const QVariantMap row = value.toMap();
            LibraryLocation location;
            location.type = LocationType::WebDav;
            location.target =
                row.value(QStringLiteral("url")).toString().trimmed();
            if (location.target.isEmpty()) continue;
            location.username =
                row.value(QStringLiteral("username")).toString().trimmed();
            location.password =
                row.value(QStringLiteral("password")).toString();
            location.name =
                row.value(QStringLiteral("name")).toString().trimmed();
            location.enabled =
                yamlBool(row.value(QStringLiteral("enabled")), true);
            location.id = row.value(QStringLiteral("id")).toString().trimmed();
            if (location.id.isEmpty())
                location.id = locationId(location.target, location.username);
            if (locationIndex(location.id) < 0)
                locations_.append(std::move(location));
        }
        config.remove({QStringLiteral("telemetry_dirs")});
        config.remove({QStringLiteral("webdav")});
        savePreferences();
        return;
    }

    for (const QVariant& value : configured.toList()) {
        const QVariantMap row = value.toMap();
        bool knownType = false;
        const LocationType type = locationTypeFromKey(
            row.value(QStringLiteral("type")).toString().trimmed(), &knownType);
        // An unknown type is a newer config than this build understands.
        // Skipping it keeps the entry in the file untouched rather than
        // silently reinterpreting it as a folder.
        if (!knownType) continue;

        LibraryLocation location;
        location.type = type;
        location.target =
            row.value(QStringLiteral("target")).toString().trimmed();
        if (location.target.isEmpty()) continue;
        location.name = row.value(QStringLiteral("name")).toString().trimmed();
        location.username =
            row.value(QStringLiteral("username")).toString().trimmed();
        location.password = row.value(QStringLiteral("password")).toString();
        const QVariantMap options =
            row.value(QStringLiteral("options")).toMap();
        for (auto it = options.cbegin(); it != options.cend(); ++it)
            location.options.insert(it.key(), it.value().toString().trimmed());
        location.enabled = yamlBool(row.value(QStringLiteral("enabled")), true);
        location.id = row.value(QStringLiteral("id")).toString().trimmed();
        if (location.id.isEmpty())
            location.id = location.type == LocationType::Folder
                              ? locationId(location.target, QString())
                              : locationId(location.target, location.username);
        if (locationIndex(location.id) < 0)
            locations_.append(std::move(location));
    }
}

int TelemetryStore::locationIndex(const QString& id) const {
    for (int i = 0; i < locations_.size(); ++i)
        if (locations_[i].id == id) return i;
    return -1;
}

bool TelemetryStore::appendFolderLocation(const QString& dirPath,
                                          bool requireExists) {
    const QString trimmed = dirPath.trimmed();
    if (trimmed.isEmpty()) return false;
    const QFileInfo info(trimmed);
    if (requireExists && (!info.exists() || !info.isDir())) return false;
    const QString absolute = info.absoluteFilePath();

    LibraryLocation location;
    location.type = LocationType::Folder;
    location.target = absolute;
    location.id = locationId(absolute, QString());
    for (const LibraryLocation& existing : std::as_const(locations_))
        if (existing.type == LocationType::Folder &&
            existing.target == absolute)
            return false;
    locations_.append(std::move(location));
    return true;
}

void TelemetryStore::savePreferences() {
    YamlConfig& config = YamlConfig::instance();
    QVariantList locationRows;
    locationRows.reserve(locations_.size());
    for (const LibraryLocation& location : std::as_const(locations_)) {
        QVariantMap row{
            {QStringLiteral("id"), location.id},
            {QStringLiteral("type"), locationTypeKey(location.type)},
            {QStringLiteral("target"), location.target},
            {QStringLiteral("enabled"), location.enabled},
        };
        if (!location.name.isEmpty())
            row.insert(QStringLiteral("name"), location.name);
        if (location.isConnection()) {
            if (!location.username.isEmpty())
                row.insert(QStringLiteral("username"), location.username);
            if (!location.password.isEmpty())
                row.insert(QStringLiteral("password"), location.password);
            QVariantMap options;
            for (auto it = location.options.cbegin();
                 it != location.options.cend(); ++it)
                if (!it.value().isEmpty()) options.insert(it.key(), it.value());
            if (!options.isEmpty())
                row.insert(QStringLiteral("options"), options);
        }
        locationRows.append(row);
    }
    config.setValue(
        {QStringLiteral("locations")},
        locationRows.isEmpty() ? QVariant() : QVariant(locationRows));
    config.setValue(QStringLiteral("recent_files"), recentFiles_);
    config.setValue(QStringLiteral("video/muted"), videoMuted_);
    QVariantList pinRows;
    pinRows.reserve(sidebarPins_.size());
    for (const SidebarPin& pin : std::as_const(sidebarPins_)) {
        pinRows.append(QVariantMap{{QStringLiteral("kind"), pin.kind},
                                   {QStringLiteral("path"), pin.path}});
    }
    config.setValue({QStringLiteral("sidebar"), QStringLiteral("pins")},
                    pinRows.isEmpty() ? QVariant() : QVariant(pinRows));
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

QString TelemetryStore::trackAtlasGeometryCachePath(
    const QString& trackSlug, const QString& layoutId) const {
    const QString directory = QFileInfo(trackAtlasCachePath()).absolutePath() +
                              QStringLiteral("/geometry");
    QDir().mkpath(directory);
    auto safeName = [](QString value) {
        value.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]")),
                      QStringLiteral("_"));
        return value;
    };
    return directory + QLatin1Char('/') + safeName(trackSlug) +
           QStringLiteral("__") + safeName(layoutId) +
           QStringLiteral(".geojson");
}

bool TelemetryStore::ensureAtlasCenterline(const QString& trackSlug,
                                           const QJsonObject& layout) {
    const QString layoutId = layout.value(QStringLiteral("id")).toString();
    if (trackSlug.isEmpty() || layoutId.isEmpty()) return false;
    const QString key = atlasGeometryKey(trackSlug, layoutId);
    const QString cachePath = trackAtlasGeometryCachePath(trackSlug, layoutId);
    const QFileInfo cacheInfo(cachePath);

    bool available = atlasCenterlines_.contains(key);
    if (!available && cacheInfo.exists()) {
        QFile cache(cachePath);
        if (cache.open(QIODevice::ReadOnly)) {
            QVector<QPointF> centerline =
                omatrack::trackatlas::parseCenterline(cache.readAll());
            if (!centerline.isEmpty()) {
                atlasCenterlines_.insert(key, std::move(centerline));
                available = true;
            }
        }
    }

    const qint64 ageSeconds =
        cacheInfo.exists()
            ? cacheInfo.lastModified().secsTo(QDateTime::currentDateTimeUtc())
            : kTrackAtlasMaxAgeSeconds;
    if (!available || !cacheInfo.exists() || ageSeconds < 0 ||
        ageSeconds >= kTrackAtlasMaxAgeSeconds)
        requestAtlasCenterline(trackSlug, layout);
    return available;
}

void TelemetryStore::requestAtlasCenterline(const QString& trackSlug,
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
        emit trackAtlasChanged();
    }

    const QUrl url(kTrackAtlasRawBase + trackSlug + QStringLiteral("/raw/") +
                   relativePath);
    QNetworkRequest request(url);
    request.setHeader(
        QNetworkRequest::UserAgentHeader,
        QStringLiteral("Omatrack/") + QCoreApplication::applicationVersion());
    request.setTransferTimeout(15000);
    QNetworkReply* reply = atlasNetwork_->get(request);
    connect(
        reply, &QNetworkReply::finished, this,
        [this, reply, trackSlug, layoutId, key]() {
            const auto cleanup =
                qScopeGuard([reply]() { reply->deleteLater(); });
            atlasGeometryRequests_.remove(key);
            if (reply->error() != QNetworkReply::NoError) {
                if (!atlasCenterlines_.contains(key)) {
                    trackAtlasStatus_ = QStringLiteral(
                        "Track-atlas layout geometry unavailable");
                    emit trackAtlasChanged();
                }
                return;
            }

            const QByteArray payload = reply->readAll();
            QVector<QPointF> centerline =
                omatrack::trackatlas::parseCenterline(payload);
            if (centerline.isEmpty()) {
                if (!atlasCenterlines_.contains(key)) {
                    trackAtlasStatus_ =
                        QStringLiteral("Track-atlas layout geometry invalid");
                    emit trackAtlasChanged();
                }
                return;
            }
            atlasCenterlines_.insert(key, std::move(centerline));
            atlasSpatialMappings_.clear();

            QSaveFile output(trackAtlasGeometryCachePath(trackSlug, layoutId));
            const bool opened = output.open(QIODevice::WriteOnly);
            const bool written =
                opened && output.write(payload) == qsizetype(payload.size());
            const bool cached = written && output.commit();
            loadCornersForPrimary();
            if (!cached) {
                trackAtlasStatus_ = QStringLiteral(
                    "Track-atlas geometry ready; cache write failed");
                emit trackAtlasChanged();
            }
            emit cornersChanged();
        });
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
    return QStandardPaths::writableLocation(
               QStandardPaths::GenericCacheLocation) +
           QStringLiteral("/omatrack/session-index.json");
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
    const QVector<LibraryLocation> locations = locations_;
    const QSet<QString> extraPaths = transientSessionPaths_;
    const QString cachePath = sessionIndexCachePath();
    scanWatcher_->setFuture(
        QtConcurrent::run([locations, extraPaths, cachePath]() {
            return scanLibraryLocations(locations, extraPaths, cachePath);
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
    fileSources_ = result->fileSources;
    discoveredFilePaths_ = result->discoveredFilePaths;
    folderDisplayNames_ = result->folderDisplayNames;
    trackMetadataPaths_ = result->trackMetadataPaths;
    locationStatuses_ = result->locationStatuses;
    locationFileCounts_ = result->locationFileCounts;
    transientSessionPaths_.clear();
    qInfo() << "File discovery:" << result->elapsedMs
            << "ms; sidebar metadata remains lazy";
    if (!ready_) {
        ready_ = true;
        emit readyChanged();
    }
    loading_ = false;
    emit loadingChanged();
    emit sessionsChanged();
    emit locationsChanged();
}

void TelemetryStore::addSessionDirectory(const QString& dirPath) {
    if (!appendFolderLocation(dirPath)) return;
    savePreferences();
    emit locationsChanged();
    scan();
}

void TelemetryStore::removeSessionDirectory(const QString& dirPath) {
    const QFileInfo info(dirPath);
    const QString absolute =
        info.absoluteFilePath().isEmpty() ? dirPath : info.absoluteFilePath();
    for (int i = 0; i < locations_.size(); ++i) {
        if (locations_[i].type != LocationType::Folder) continue;
        if (locations_[i].target != absolute && locations_[i].target != dirPath)
            continue;
        removeLocation(locations_[i].id);
        return;
    }
}

void TelemetryStore::openFile(const QString& filePath) {
    queueFileOpen(filePath, FileOpenRole::Automatic);
}

void TelemetryStore::queueFileOpen(const QString& filePath, FileOpenRole role) {
    if (filePath.trimmed().isEmpty()) {
        emit operationError(QStringLiteral("Unable to open file"),
                            QStringLiteral("No file was selected."));
        return;
    }
    if (!QFileInfo::exists(filePath)) {
        emit operationError(
            QStringLiteral("Unable to open file"),
            QStringLiteral("The file no longer exists:\n%1").arg(filePath));
        return;
    }
    const QString resolvedPath = telemetryPathForInput(filePath);
    if (resolvedPath.isEmpty()) {
        emit operationError(
            QStringLiteral("Missing telemetry companion"),
            QStringLiteral("%1 needs a matching .ld file in the same folder.")
                .arg(QFileInfo(filePath).fileName()));
        return;
    }
    if (!QFileInfo::exists(resolvedPath)) {
        emit operationError(
            QStringLiteral("Unable to open file"),
            QStringLiteral("The telemetry source could not be found:\n%1")
                .arg(resolvedPath));
        return;
    }
    const QFileInfo resolved(resolvedPath);
    const QString telemetryPath = resolved.canonicalFilePath().isEmpty()
                                      ? resolved.absoluteFilePath()
                                      : resolved.canonicalFilePath();
    if (loading_) {
        transientSessionPaths_.insert(telemetryPath);
        rescanPending_ = true;
    }
    pendingFileOpens_.append(PendingFileOpen{telemetryPath, role});
    pauseSidebarMetadataQueue();
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

    const PendingFileOpen request = pendingFileOpens_.takeFirst();
    const QString path = request.path;
    const QString cachePath = sessionIndexCachePath();
    const QVariantMap metadata =
        recordingMetadataForPath(path, recordingMetadata_);
    auto* watcher = new QFutureWatcher<std::shared_ptr<FileOpenResult>>(this);
    connect(
        watcher, &QFutureWatcher<std::shared_ptr<FileOpenResult>>::finished,
        this, [this, watcher, role = request.role]() {
            std::shared_ptr<FileOpenResult> result = watcher->result();
            watcher->deleteLater();
            if (result->standaloneVideo) {
                // An ordinary video has no analytical relationship to the
                // currently selected laps. Clear both roles before handing
                // playback to QML so stale traces are never presented as if
                // they belonged to this recording.
                clearPrimary();
                transientSessionPaths_.remove(result->path);
                rememberRecentFile(result->path);
                emit standaloneVideoRequested(videoSourceFor(result->path));
            } else if (result->handle && result->lap &&
                       result->lap->error.isEmpty() && result->lap->source &&
                       result->lap->unified) {
                transientSessionPaths_.insert(result->path);
                rememberRecentFile(result->path);
                SessionHandle* session = findSession(result->path);
                bool added = false;
                if (!session) {
                    session = result->handle.get();
                    sessions_.push_back(std::move(result->handle));
                    added = true;
                } else {
                    const bool hasLoadedLap = std::any_of(
                        session->laps().cbegin(), session->laps().cend(),
                        [result](const LapEntry& lap) {
                            return lap.lapId == result->lap->lapId;
                        });
                    if (!hasLoadedLap) {
                        for (auto& candidate : sessions_) {
                            if (candidate.get() != session) continue;
                            const bool wasPrimary = primarySession_ == session;
                            const bool wasCompare = compareSession_ == session;
                            candidate = std::move(result->handle);
                            session = candidate.get();
                            if (wasPrimary) primarySession_ = session;
                            if (wasCompare) compareSession_ = session;
                            break;
                        }
                    }
                }
                session->adoptLoadedLap(
                    result->lap->lapId, std::move(result->lap->source),
                    std::move(result->lap->unified), result->lap->driverId,
                    result->lap->forceDriverId);
                if (added) emit sessionsChanged();
                if (role == FileOpenRole::Compare ||
                    (role == FileOpenRole::Automatic && session->isVideo() &&
                     primarySession_ && primarySession_->isVideo() &&
                     primarySession_ != session)) {
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
                transientSessionPaths_.remove(result->path);
                qWarning() << "Unable to open telemetry file" << result->path
                           << result->error;
                const QString detail =
                    result->error.isEmpty()
                        ? QStringLiteral(
                              "The file does not contain supported "
                              "telemetry.")
                        : result->error;
                emit operationError(
                    QStringLiteral("Unable to open telemetry"),
                    QStringLiteral("%1\n\n%2")
                        .arg(QFileInfo(result->path).fileName(), detail));
            }
            fileOpenLoading_ = false;
            if (pendingFileOpens_.isEmpty()) {
                emit lapLoadingChanged();
                resumeSidebarMetadataQueue();
            } else {
                startNextFileOpen();
            }
        });
    const bool expectTelemetry = request.role != FileOpenRole::Automatic;
    watcher->setFuture(
        QtConcurrent::run([path, cachePath, metadata, expectTelemetry]() {
            return openIndexedFile(path, cachePath, metadata, expectTelemetry);
        }));
}

void TelemetryStore::rememberRecentFile(const QString& filePath) {
    recentFiles_.removeAll(filePath);
    recentFiles_.prepend(filePath);
    while (recentFiles_.size() > kMaximumRecentFiles) recentFiles_.removeLast();
    savePreferences();
    emit recentFilesChanged();
}

void TelemetryStore::clearSessions() {
    sessions_.clear();
    sidebarMetadataQueue_.clear();
    sidebarMetadataQueued_.clear();
    sidebarMetadataLoaded_.clear();
    sidebarMetadataLoadingPath_.clear();
    ++sidebarMetadataGeneration_;
    fileSources_.clear();
    discoveredFilePaths_.clear();
    folderDisplayNames_.clear();
    folderChannelSamples_.clear();
    folderChannelSampleRequests_.clear();
    ++folderChannelSampleGeneration_;
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
    atlasSpatialMappings_.clear();
    setReferenceAlignment(0.0);
    corners_.clear();
    emit selectionChanged();
    emit videoTimeChanged();
    emit cornersChanged();
}

QStringList TelemetryStore::sessionDirectories() const {
    QStringList directories;
    directories.reserve(locations_.size());
    for (const LibraryLocation& location : std::as_const(locations_)) {
        if (!location.enabled) continue;
        if (location.type == LocationType::Folder) {
            directories.append(location.target);
            continue;
        }
        const QString cache = cachePathFor(location);
        if (!cache.isEmpty() && QFileInfo::exists(cache))
            directories.append(cache);
    }
    return directories;
}

QVariantList TelemetryStore::connectionTypes() const {
    // ConnectionDialog builds its whole form from this list — the labels on
    // the credential fields and any protocol-specific rows included — so a
    // new connection kind appears in the UI without touching any QML.
    return QVariantList{
        QVariantMap{
            {QStringLiteral("type"), locationTypeKey(LocationType::WebDav)},
            {QStringLiteral("label"), QStringLiteral("WebDAV server")},
            {QStringLiteral("placeholder"),
             QStringLiteral("https://server.example/dav/")},
            {QStringLiteral("needsCredentials"), true},
            {QStringLiteral("detail"),
             QStringLiteral("Files are synchronized into a local cache before "
                            "scanning, and stay available offline.")}},
        QVariantMap{
            {QStringLiteral("type"), locationTypeKey(LocationType::S3)},
            {QStringLiteral("label"), QStringLiteral("S3 bucket")},
            {QStringLiteral("placeholder"),
             QStringLiteral("s3://bucket/season-2026/")},
            {QStringLiteral("needsCredentials"), true},
            {QStringLiteral("usernameLabel"), QStringLiteral("Access key")},
            {QStringLiteral("passwordLabel"), QStringLiteral("Secret key")},
            {QStringLiteral("detail"),
             QStringLiteral(
                 "Telemetry is synchronized into a local cache and stays "
                 "available offline. Leave the keys empty for a public "
                 "bucket.")},
            {QStringLiteral("extraFields"),
             QVariantList{
                 QVariantMap{
                     {QStringLiteral("key"), QStringLiteral("region")},
                     {QStringLiteral("label"), QStringLiteral("Region")},
                     {QStringLiteral("placeholder"),
                      QStringLiteral("Detected automatically")}},
                 QVariantMap{
                     {QStringLiteral("key"), QStringLiteral("endpoint")},
                     {QStringLiteral("label"), QStringLiteral("Endpoint")},
                     {QStringLiteral("placeholder"),
                      QStringLiteral("For MinIO, R2, or another S3 API")}}}}},
        QVariantMap{
            {QStringLiteral("type"), locationTypeKey(LocationType::Gcs)},
            {QStringLiteral("label"), QStringLiteral("Google Cloud Storage")},
            {QStringLiteral("placeholder"),
             QStringLiteral("gs://bucket/season-2026/")},
            {QStringLiteral("needsCredentials"), true},
            {QStringLiteral("usernameLabel"), QStringLiteral("Access key")},
            {QStringLiteral("passwordLabel"), QStringLiteral("Secret")},
            // HMAC keys, not a service-account file: they are what the
            // S3-compatible endpoint takes, and they need no key material on
            // disk beyond what every other connection here already stores.
            {QStringLiteral("detail"),
             QStringLiteral(
                 "Uses an HMAC interoperability key, which you create under "
                 "Cloud Storage → Settings → Interoperability. Telemetry is "
                 "cached locally and stays available offline.")}}};
}

QVariantMap TelemetryStore::cacheUsage() const {
    const qint64 bytes = cacheUsageBytes();
    return QVariantMap{
        {QStringLiteral("bytes"), bytes},
        {QStringLiteral("text"), QLocale().formattedDataSize(bytes)}};
}

void TelemetryStore::clearCache() {
    omatrack::clearCache();
    // The library was scanning those files a moment ago, so re-scan rather
    // than leave rows pointing at paths that no longer exist. Every enabled
    // connection downloads again as part of it.
    locationStatuses_.clear();
    locationFileCounts_.clear();
    emit locationsChanged();
    scan();
}

QVariantList TelemetryStore::libraryLocations() const {
    QVariantList rows;
    rows.reserve(locations_.size());
    for (const LibraryLocation& location : std::as_const(locations_)) {
        const bool folder = location.type == LocationType::Folder;
        const QString cachePath = cachePathFor(location);
        QVariantMap options;
        for (auto it = location.options.cbegin(); it != location.options.cend();
             ++it)
            options.insert(it.key(), it.value());
        // A folder reports liveness directly; a connection only knows what
        // the last sync said, so it falls back to "Not connected yet".
        const QString fallback =
            folder ? (QFileInfo(location.target).isDir()
                          ? QStringLiteral("Not scanned yet")
                          : QStringLiteral("Folder not found"))
                   : QStringLiteral("Not connected yet");
        const QString status =
            location.enabled ? locationStatuses_.value(location.id, fallback)
                             : QStringLiteral("Disabled");
        rows.append(QVariantMap{
            {QStringLiteral("id"), location.id},
            {QStringLiteral("type"), locationTypeKey(location.type)},
            {QStringLiteral("isConnection"), location.isConnection()},
            {QStringLiteral("name"), location.name.isEmpty()
                                         ? defaultLocationName(location)
                                         : location.name},
            {QStringLiteral("target"), location.target},
            {QStringLiteral("username"), location.username},
            {QStringLiteral("hasPassword"), !location.password.isEmpty()},
            {QStringLiteral("options"), options},
            {QStringLiteral("enabled"), location.enabled},
            // A connection counts as available once it has a populated cache:
            // that is what the library can actually scan, online or not.
            {QStringLiteral("available"),
             folder ? QFileInfo(location.target).isDir()
                    : !cachePath.isEmpty() && QFileInfo(cachePath).isDir()},
            {QStringLiteral("status"), status},
            {QStringLiteral("fileCount"),
             locationFileCounts_.value(location.id, -1)},
            {QStringLiteral("cachePath"), cachePath}});
    }
    return rows;
}

QString TelemetryStore::saveConnection(const QVariantMap& fields) {
    bool knownType = false;
    const LocationType type = locationTypeFromKey(
        fields.value(QStringLiteral("type")).toString().trimmed(), &knownType);
    if (!knownType || type == LocationType::Folder)
        return QStringLiteral("Unsupported connection type.");

    // The protocol owns what a usable address looks like, so that the dialog
    // and the sync can never disagree about whether one is acceptable.
    const QString target =
        fields.value(QStringLiteral("target")).toString().trimmed();
    const QString invalid = validateTarget(type, target);
    if (!invalid.isEmpty()) return invalid;

    LibraryLocation location;
    location.type = type;
    // Normalization is the protocol's, not QUrl's: QUrl lowercases an
    // authority, and an S3 bucket is not a hostname — a capital letter in one
    // would be silently rewritten into a bucket that does not exist.
    location.target = normalizeTarget(type, target);
    location.username =
        fields.value(QStringLiteral("username")).toString().trimmed();
    location.name = fields.value(QStringLiteral("name")).toString().trimmed();
    location.enabled = fields.value(QStringLiteral("enabled"), true).toBool();
    const QVariantMap options = fields.value(QStringLiteral("options")).toMap();
    for (auto it = options.cbegin(); it != options.cend(); ++it) {
        const QString value = it.value().toString().trimmed();
        if (!value.isEmpty()) location.options.insert(it.key(), value);
    }
    const QString password =
        fields.value(QStringLiteral("password")).toString();
    location.id = locationId(location.target, location.username);

    // Editing keeps the stored password when the field was left blank, so the
    // dialog never has to round-trip a secret it does not display.
    const QString editingId =
        fields.value(QStringLiteral("id")).toString().trimmed();
    const int editing = editingId.isEmpty() ? -1 : locationIndex(editingId);
    const int duplicate = locationIndex(location.id);
    if (duplicate >= 0 && duplicate != editing)
        return QStringLiteral("That server is already connected.");

    if (editing >= 0) {
        location.password =
            password.isEmpty() ? locations_[editing].password : password;
        locations_[editing] = location;
    } else {
        location.password = password;
        locations_.append(location);
    }
    locationStatuses_.remove(location.id);
    locationFileCounts_.remove(location.id);
    savePreferences();
    emit locationsChanged();
    scan();
    return {};
}

void TelemetryStore::removeLocation(const QString& id) {
    const int index = locationIndex(id);
    if (index < 0) return;
    // Disconnecting a server should not leave its downloads on the disk
    // forever: nothing can reach them again once the location is gone.
    const QString cache = cachePathFor(locations_[index]);
    if (!cache.isEmpty()) QDir(cache).removeRecursively();
    locations_.remove(index);
    locationStatuses_.remove(id);
    locationFileCounts_.remove(id);
    savePreferences();
    emit locationsChanged();
    scan();
}

void TelemetryStore::setLocationEnabled(const QString& id, bool enabled) {
    const int index = locationIndex(id);
    if (index < 0 || locations_[index].enabled == enabled) return;
    locations_[index].enabled = enabled;
    locationStatuses_.remove(id);
    locationFileCounts_.remove(id);
    savePreferences();
    emit locationsChanged();
    scan();
}

void TelemetryStore::setLocationName(const QString& id, const QString& name) {
    const int index = locationIndex(id);
    if (index < 0) return;
    const QString trimmed = name.trimmed();
    if (locations_[index].name == trimmed) return;
    locations_[index].name = trimmed;
    savePreferences();
    emit locationsChanged();
    // Folder display names feed the session tree, so refresh discovery too.
    scan();
}

void TelemetryStore::moveLocation(const QString& id, int delta) {
    const int index = locationIndex(id);
    if (index < 0 || delta == 0) return;
    const int target =
        std::clamp(index + delta, 0, static_cast<int>(locations_.size()) - 1);
    if (target == index) return;
    locations_.move(index, target);
    savePreferences();
    emit locationsChanged();
    scan();
}

void TelemetryStore::requestSidebarMetadata(const QString& path, bool visible) {
    const QString canonical = canonicalInputPath(path);
    if (canonical.isEmpty() || !visible) return;
    if (findSession(canonical) || sidebarMetadataLoaded_.contains(canonical) ||
        sidebarMetadataQueued_.contains(canonical) ||
        sidebarMetadataLoadingPath_ == canonical)
        return;
    sidebarMetadataQueue_.append(canonical);
    sidebarMetadataQueued_.insert(canonical);
    startNextSidebarMetadataLoad();
}

void TelemetryStore::pauseSidebarMetadataQueue() {
    sidebarMetadataQueuePaused_ = true;
}

void TelemetryStore::resumeSidebarMetadataQueue() {
    if (fileOpenLoading_ || primaryLapLoading_ || compareLapLoading_ ||
        !pendingFileOpens_.isEmpty())
        return;
    sidebarMetadataQueuePaused_ = false;
    startNextSidebarMetadataLoad();
}

void TelemetryStore::startNextSidebarMetadataLoad() {
    if (sidebarMetadataQueuePaused_ || fileOpenLoading_ || primaryLapLoading_ ||
        compareLapLoading_ || !sidebarMetadataLoadingPath_.isEmpty())
        return;
    while (!sidebarMetadataQueue_.isEmpty()) {
        const QString path = sidebarMetadataQueue_.takeFirst();
        sidebarMetadataQueued_.remove(path);
        if (findSession(path) || sidebarMetadataLoaded_.contains(path))
            continue;

        sidebarMetadataLoadingPath_ = path;
        const quint64 generation = sidebarMetadataGeneration_;
        const QString cachePath = sessionIndexCachePath();
        auto* watcher =
            new QFutureWatcher<std::shared_ptr<SidebarMetadataResult>>(this);
        connect(
            watcher,
            &QFutureWatcher<std::shared_ptr<SidebarMetadataResult>>::finished,
            this, [this, watcher, generation]() {
                const std::shared_ptr<SidebarMetadataResult> result =
                    watcher->result();
                watcher->deleteLater();
                if (generation != sidebarMetadataGeneration_) return;
                sidebarMetadataLoaded_.insert(result->path);
                fileMetadata_.insert(result->path, result->metadata);
                if (result->handle && !findSession(result->path))
                    sessions_.push_back(std::move(result->handle));
                sidebarMetadataLoadingPath_.clear();
                emit sidebarMetadataChanged(result->path,
                                            sidebarFileDetails(result->path));
                startNextSidebarMetadataLoad();
            });
        watcher->setFuture(
            QtConcurrent::run(&sidebarMetadataPool_, [path, cachePath]() {
                return loadSidebarMetadata(path, cachePath);
            }));
        return;
    }
}

void TelemetryStore::copyFilePath(const QString& path) const {
    const QString filePath = canonicalInputPath(path);
    if (filePath.isEmpty()) return;
    QGuiApplication::clipboard()->setText(filePath);
}

void TelemetryStore::openContainingFolder(const QString& path) const {
    const QString filePath = canonicalInputPath(path);
    if (filePath.isEmpty()) return;
    QDesktopServices::openUrl(
        QUrl::fromLocalFile(QFileInfo(filePath).absolutePath()));
}

int TelemetryStore::sidebarPinIndex(const QString& kind,
                                    const QString& path) const {
    for (int index = 0; index < sidebarPins_.size(); ++index) {
        const SidebarPin& pin = sidebarPins_.at(index);
        if (pin.kind == kind && pin.path == path) return index;
    }
    return -1;
}

bool TelemetryStore::filePinned(const QString& role,
                                const QString& path) const {
    const QString kind = sidebarPinKind(role, path);
    return !kind.isEmpty() &&
           sidebarPinIndex(kind, normalizedSidebarPinPath(path)) >= 0;
}

void TelemetryStore::setFilePinned(const QString& role, const QString& path,
                                   bool pinned) {
    const QString kind = sidebarPinKind(role, path);
    const QString normalizedPath = normalizedSidebarPinPath(path);
    if (kind.isEmpty() || normalizedPath.isEmpty()) return;

    const int existing = sidebarPinIndex(kind, normalizedPath);
    if (pinned && existing < 0) {
        sidebarPins_.prepend(SidebarPin{kind, normalizedPath});
    } else if (!pinned && existing >= 0) {
        sidebarPins_.removeAt(existing);
    } else {
        return;
    }
    savePreferences();
    emit filePinsChanged();
}

QVariantMap TelemetryStore::sidebarFileDetails(const QString& path) const {
    SessionHandle* session = findSession(path);
    const bool video = isVideoPath(path);
    QVariantMap metadata = fileMetadata_.value(path);
    omatrack::track_metadata::merge(&metadata, recordingMetadata_.value(path));
    const auto metadataOr = [&metadata](const QStringList& field,
                                        const QString& fallback) {
        const QString value = nestedText(metadata, field);
        return value.isEmpty() ? fallback : value;
    };
    QString date = session ? session->date() : QString();
    if (date == QStringLiteral("Unknown")) date.clear();
    return QVariantMap{
        {QStringLiteral("key"), session ? session->sessionKey() : QString()},
        {QStringLiteral("hasSession"), session != nullptr},
        {QStringLiteral("isVideo"), video},
        {QStringLiteral("driver"),
         session ? driverDisplay(session) : QString()},
        {QStringLiteral("mappingKey"),
         session ? session->driverMappingKey() : QString()},
        {QStringLiteral("bestTime"),
         session ? session->bestLapTime() : QString()},
        {QStringLiteral("topQuartileTime"),
         averageFastestQuartileTime(session)},
        {QStringLiteral("driveTime"), indexedDriveTime(session)},
        {QStringLiteral("lapCount"), indexedLapCount(session)},
        {QStringLiteral("carClass"),
         metadataOr({QStringLiteral("car"), QStringLiteral("class")},
                    session ? session->carClass() : QString())},
        {QStringLiteral("seriesName"),
         nestedText(metadata, {QStringLiteral("series")})},
        {QStringLiteral("sessionDate"), date},
    };
}

QVariantList TelemetryStore::fileSources() const {
    auto enrichNode = [this](auto&& self,
                             const QVariantMap& sourceNode) -> QVariantMap {
        QVariantMap node = sourceNode;
        const QString role = node.value(QStringLiteral("role")).toString();
        const QString path = node.value(QStringLiteral("path")).toString();
        node.insert(QStringLiteral("pinned"), filePinned(role, path));
        if (role == QStringLiteral("source") ||
            role == QStringLiteral("folder")) {
            const QString displayName =
                folderDisplayNames_.value(normalizedSidebarPinPath(path));
            if (!displayName.isEmpty())
                node.insert(QStringLiteral("name"), displayName);
        }
        if (role == QStringLiteral("file")) {
            const QVariantMap details = sidebarFileDetails(path);
            for (auto it = details.cbegin(); it != details.cend(); ++it)
                node.insert(it.key(), it.value());
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
    sources.reserve(fileSources_.size() + 1);
    for (const QVariant& source : fileSources_)
        sources.append(enrichNode(enrichNode, source.toMap()));

    auto findPinnedNode = [](auto&& self, const QVariantMap& node,
                             const SidebarPin& pin) -> QVariantMap {
        const QString role = node.value(QStringLiteral("role")).toString();
        const QString path = normalizedSidebarPinPath(
            node.value(QStringLiteral("path")).toString());
        const bool roleMatches =
            pin.kind == QStringLiteral("folder")
                ? role == QStringLiteral("source") ||
                      role == QStringLiteral("folder")
                : role == QStringLiteral("file") &&
                      node.value(QStringLiteral("isVideo")).toBool();
        if (roleMatches && path == pin.path) return node;
        for (const QVariant& child :
             node.value(QStringLiteral("children")).toList()) {
            const QVariantMap match = self(self, child.toMap(), pin);
            if (!match.isEmpty()) return match;
        }
        return {};
    };

    QVariantList pinnedChildren;
    pinnedChildren.reserve(sidebarPins_.size());
    for (const SidebarPin& pin : sidebarPins_) {
        for (const QVariant& source : sources) {
            QVariantMap match =
                findPinnedNode(findPinnedNode, source.toMap(), pin);
            if (match.isEmpty()) continue;
            if (match.value(QStringLiteral("role")).toString() ==
                QStringLiteral("source"))
                match.insert(QStringLiteral("role"), QStringLiteral("folder"));
            pinnedChildren.append(match);
            break;
        }
    }
    if (!pinnedChildren.isEmpty()) {
        sources.prepend(QVariantMap{
            {QStringLiteral("role"), QStringLiteral("pins")},
            {QStringLiteral("name"), QStringLiteral("Pinned")},
            {QStringLiteral("path"), QStringLiteral("sidebar-pins")},
            {QStringLiteral("available"), true},
            {QStringLiteral("fileCount"), pinnedChildren.size()},
            {QStringLiteral("pinned"), false},
            {QStringLiteral("children"), pinnedChildren},
        });
    }
    return sources;
}

bool TelemetryStore::directoryExists(const QString& dirPath) const {
    return !dirPath.isEmpty() && QFileInfo(dirPath).isDir();
}

QString TelemetryStore::defaultTelemetryDirectory() const {
    // QStandardPaths::DocumentsLocation is the OS-correct location on every
    // platform and follows the user's OneDrive redirection on Windows; fall
    // back to home/Documents when the platform reports no Documents folder.
    QString documents =
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (documents.isEmpty())
        documents = QDir::homePath() + QStringLiteral("/Documents");
    const QString directory = documents + QStringLiteral("/Telemetry");
    QDir().mkpath(directory);
    return directory;
}

// True when the active lap carries usable GPS: the damper-alignment tool is
// only the fallback for sessions that cannot be aligned positionally.
bool TelemetryStore::hasGpsData() const {
    const omatrack::UnifiedLap* lap = primaryUnified();
    return lap && omatrack::trackatlas::hasPositionalGps(*lap);
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
    QStringList descendantPaths;
    for (const QString& path : discoveredFilePaths_) {
        const QString relative = QDir(canonical).relativeFilePath(path);
        if (relative == QStringLiteral("..") ||
            relative.startsWith(QStringLiteral("../")))
            continue;
        descendantPaths.append(path);
    }
    const int metadataSourceCount = descendantPaths.size();
    const int directRecordingCount = int(
        std::count_if(descendantPaths.cbegin(), descendantPaths.cend(),
                      [&canonical](const QString& path) {
                          return QFileInfo(path).absolutePath() == canonical;
                      }));
    int sourceRecordingCount = 0;
    for (const auto& session : sessions_) {
        if (!session) continue;
        const QString relative =
            QDir(canonical).relativeFilePath(session->path());
        if (relative == QStringLiteral("..") ||
            relative.startsWith(QStringLiteral("../")))
            continue;

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

        if (session->sourceChannels().isEmpty()) continue;
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
    const auto sampled = folderChannelSamples_.constFind(canonical);
    int channelSampleCandidateCount = metadataSourceCount;
    bool channelSampleLoading =
        folderChannelSampleRequests_.contains(canonical);
    if (sampled != folderChannelSamples_.cend() && *sampled) {
        channelSampleCandidateCount = (*sampled)->candidateCount;
        sourceRecordingCount = (*sampled)->recordingCount;
        aggregatedChannels.clear();
        for (const SourceChannelSummary& source : (*sampled)->channels)
            aggregatedChannels.insert(source.name.toCaseFolded(), source);
        automaticMappingCounts.clear();
        automaticMappingValues.clear();
        for (auto it = (*sampled)->automaticMappings.cbegin();
             it != (*sampled)->automaticMappings.cend(); ++it) {
            automaticMappingCounts[it.key()][it.value().toCaseFolded()] = 1;
            automaticMappingValues[it.key()][it.value().toCaseFolded()] =
                it.value();
        }
        channelSampleLoading = false;
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
        {QStringLiteral("folderName"),
         nestedText(metadata,
                    {QStringLiteral("folder"), QStringLiteral("name")})},
        {QStringLiteral("folder"), canonical},
        {QStringLiteral("sidecarPath"), targetSidecarPath},
        {QStringLiteral("inheritedSidecarPath"),
         inheritedPaths.isEmpty() ? QString() : inheritedPaths.constLast()},
        {QStringLiteral("trackFileCount"), trackFiles.size()},
        {QStringLiteral("sourceChannelCount"), sourceChannels.size()},
        {QStringLiteral("sourceRecordingCount"), sourceRecordingCount},
        {QStringLiteral("directRecordingCount"), directRecordingCount},
        {QStringLiteral("channelSampleCandidateCount"),
         channelSampleCandidateCount},
        {QStringLiteral("channelSampleLoading"), channelSampleLoading},
        {QStringLiteral("channelSampleComplete"),
         sampled != folderChannelSamples_.cend()},
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

void TelemetryStore::sampleFolderChannels(const QString& folderPath) {
    const QString canonical = canonicalDirectoryPath(folderPath);
    if (canonical.isEmpty()) return;
    if (folderChannelSamples_.contains(canonical)) {
        emit folderChannelSampleReady(folderMetadata(canonical));
        return;
    }
    if (folderChannelSampleRequests_.contains(canonical)) return;

    QStringList candidates;
    for (const QString& path : discoveredFilePaths_) {
        const QString relative = QDir(canonical).relativeFilePath(path);
        if (relative == QStringLiteral("..") ||
            relative.startsWith(QStringLiteral("../")))
            continue;
        candidates.append(path);
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const QString& left, const QString& right) {
                  const QFileInfo leftInfo(left);
                  const QFileInfo rightInfo(right);
                  if (leftInfo.lastModified() != rightInfo.lastModified())
                      return leftInfo.lastModified() > rightInfo.lastModified();
                  return left.compare(right, Qt::CaseInsensitive) < 0;
              });
    const int candidateCount = candidates.size();
    constexpr int kFolderChannelSampleLimit = 8;
    if (candidates.size() > kFolderChannelSampleLimit)
        candidates.resize(kFolderChannelSampleLimit);

    folderChannelSampleRequests_.insert(canonical);
    const quint64 generation = folderChannelSampleGeneration_;
    auto* watcher =
        new QFutureWatcher<std::shared_ptr<FolderChannelSample>>(this);
    connect(watcher,
            &QFutureWatcher<std::shared_ptr<FolderChannelSample>>::finished,
            this, [this, watcher, canonical, generation]() {
                const std::shared_ptr<FolderChannelSample> sample =
                    watcher->result();
                watcher->deleteLater();
                if (generation != folderChannelSampleGeneration_) return;
                folderChannelSampleRequests_.remove(canonical);
                folderChannelSamples_.insert(canonical, sample);
                emit folderChannelSampleReady(folderMetadata(canonical));
            });
    watcher->setFuture(QtConcurrent::run(
        &sidebarMetadataPool_, [candidates, candidateCount]() {
            auto result = std::make_shared<FolderChannelSample>();
            result->candidateCount = candidateCount;
            QHash<QString, QHash<QString, int>> mappingCounts;
            QHash<QString, QHash<QString, QString>> mappingValues;
            for (const QString& path : candidates) {
                SessionHandle sampledSession(path);
                if (!sampledSession.loadChannelSummaryForIndex()) continue;
                mergeChannelSample(result.get(), sampledSession);
                for (auto it =
                         sampledSession.automaticChannelMappings().cbegin();
                     it != sampledSession.automaticChannelMappings().cend();
                     ++it) {
                    const QString key = it.value().toCaseFolded();
                    ++mappingCounts[it.key()][key];
                    mappingValues[it.key()][key] = it.value();
                }
            }
            for (auto field = mappingCounts.cbegin();
                 field != mappingCounts.cend(); ++field) {
                QString selected;
                int selectedCount = 0;
                for (auto candidate = field.value().cbegin();
                     candidate != field.value().cend(); ++candidate) {
                    const QString value =
                        mappingValues[field.key()].value(candidate.key());
                    if (candidate.value() > selectedCount ||
                        (candidate.value() == selectedCount &&
                         value.compare(selected, Qt::CaseInsensitive) < 0)) {
                        selected = value;
                        selectedCount = candidate.value();
                    }
                }
                if (!selected.isEmpty())
                    result->automaticMappings.insert(field.key(), selected);
            }
            return result;
        }));
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
    const QString displayName = nestedText(
        document, {QStringLiteral("folder"), QStringLiteral("name")});
    const QString requestedPath = normalizedSidebarPinPath(folderPath);
    const QString canonicalPath = normalizedSidebarPinPath(canonical);
    if (displayName.isEmpty()) {
        folderDisplayNames_.remove(requestedPath);
        folderDisplayNames_.remove(canonicalPath);
    } else {
        folderDisplayNames_.insert(requestedPath, displayName);
        folderDisplayNames_.insert(canonicalPath, displayName);
    }
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

QVector<CornerZone> TelemetryStore::atlasCornersForPrimary() {
    QVector<CornerZone> result;
    if (!primarySession_ || primaryLap_ < 0 || atlasTracks_.isEmpty())
        return result;

    const QString assignedSlug = resolvedTrackSlug(primarySession_);
    const QString wanted = normalizeAtlasName(
        assignedSlug.isEmpty() ? primarySession_->track() : assignedSlug);
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
                emit trackAtlasChanged();
            }
            return result;
        }
        const QString mappingKey =
            primarySession_->sessionKey() + QLatin1Char('|') +
            QString::number(primaryLap_) + QLatin1Char('|') + geometryKey;
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
            emit trackAtlasChanged();
            return result;
        }
        trackAtlasStatus_ =
            QStringLiteral("%1 tracks cached · GPS-mapped corners")
                .arg(atlasTracks_.size());
        emit trackAtlasChanged();
    } else {
        trackAtlasStatus_ =
            QStringLiteral("%1 tracks cached · distance fallback (no GPS)")
                .arg(atlasTracks_.size());
        emit trackAtlasChanged();
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

void TelemetryStore::selectSession(const QString& sessionKey, bool compare) {
    SessionHandle* session = findSession(sessionKey);
    if (!session) {
        resumeSidebarMetadataQueue();
        emit operationError(
            QStringLiteral("Unable to select session"),
            QStringLiteral("The selected session is no longer available."));
        return;
    }

    const LapEntry* lap = bestLap(*session);
    if (!lap && !session->laps().isEmpty()) lap = &session->laps().constFirst();
    if (lap) {
        requestLapLoad(session, lap->lapId, compare);
        return;
    }
    if (session->isVideo()) {
        if (!compare && compareSession_ == session) clearCompare();
        queueFileOpen(session->path(),
                      compare ? FileOpenRole::Compare : FileOpenRole::Primary);
        return;
    }
    emit operationError(QStringLiteral("Unable to select session"),
                        QStringLiteral("%1 does not contain a selectable lap.")
                            .arg(QFileInfo(session->path()).fileName()));
}

void TelemetryStore::requestLapLoad(SessionHandle* session, int lapId,
                                    bool compare) {
    if (!session || lapId < 0) {
        emit operationError(QStringLiteral("Unable to load lap"),
                            QStringLiteral("The selected lap is invalid."));
        return;
    }
    pauseSidebarMetadataQueue();
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
        resumeSidebarMetadataQueue();
        return;
    }

    const LapEntry* wanted = nullptr;
    for (const LapEntry& lap : session->laps()) {
        if (lap.lapId == lapId) {
            wanted = &lap;
            break;
        }
    }
    if (!wanted) {
        emit operationError(
            QStringLiteral("Unable to load lap"),
            QStringLiteral("The selected lap is no longer available."));
        resumeSidebarMetadataQueue();
        return;
    }

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
            if (generation != currentGeneration) {
                resumeSidebarMetadataQueue();
                return;
            }

            SessionHandle* session = findSession(result->sessionKey);
            if (!session || !result->error.isEmpty() || !result->source ||
                !result->unified) {
                qWarning() << "Unable to load lap" << result->sessionKey
                           << result->lapId << result->error;
                if (compare)
                    setCompareLapLoading(false);
                else
                    setPrimaryLapLoading(false);
                emit operationError(
                    QStringLiteral("Unable to load lap"),
                    result->error.isEmpty()
                        ? QStringLiteral(
                              "The selected lap could not be loaded.")
                        : result->error);
                resumeSidebarMetadataQueue();
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
            resumeSidebarMetadataQueue();
        });
    watcher->setFuture(QtConcurrent::run([path, sessionKey, lap, metadata]() {
        return loadSessionLap(path, sessionKey, lap, metadata);
    }));
}

void TelemetryStore::setPrimary(SessionHandle* session, int lapId) {
    const bool sessionChanged = primarySession_ != session;
    if (sessionChanged && primarySession_ && primarySession_ != session &&
        primarySession_ != compareSession_)
        primarySession_->clearUnifiedCache();
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
    if (sessionChanged && compareSession_ && compareSession_ != session &&
        compareSession_ != primarySession_)
        compareSession_->clearUnifiedCache();
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
    if (!session) {
        resumeSidebarMetadataQueue();
        emit operationError(
            QStringLiteral("Unable to select lap"),
            QStringLiteral("The selected session is no longer available."));
        return;
    }
    if (primarySession_ == session && primaryLap_ == lapId) {
        resumeSidebarMetadataQueue();
        return;
    }
    requestLapLoad(session, lapId, false);
}

void TelemetryStore::compareLap(const QString& sessionKey, int lapId) {
    SessionHandle* session = findSession(sessionKey);
    if (!session) {
        emit operationError(
            QStringLiteral("Unable to select reference"),
            QStringLiteral("The selected session is no longer available."));
        return;
    }
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
bool TelemetryStore::traceConfidenceIncludesLap(const QString& sessionKey,
                                                int lapId) const {
    return primarySession_ && sessionKey == primarySession_->sessionKey() &&
           traceConfidenceLapIds_.contains(lapId);
}

// ── navigation ──────────────────────────────────────────────────────

void TelemetryStore::setCursorFrac(double v) {
    v = qBound(0.0, v, 1.0);
    if (qFuzzyCompare(v, cursorFrac_)) return;
    cursorFrac_ = v;
    emit cursorFracChanged();
    emit videoTimeChanged();
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
    const UnifiedLap* unified = primaryUnified();
    if (!unified || unified->time.size() < 2) return;
    for (const LapEntry& lap : primarySession_->laps()) {
        if (lap.lapId != primaryLap_) continue;
        const double presentationOffset =
            primarySession_->videoPresentationOffsetSec().value_or(0.0);
        const double relativeTime =
            mediaTime - presentationOffset - lap.startTime;
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
        // Pickup is the exit event: the search starts at the throttle
        // minimum, so a corner entered with partial throttle cannot report
        // its own entry as the application point.
        int pickupStart = first;
        double pickupFloor = std::numeric_limits<double>::infinity();
        for (int i = first; i <= finish && i < int(lap.throttle.size()); ++i) {
            if (lap.throttle[i] < pickupFloor) {
                pickupFloor = lap.throttle[i];
                pickupStart = i;
            }
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
        auto windowPosition = [&](double distanceFromCornerStart) {
            return (startDistance + distanceFromCornerStart - windowStart) /
                   windowMeters;
        };
        const double turnInPosition = windowPosition(turnInPoint);
        const double apexPosition = windowPosition(apexPoint);
        const double throttlePosition = windowPosition(throttlePoint);
        const double cornerStartPosition = windowPosition(0.0);
        const double cornerEndPosition = windowPosition(cornerLength);

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
            {"apexFraction", last > 0 ? double(apexIndex) / double(last) : 0.0},
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
    const QVector<double>& delta = deltaTrace();
    auto deltaAt = [&delta](double fraction) {
        if (delta.isEmpty()) return 0.0;
        const double position =
            qBound(0.0, fraction, 1.0) * double(delta.size() - 1);
        const int lo =
            std::clamp(int(std::floor(position)), 0, int(delta.size()) - 1);
        const int hi = std::min(lo + 1, int(delta.size()) - 1);
        return delta[lo] + (delta[hi] - delta[lo]) * (position - lo);
    };

    for (const CornerZone& corner : corners_) {
        const QVariantMap primaryStats = stats(*primary, corner);
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
            {"apexFraction", primaryStats.value("apexFraction")},
            {"hasCompare", compare != nullptr}};

        omatrack::CornerContext context;
        context.primary = primary;
        context.primaryMetrics =
            omatrack::measureCorner(*primary, corner.start, corner.end);

        if (compare) {
            const double startDistance =
                sample(primary->distance, corner.start);
            const double endDistance = sample(primary->distance, corner.end);
            CornerZone compareCorner = corner;
            compareCorner.start = fractionAtDistance(*compare, startDistance);
            compareCorner.end = fractionAtDistance(*compare, endDistance);
            const QVariantMap compareStats = stats(*compare, compareCorner);
            // The reference apex, expressed on the primary lap's distance
            // axis, so both markers land on the same zoomed viewport.
            row.insert(
                QStringLiteral("compareApexFraction"),
                fractionAtDistance(
                    *primary, startDistance +
                                  compareStats.value("apexPoint").toDouble()));

            // Time through the corner comes from the one cached delta trace
            // whenever it exists, so the panel, the Δ lane and the cursor
            // readout cannot disagree. Raw corner times are the fallback for
            // laps the track-station map could not align.
            const double timeDelta =
                delta.isEmpty() ? primaryStats.value("time").toDouble() -
                                      compareStats.value("time").toDouble()
                                : deltaAt(corner.end) - deltaAt(corner.start);
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
            // Entry and exit time come from the one cached delta trace, so
            // the corner overlay can never disagree with the Δ lane.
            const double apexFraction =
                primaryStats.value("apexFraction").toDouble();
            const double entryTimeDelta =
                deltaAt(apexFraction) - deltaAt(corner.start);
            const double exitTimeDelta =
                deltaAt(corner.end) - deltaAt(apexFraction);
            const double score = qBound(
                0.0,
                50.0 - timeDelta * 40.0 + exitDelta * 0.8 + apexDelta * 0.35,
                100.0);

            context.reference = compare;
            context.referenceMetrics = omatrack::measureCorner(
                *compare, compareCorner.start, compareCorner.end);
            context.timeDelta = timeDelta;
            context.entryTimeDelta = entryTimeDelta;
            context.exitTimeDelta = exitTimeDelta;

            row.insert("compareEntrySpeed", compareStats.value("entrySpeed"));
            row.insert("compareApexSpeed", compareStats.value("apexSpeed"));
            row.insert("compareExitSpeed", compareStats.value("exitSpeed"));
            row.insert("compareTime",
                       delta.isEmpty()
                           ? compareStats.value("time")
                           : primaryStats.value("time").toDouble() - timeDelta);
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
            row.insert("entryTimeDelta", entryTimeDelta);
            row.insert("exitTimeDelta", exitTimeDelta);
            row.insert("entryDelta", entryDelta);
            row.insert("apexDelta", apexDelta);
            row.insert("exitDelta", exitDelta);
            row.insert("brakePointDelta", brakePointDelta);
            row.insert("liftPointDelta", liftPointDelta);
            row.insert("turnInDelta", turnInDelta);
            row.insert("apexPointDelta", apexPointDelta);
            row.insert("throttlePointDelta", throttlePointDelta);
            row.insert("score", score);
        }
        const std::vector<omatrack::CornerNote> cornerNotes =
            omatrack::CornerAnalysisRegistry::instance().run(context);
        QVariantList notesList;
        QStringList noteParts;
        for (const omatrack::CornerNote& n : cornerNotes) {
            notesList.append(QVariantMap{
                {"id", QString::fromStdString(n.id)},
                {"text", QString::fromStdString(n.text)},
                {"severity",
                 QString::fromLatin1(omatrack::severityName(n.severity))},
            });
            noteParts << QString::fromStdString(n.text);
        }
        if (cornerNotes.empty() && compare) {
            const QString matched = QStringLiteral("Closely matched");
            notesList.append(QVariantMap{
                {"id", QStringLiteral("matched")},
                {"text", matched},
                {"severity", QString::fromLatin1(omatrack::severityName(
                                 omatrack::NoteSeverity::Info))},
            });
            noteParts << matched;
        }
        row.insert("notes", notesList);
        row.insert("note", noteParts.join(QStringLiteral(" · ")));

        out.append(row);
    }
    return out;
}

// ── corner focus ────────────────────────────────────────────────────

void TelemetryStore::resetView() {
    if (focusedCorner_ >= 0) {
        focusedCorner_ = -1;
        markers_.clear();
        ++cornerConsistencyGeneration_;
        cornerConsistency_ = {};
        emit cornerConsistencyChanged();
        emit cornerFocusChanged();
    }
    focusReturnStart_ = 0.0;
    focusReturnEnd_ = 1.0;
    if (viewStart_ == 0.0 && viewEnd_ == 1.0) return;
    viewStart_ = 0.0;
    viewEnd_ = 1.0;
    emit viewChanged();
}

void TelemetryStore::focusCorner(int index) {
    if (index < 0 || index >= corners_.size()) return;
    if (focusedCorner_ < 0) {
        focusReturnStart_ = viewStart_;
        focusReturnEnd_ = viewEnd_;
    }
    focusedCorner_ = index;

    // The corner sits in the middle of the left half of the workspace: the
    // information overlay owns the right side, and the approach and exit
    // context stays visible on either side of the zone.
    constexpr double kZoneShareOfView = 0.30;
    constexpr double kCornerCentre = 0.25;
    const CornerZone& corner = corners_[index];
    const double zone = std::max(0.002, corner.end - corner.start);
    const double span = qBound(0.004, zone / kZoneShareOfView, 1.0);
    // Deliberately unclamped: a corner near start/finish keeps its place in
    // the left half and the viewport runs off the end of the lap. The
    // renderer fills that space with the neighbouring lap, masked, or with
    // black when there is no more data to show.
    const double start = corner.mid() - kCornerCentre * span;
    viewStart_ = start;
    viewEnd_ = start + span;
    emit viewChanged();
    prefetchNeighbourLaps();

    rebuildCornerMarkers();
    requestCornerConsistency();
    for (const CornerMarker& marker : markers_)
        if (marker.key == QLatin1String("apex")) {
            setCursorFrac(marker.fraction);
            break;
        }
}

// ── neighbouring laps ───────────────────────────────────────────────

int TelemetryStore::neighbourLapId(int offset) const {
    if (!primarySession_ || primaryLap_ < 0) return -1;
    const QVector<LapEntry>& laps = primarySession_->laps();
    int index = -1;
    for (int i = 0; i < laps.size(); ++i)
        if (laps[i].lapId == primaryLap_) {
            index = i;
            break;
        }
    if (index < 0) return -1;
    const int neighbour = index + offset;
    if (neighbour < 0 || neighbour >= laps.size()) return -1;
    return laps[neighbour].lapId;
}

const UnifiedLap* TelemetryStore::neighbourUnified(int offset) const {
    const int lapId = neighbourLapId(offset);
    if (lapId < 0 || !primarySession_) return nullptr;
    // Cache only: the renderer asks for this inside a frame.
    const std::shared_ptr<const UnifiedLap> lap =
        primarySession_->unifiedLap(lapId);
    return lap && lap->size() > 1 ? lap.get() : nullptr;
}

QString TelemetryStore::neighbourLabel(int offset) const {
    if (!neighbourUnified(offset)) return QString();
    const int lapId = neighbourLapId(offset);
    for (const LapEntry& lap : primarySession_->laps())
        if (lap.lapId == lapId)
            return lap.label.isEmpty() ? QStringLiteral("lap %1").arg(lapId)
                                       : lap.label;
    return QString();
}

// Corner focus can frame the viewport past the lap bounds, so the laps on
// either side are worth having. They load on the worker pool and appear when
// they arrive; until then the renderer shows black.
void TelemetryStore::prefetchNeighbourLaps() {
    if (!primarySession_) return;
    const QString sessionKey = primarySession_->sessionKey();
    const QString path = primarySession_->path();
    const QVariantMap metadata =
        recordingMetadataForPath(path, recordingMetadata_);

    for (const int offset : {-1, 1}) {
        const int lapId = neighbourLapId(offset);
        if (lapId < 0) continue;
        if (primarySession_->unifiedLap(lapId)) continue;
        const QString token =
            sessionKey + QLatin1Char('#') + QString::number(lapId);
        if (neighbourPrefetch_.contains(token)) continue;

        const LapEntry* wanted = nullptr;
        for (const LapEntry& lap : primarySession_->laps())
            if (lap.lapId == lapId) {
                wanted = &lap;
                break;
            }
        if (!wanted) continue;
        neighbourPrefetch_.insert(token);

        const LapEntry lap = *wanted;
        auto* watcher =
            new QFutureWatcher<std::shared_ptr<SessionLapLoadResult>>(this);
        connect(
            watcher,
            &QFutureWatcher<std::shared_ptr<SessionLapLoadResult>>::finished,
            this, [this, watcher, token]() {
                std::shared_ptr<SessionLapLoadResult> result =
                    watcher->result();
                watcher->deleteLater();
                neighbourPrefetch_.remove(token);
                if (!result || !result->error.isEmpty() || !result->source ||
                    !result->unified)
                    return;
                SessionHandle* session = findSession(result->sessionKey);
                if (!session) return;
                session->adoptLoadedLap(
                    result->lapId, std::move(result->source),
                    std::move(result->unified), result->driverId,
                    result->forceDriverId);
                // Only the viewport needs to know: this lap is context, not
                // a selection change.
                if (session == primarySession_) emit viewChanged();
            });
        watcher->setFuture(
            QtConcurrent::run([path, sessionKey, lap, metadata]() {
                return loadSessionLap(path, sessionKey, lap, metadata);
            }));
    }
}

void TelemetryStore::focusCornerAtCursor() {
    if (corners_.isEmpty()) return;
    int index = 0;
    for (int i = 0; i < corners_.size(); ++i)
        if (corners_[i].start <= cursorFrac_ &&
            cursorFrac_ <= corners_[i].end) {
            index = i;
            break;
        }
    focusCorner(index);
}

void TelemetryStore::clearCornerFocus() {
    if (focusedCorner_ < 0) return;
    focusedCorner_ = -1;
    markers_.clear();
    ++cornerConsistencyGeneration_;
    cornerConsistency_ = {};
    emit cornerConsistencyChanged();
    viewStart_ = focusReturnStart_;
    viewEnd_ = focusReturnEnd_;
    emit viewChanged();
    emit cornerFocusChanged();
}

QVariantMap TelemetryStore::cornerFocusSummary() const {
    if (focusedCorner_ < 0) return {};
    const QVariantList rows = cornerComparison();
    if (focusedCorner_ >= rows.size()) return {};

    QVariantMap row = rows[focusedCorner_].toMap();
    row.insert(QStringLiteral("consistencyLoading"),
               cornerConsistency_.loading);
    row.insert(QStringLiteral("consistencyLapCount"),
               cornerConsistency_.lapCount);
    row.insert(QStringLiteral("consistencyValidLapCount"),
               cornerConsistency_.validLapCount);
    row.insert(QStringLiteral("consistencyBrakeLapCount"),
               cornerConsistency_.brakingLapCount);
    const bool available = cornerConsistency_.brakingLapCount >= 2 &&
                           std::isfinite(cornerConsistency_.medianBrakePoint) &&
                           std::isfinite(cornerConsistency_.brakePointStdDev) &&
                           std::isfinite(cornerConsistency_.brakePointRange);
    row.insert(QStringLiteral("brakeConsistencyAvailable"), available);
    if (available) {
        row.insert(QStringLiteral("brakePointMedian"),
                   cornerConsistency_.medianBrakePoint);
        row.insert(QStringLiteral("brakePointStdDev"),
                   cornerConsistency_.brakePointStdDev);
        row.insert(QStringLiteral("brakePointRange"),
                   cornerConsistency_.brakePointRange);
        if (row.value(QStringLiteral("maxBrake")).toDouble() > 2.0)
            row.insert(QStringLiteral("brakePointVsMedian"),
                       row.value(QStringLiteral("brakePoint")).toDouble() -
                           cornerConsistency_.medianBrakePoint);
    }
    return row;
}

void TelemetryStore::setTraceConfidenceMode(bool enabled) {
    if (traceConfidenceMode_ == enabled) return;
    traceConfidenceMode_ = enabled;
    emit traceConfidenceChanged();
    if (enabled) requestTraceConfidence();
}

const TraceConfidenceBand* TelemetryStore::traceConfidenceBand(
    const QString& field) const {
    const auto band = traceConfidenceBands_.constFind(field);
    return band == traceConfidenceBands_.cend() ? nullptr : &band.value();
}

void TelemetryStore::invalidateTraceConfidence() {
    const QString key = primarySession_ && primaryLap_ >= 0
                            ? primarySession_->sessionKey() + QLatin1Char('#') +
                                  QString::number(primaryLap_)
                            : QString();
    if (traceConfidenceKey_ == key) return;
    ++traceConfidenceGeneration_;
    traceConfidenceKey_ = key;
    traceConfidenceBands_.clear();
    traceConfidenceLapIds_.clear();
    traceConfidenceLapCount_ = 0;
    traceConfidenceLoading_ = false;
    traceConfidenceReady_ = false;
    emit traceConfidenceChanged();
}

void TelemetryStore::requestTraceConfidence() {
    const std::shared_ptr<const UnifiedLap> primary =
        primarySession_ && primaryLap_ >= 0
            ? primarySession_->unifiedLap(primaryLap_)
            : nullptr;
    if (!primarySession_ || !primary || primary->size() < 3) return;

    const QString key = primarySession_->sessionKey() + QLatin1Char('#') +
                        QString::number(primaryLap_);
    if (traceConfidenceKey_ != key) invalidateTraceConfidence();
    if (traceConfidenceLoading_ || traceConfidenceReady_) return;

    QVector<LapEntry> ranked;
    for (const LapEntry& lap : primarySession_->laps())
        if (lap.countsForBest() && std::isfinite(lap.timeMs) &&
            lap.timeMs > 0.0)
            ranked.append(lap);
    std::sort(ranked.begin(), ranked.end(),
              [](const LapEntry& left, const LapEntry& right) {
                  return left.timeMs < right.timeMs;
              });
    if (!ranked.isEmpty()) ranked.resize((ranked.size() + 1) / 2);
    ranked.erase(std::remove_if(ranked.begin(), ranked.end(),
                                [this](const LapEntry& lap) {
                                    return lap.lapId == primaryLap_;
                                }),
                 ranked.end());
    traceConfidenceLapIds_.clear();
    for (const LapEntry& lap : ranked) traceConfidenceLapIds_.insert(lap.lapId);

    traceConfidenceLapCount_ = ranked.size();
    if (ranked.size() < 2) {
        traceConfidenceReady_ = true;
        emit traceConfidenceChanged();
        return;
    }

    const quint64 generation = ++traceConfidenceGeneration_;
    traceConfidenceLoading_ = true;
    emit traceConfidenceChanged();
    const QString path = primarySession_->path();
    const QVariantMap metadata =
        recordingMetadataForPath(path, recordingMetadata_);
    auto* watcher =
        new QFutureWatcher<std::shared_ptr<SessionConfidenceLoadResult>>(this);
    connect(
        watcher,
        &QFutureWatcher<std::shared_ptr<SessionConfidenceLoadResult>>::finished,
        this, [this, watcher, generation, key]() {
            const std::shared_ptr<SessionConfidenceLoadResult> result =
                watcher->result();
            watcher->deleteLater();
            if (generation != traceConfidenceGeneration_ ||
                traceConfidenceKey_ != key || !result)
                return;

            traceConfidenceLoading_ = false;
            traceConfidenceReady_ = true;
            traceConfidenceLapCount_ = result->lapCount;
            traceConfidenceBands_ = std::move(result->bands);
            if (!result->error.isEmpty())
                qWarning() << "Unable to build session trace confidence"
                           << result->error;
            emit traceConfidenceChanged();
        });
    watcher->setFuture(
        QtConcurrent::run([path, key, ranked, metadata, primary]() {
            return loadSessionConfidence(path, key, ranked, metadata, primary);
        }));
}

void TelemetryStore::requestCornerConsistency() {
    const UnifiedLap* primary = primaryUnified();
    if (focusedCorner_ < 0 || focusedCorner_ >= corners_.size() ||
        !primarySession_ || !primary || primary->distance.size() < 2) {
        ++cornerConsistencyGeneration_;
        cornerConsistency_ = {};
        emit cornerConsistencyChanged();
        return;
    }

    const CornerZone& corner = corners_[focusedCorner_];
    const auto sampleDistance = [primary](double fraction) {
        const std::vector<double>& distance = primary->distance;
        const double position =
            qBound(0.0, fraction, 1.0) * double(distance.size() - 1);
        const int lo =
            std::clamp(int(std::floor(position)), 0, int(distance.size()) - 1);
        const int hi = std::min(lo + 1, int(distance.size()) - 1);
        return distance[size_t(lo)] +
               (distance[size_t(hi)] - distance[size_t(lo)]) * (position - lo);
    };
    const double startDistance = sampleDistance(corner.start);
    const double endDistance = sampleDistance(corner.end);
    const QString sessionKey = primarySession_->sessionKey();
    const QString key = sessionKey + QLatin1Char('#') +
                        QString::number(startDistance, 'f', 3) +
                        QLatin1Char(':') + QString::number(endDistance, 'f', 3);
    if (cornerConsistency_.key == key) return;

    QVector<LapEntry> ranked;
    for (const LapEntry& lap : primarySession_->laps())
        if (lap.countsForBest() && std::isfinite(lap.timeMs) &&
            lap.timeMs > 0.0)
            ranked.append(lap);
    std::sort(ranked.begin(), ranked.end(),
              [](const LapEntry& left, const LapEntry& right) {
                  return left.timeMs < right.timeMs;
              });
    if (!ranked.isEmpty()) ranked.resize((ranked.size() + 3) / 4);

    const quint64 generation = ++cornerConsistencyGeneration_;
    cornerConsistency_ = {};
    cornerConsistency_.key = key;
    cornerConsistency_.loading = !ranked.isEmpty();
    cornerConsistency_.lapCount = ranked.size();
    emit cornerConsistencyChanged();
    if (ranked.isEmpty()) return;

    const QString path = primarySession_->path();
    const QVariantMap metadata =
        recordingMetadataForPath(path, recordingMetadata_);
    auto* watcher =
        new QFutureWatcher<std::shared_ptr<CornerConsistencyLoadResult>>(this);
    connect(
        watcher,
        &QFutureWatcher<std::shared_ptr<CornerConsistencyLoadResult>>::finished,
        this, [this, watcher, generation, key]() {
            const std::shared_ptr<CornerConsistencyLoadResult> result =
                watcher->result();
            watcher->deleteLater();
            if (generation != cornerConsistencyGeneration_ ||
                cornerConsistency_.key != key || !result)
                return;

            cornerConsistency_.loading = false;
            cornerConsistency_.lapCount = result->lapCount;
            cornerConsistency_.validLapCount = result->validLapCount;
            cornerConsistency_.brakingLapCount =
                int(result->brakePoints.size());
            if (!result->error.isEmpty())
                qWarning() << "Unable to measure corner consistency"
                           << result->sessionKey << result->error;

            std::vector<double> points = result->brakePoints;
            if (!points.empty()) {
                std::sort(points.begin(), points.end());
                const size_t middle = points.size() / 2;
                cornerConsistency_.medianBrakePoint =
                    points.size() % 2 == 0
                        ? (points[middle - 1] + points[middle]) * 0.5
                        : points[middle];
                const double mean =
                    std::accumulate(points.begin(), points.end(), 0.0) /
                    double(points.size());
                double squaredDeviation = 0.0;
                for (const double point : points) {
                    const double deviation = point - mean;
                    squaredDeviation += deviation * deviation;
                }
                cornerConsistency_.brakePointStdDev =
                    std::sqrt(squaredDeviation / double(points.size()));
                cornerConsistency_.brakePointRange =
                    points.back() - points.front();
            }
            emit cornerConsistencyChanged();
        });
    watcher->setFuture(
        QtConcurrent::run([path, sessionKey, key, ranked, metadata,
                           startDistance, endDistance]() {
            return loadCornerConsistency(path, sessionKey, key, ranked,
                                         metadata, startDistance, endDistance);
        }));
}

// Brake, turn-in, apex and throttle pickup for the focused corner. Both laps
// are placed on the primary lap's distance axis so the zoomed viewport can
// draw them against one x scale.
void TelemetryStore::rebuildCornerMarkers() {
    markers_.clear();
    const UnifiedLap* primary = primaryUnified();
    const QVariantList rows =
        focusedCorner_ >= 0 ? cornerComparison() : QVariantList();
    if (focusedCorner_ >= 0 && focusedCorner_ < rows.size() && primary &&
        primary->distance.size() > 1) {
        const std::vector<double>& distance = primary->distance;
        const QVariantMap row = rows[focusedCorner_].toMap();
        const bool hasCompare =
            row.value(QStringLiteral("hasCompare")).toBool();
        auto fractionAt = [&distance](double metres) {
            if (metres <= distance.front()) return 0.0;
            if (metres >= distance.back()) return 1.0;
            const auto it =
                std::lower_bound(distance.begin(), distance.end(), metres);
            const int hi = int(it - distance.begin());
            const int lo = std::max(0, hi - 1);
            const double span = distance[hi] - distance[lo];
            const double local =
                span > 0.0 ? (metres - distance[lo]) / span : 0.0;
            return (lo + local) / double(distance.size() - 1);
        };
        const int startIndex = std::clamp(
            int(std::lround(row.value(QStringLiteral("start")).toDouble() *
                            double(distance.size() - 1))),
            0, int(distance.size()) - 1);
        const double startDistance = distance[size_t(startIndex)];

        struct MarkerSpec {
            const char* key;
            const char* label;
            const char* primaryField;
            const char* compareField;
        };
        static constexpr MarkerSpec kSpecs[] = {
            {"brake", "BRAKE", "brakePoint", "compareBrakePoint"},
            {"turnin", "TURN-IN", "turnInPoint", "compareTurnInPoint"},
            {"apex", "APEX", "apexPoint", "compareApexPoint"},
            {"pickup", "THROTTLE", "throttlePoint", "compareThrottlePoint"}};
        for (const MarkerSpec& spec : kSpecs) {
            CornerMarker marker;
            marker.key = QLatin1String(spec.key);
            marker.label = QLatin1String(spec.label);
            marker.fraction = fractionAt(
                startDistance +
                row.value(QLatin1String(spec.primaryField)).toDouble());
            if (hasCompare)
                marker.referenceFraction = fractionAt(
                    startDistance +
                    row.value(QLatin1String(spec.compareField)).toDouble());
            markers_.append(marker);
        }
    }
    emit cornerFocusChanged();
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

    const QString rawName = key.mid(4);
    // src_ is freed after unification to save ~300 MB per session.
    // Re-open the file on demand for the opt-in raw-channel feature.
    const omatrack::TelemetrySource* source = session->source();
    std::unique_ptr<omatrack::TelemetrySource> reopened;
    if (!source) {
        std::string error;
        reopened = omatrack::TelemetrySource::open(
            session->path().toStdString(), &error);
        if (!reopened) return nullptr;
        source = reopened.get();
    }
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
        for (const SourceChannelSummary& channel :
             primarySession_->sourceChannels()) {
            const QString key = QStringLiteral("raw:") + channel.name;
            out.append(QVariantMap{{"key", key},
                                   {"title", channel.name},
                                   {"unit", channel.unit},
                                   {"visible", channelVisible(key)},
                                   {"color", channelColor(key)},
                                   {"weight", channelWeight(key)},
                                   {"source", true}});
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
    QStringList details{driverDisplay(primarySession_)};
    const auto meaningful = [](const QString& input) {
        const QString value = input.trimmed();
        const QString folded = value.toCaseFolded();
        return !value.isEmpty() && folded != QStringLiteral("unknown") &&
               folded != QStringLiteral("n/a") && value != QStringLiteral("—");
    };
    const auto appendDetail = [&details, &meaningful](const QString& value) {
        const QString trimmed = value.trimmed();
        if (meaningful(trimmed) && !details.contains(trimmed))
            details.append(trimmed);
    };
    if (primaryLap_ >= 0) {
        for (const auto& l : primarySession_->laps())
            if (l.lapId == primaryLap_) {
                appendDetail(l.label + QStringLiteral(" ") + l.timeText);
                break;
            }
    }

    const QVariantMap metadata =
        recordingMetadataForPath(primarySession_->path(), recordingMetadata_);
    const auto metadataOr = [&metadata](const QStringList& path,
                                        const QString& fallback) {
        const QString value = nestedText(metadata, path).trimmed();
        return value.isEmpty() ? fallback.trimmed() : value;
    };
    const QString event = metadataOr({QStringLiteral("event")}, QString());
    const QString series = metadataOr({QStringLiteral("series")}, QString());

    const QString carNumber =
        metadataOr({QStringLiteral("car"), QStringLiteral("number")},
                   primarySession_->carNumber());
    const QString carClass =
        metadataOr({QStringLiteral("car"), QStringLiteral("class")},
                   primarySession_->carClass());
    QStringList car;
    if (meaningful(primarySession_->vehicle()))
        car.append(primarySession_->vehicle().trimmed());
    if (meaningful(carNumber))
        car.append(carNumber.startsWith(QLatin1Char('#'))
                       ? carNumber
                       : QStringLiteral("#") + carNumber);
    if (meaningful(carClass)) car.append(carClass);
    appendDetail(car.join(QStringLiteral(" ")));
    appendDetail(event);
    if (series != event) appendDetail(series);

    QString recorded = meaningful(primarySession_->date())
                           ? primarySession_->date().trimmed()
                           : QString();
    const QString sessionTime = primarySession_->sessionTime().trimmed();
    if (meaningful(sessionTime) && !recorded.contains(sessionTime))
        recorded += (recorded.isEmpty() ? QString() : QStringLiteral(" ")) +
                    sessionTime;
    appendDetail(recorded);

    details.removeAll(QString());
    details.removeDuplicates();
    return details.join(QStringLiteral("  ·  "));
}

QString TelemetryStore::primaryDriverName() const {
    return primarySession_ ? driverDisplay(primarySession_) : QString();
}

QString TelemetryStore::primaryDriverMappingKey() const {
    return primarySession_ ? primarySession_->driverMappingKey() : QString();
}

QString TelemetryStore::primaryMetadataPath() const {
    if (!primarySession_) return {};
    if (primarySession_->isVideo()) return primarySession_->path();
    return QFileInfo(primarySession_->path()).absolutePath();
}

bool TelemetryStore::primaryMetadataFolderScope() const {
    return primarySession_ && !primarySession_->isVideo();
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

QUrl TelemetryStore::videoSourceFor(const QString& path) const {
    for (const LibraryLocation& location : locations_) {
        if (!location.isConnection()) continue;
        const QString cache = cachePathFor(location);
        if (cache.isEmpty() || !path.startsWith(cache + QLatin1Char('/')))
            continue;
        const QUrl stream = streamSource(connectionFor(location), path);
        // A connection also caches real files, and after "Clear cache" it
        // holds neither — either way the local path is still the answer.
        return stream.isValid() ? stream : QUrl::fromLocalFile(path);
    }
    return QUrl::fromLocalFile(path);
}

QUrl TelemetryStore::primaryVideoSource() const {
    if (!primarySession_ || !primarySession_->isVideo()) return {};
    return videoSourceFor(primarySession_->path());
}

double TelemetryStore::primaryVideoTime() const {
    if (!primarySession_ || !primarySession_->isVideo() || primaryLap_ < 0)
        return 0.0;
    const UnifiedLap* unified = primaryUnified();
    if (!unified || unified->time.empty()) return 0.0;
    const double position =
        qBound(0.0, cursorFrac_, 1.0) * double(unified->time.size() - 1);
    const size_t low = size_t(std::floor(position));
    const size_t high = std::min(low + 1, unified->time.size() - 1);
    const double relativeTime =
        unified->time[low] +
        (unified->time[high] - unified->time[low]) * (position - double(low));
    for (const LapEntry& lap : primarySession_->laps()) {
        if (lap.lapId == primaryLap_)
            return lap.startTime + relativeTime +
                   primarySession_->videoPresentationOffsetSec().value_or(0.0);
    }
    return 0.0;
}

QUrl TelemetryStore::compareVideoSource() const {
    if (!compareSession_ || !compareSession_->isVideo()) return {};
    return videoSourceFor(compareSession_->path());
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
    const UnifiedLap* primary = primaryUnified();
    if (!primary || primary->time.size() < 2) return 0.0;

    const double relativeTime =
        compareTimeForPrimaryFraction(qBound(0.0, cursorFrac_, 1.0));
    if (relativeTime < 0.0) return 0.0;
    for (const LapEntry& lap : session->laps()) {
        if (lap.lapId == compareLap_)
            return lap.startTime + relativeTime +
                   session->videoPresentationOffsetSec().value_or(0.0);
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
