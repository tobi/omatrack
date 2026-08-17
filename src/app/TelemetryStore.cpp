#include "TelemetryStore.h"
#include "AimRemoteIndex.h"

#include "ComparisonAlignment.h"
#include "RecordingSidecar.h"

#include "TrackMetadata.h"
#include "TrackAtlasSpatial.h"
#include "core/CornerAnalysis.h"
#include "core/TelemetryEngine.h"
#include "YamlConfig.h"
#include "RemoteCache.h"
#include "VerboseLog.h"

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
#include <QTimeZone>
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
bool isTelemetryFilePath(const QString& path) {
    return QFileInfo(path).suffix().compare(QStringLiteral("telemetry"),
                                            Qt::CaseInsensitive) == 0;
}

bool isMotecLayoutPath(const QString& path) {
    return QFileInfo(path).suffix().compare(QStringLiteral("ldx"),
                                            Qt::CaseInsensitive) == 0;
}

/// Motec layout files are not recordings. A sibling `.ld` is ingest-only and
/// is converted to `.telemetry` before anything analyses it.
QString motecRecordingPath(const QString& ldxPath) {
    QString stem = ldxPath;
    stem.chop(3);
    for (const QString& extension :
         {QStringLiteral("ld"), QStringLiteral("LD")}) {
        const QString recording = stem + extension;
        if (QFileInfo::exists(recording)) return recording;
    }
    return {};
}

/// Vendor file the Rust crates may open. Never a Motec `.ldx`.
QString vendorSourcePath(const QString& path) {
    if (isMotecLayoutPath(path)) return motecRecordingPath(path);
    return path;
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

QString formatFuelLoad(const std::vector<double>& fuel, double fraction) {
    if (fuel.size() < 2) return {};
    const double position =
        std::clamp(fraction, 0.0, 1.0) * double(fuel.size() - 1);
    const size_t low = size_t(std::floor(position));
    const size_t high = std::min(low + 1, fuel.size() - 1);
    const double value =
        fuel[low] + (fuel[high] - fuel[low]) * (position - double(low));
    if (!std::isfinite(value)) return {};
    if (value >= 0.0 && value <= 1.5)
        return QStringLiteral("%1%").arg(qRound(value * 100.0));
    return QStringLiteral("%1 L").arg(value, 0, 'f', 1);
}

int lapOrdinalInSession(const SessionHandle* session, int lapId) {
    if (!session || lapId < 0) return 0;
    int ordinal = 0;
    for (const LapEntry& lap : session->laps()) {
        ++ordinal;
        if (lap.lapId == lapId) return ordinal;
    }
    return 0;
}

QDate sessionDate(const SessionHandle* session) {
    if (!session) return {};
    QDate date =
        QDate::fromString(session->date(), QStringLiteral("dd/MM/yyyy"));
    if (date.isValid()) return date;
    return QDate::fromString(QFileInfo(session->path()).dir().dirName(),
                             Qt::ISODate);
}

QString normalizeSessionToken(QString token) {
    const QString upper = token.toUpper();
    if (upper.startsWith(QStringLiteral("FP")) ||
        upper.startsWith(QStringLiteral("CT")))
        return upper;
    if (upper == QStringLiteral("QUALI") || upper == QStringLiteral("QUALY") ||
        upper == QStringLiteral("QUALIFYING"))
        return QStringLiteral("Qualifying");
    if (upper == QStringLiteral("QUALYSIM")) return QStringLiteral("QualySim");
    if (upper == QStringLiteral("WARM-UP") || upper == QStringLiteral("WARMUP"))
        return QStringLiteral("Warmup");
    token = token.toLower();
    if (!token.isEmpty()) token[0] = token[0].toUpper();
    return token;
}

QString inferredSessionName(const QString& stem, const QString& folderName) {
    static const QRegularExpression kSessionToken(
        QStringLiteral("(?:^|[_ \\-])(FP\\d+|CT\\d+|Qualifying|QualySim|"
                       "Quali|Qualy|Practice|Race|Warm-?up)(?:$|[_ \\-])"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch stemMatch = kSessionToken.match(stem);
    if (stemMatch.hasMatch())
        return normalizeSessionToken(stemMatch.captured(1));
    const QRegularExpressionMatch folderMatch = kSessionToken.match(folderName);
    if (folderMatch.hasMatch())
        return normalizeSessionToken(folderMatch.captured(1));
    return {};
}

QString sessionStartClock(const QString& sessionTime,
                          const QDateTime& modified) {
    QTime time = QTime::fromString(sessionTime, QStringLiteral("HH:mm:ss"));
    if (!time.isValid())
        time = QTime::fromString(sessionTime, QStringLiteral("HH:mm"));
    if (time.isValid()) return time.toString(QStringLiteral("HH:mm"));
    if (modified.isValid()) return modified.toString(QStringLiteral("HH:mm"));
    return {};
}

QString sessionStartSortKey(const QString& sessionTime,
                            const QDateTime& modified) {
    QTime time = QTime::fromString(sessionTime, QStringLiteral("HH:mm:ss"));
    if (!time.isValid())
        time = QTime::fromString(sessionTime, QStringLiteral("HH:mm"));
    if (time.isValid()) return time.toString(QStringLiteral("HH:mm:ss"));
    if (modified.isValid())
        return modified.toString(QStringLiteral("HH:mm:ss"));
    return {};
}

QVariantList groupFolderChildrenByDay(const QString& folderPath,
                                      const QVariantList& children) {
    QVariantList folders;
    QVector<QVariantMap> files;
    folders.reserve(children.size());
    files.reserve(children.size());
    for (const QVariant& child : children) {
        const QVariantMap node = child.toMap();
        if (node.value(QStringLiteral("role")).toString() ==
            QStringLiteral("file"))
            files.append(node);
        else
            folders.append(node);
    }
    std::sort(
        files.begin(), files.end(),
        [](const QVariantMap& left, const QVariantMap& right) {
            const QString leftDay =
                left.value(QStringLiteral("sessionDayKey")).toString();
            const QString rightDay =
                right.value(QStringLiteral("sessionDayKey")).toString();
            const bool leftUnknown =
                leftDay.isEmpty() || leftDay == QStringLiteral("unknown");
            const bool rightUnknown =
                rightDay.isEmpty() || rightDay == QStringLiteral("unknown");
            if (leftUnknown != rightUnknown) return !leftUnknown;
            if (leftDay != rightDay) return leftDay < rightDay;
            const QString leftStart =
                left.value(QStringLiteral("sessionStartSort")).toString();
            const QString rightStart =
                right.value(QStringLiteral("sessionStartSort")).toString();
            if (leftStart != rightStart) return leftStart < rightStart;
            const qint64 leftModified =
                left.value(QStringLiteral("modifiedMs")).toLongLong();
            const qint64 rightModified =
                right.value(QStringLiteral("modifiedMs")).toLongLong();
            if (leftModified != rightModified)
                return leftModified < rightModified;
            return left.value(QStringLiteral("name"))
                       .toString()
                       .compare(right.value(QStringLiteral("name")).toString(),
                                Qt::CaseInsensitive) < 0;
        });

    QVariantList grouped = folders;
    QString currentDay;
    QVariantMap dayNode;
    QVariantList dayFiles;
    const auto flushDay = [&]() {
        if (dayNode.isEmpty()) return;
        dayNode.insert(QStringLiteral("children"), dayFiles);
        grouped.append(dayNode);
        dayNode.clear();
        dayFiles.clear();
    };
    for (const QVariantMap& file : std::as_const(files)) {
        QString dayKey = file.value(QStringLiteral("sessionDayKey")).toString();
        if (dayKey.isEmpty()) dayKey = QStringLiteral("unknown");
        if (dayKey != currentDay) {
            flushDay();
            currentDay = dayKey;
            QString heading =
                file.value(QStringLiteral("sessionDayHeading")).toString();
            if (heading.isEmpty())
                heading = dayKey == QStringLiteral("unknown")
                              ? QStringLiteral("Unknown date")
                              : dayKey;
            dayNode = QVariantMap{
                {QStringLiteral("role"), QStringLiteral("day")},
                {QStringLiteral("name"), heading},
                {QStringLiteral("path"),
                 folderPath + QStringLiteral("#day-") + dayKey},
                {QStringLiteral("available"), true},
                {QStringLiteral("pinned"), false},
                {QStringLiteral("children"), QVariantList{}},
            };
        }
        dayFiles.append(file);
    }
    flushDay();
    return grouped;
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
        {"fuel", "Fuel remaining", "L", "Fuel load remaining in the tank"},
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
                             const QJsonObject& cachedMetadata,
                             const QString& telemetryPath)
    : path_(path),
      telemetryPath_(telemetryPath.isEmpty() ? path : telemetryPath) {
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

void SessionHandle::setVideoClock(const VideoClock& clock,
                                  const VideoIdentityResult& identity) {
    videoClock_ = clock;
    if (identity.status != VideoIdentityStatus::NotChecked)
        videoIdentity_ = identity;
}

std::optional<double> SessionHandle::videoPresentationTime(
    double fileRelativeTime) const {
    if (!videoIdentity_.trusted() || !std::isfinite(fileRelativeTime) ||
        fileRelativeTime < 0.0)
        return std::nullopt;
    const auto nanoseconds =
        std::uint64_t(std::llround(fileRelativeTime * 1e9));
    const auto presentation =
        videoClock_.presentationTimeNs(nanoseconds, videoIdentity_.fileIndex);
    if (!presentation) return std::nullopt;
    return double(*presentation) / 1e9;
}

std::optional<double> SessionHandle::videoTelemetryTime(
    double presentationTime) const {
    if (!videoIdentity_.trusted() || !std::isfinite(presentationTime) ||
        presentationTime < 0.0)
        return std::nullopt;
    const auto nanoseconds =
        std::uint64_t(std::llround(presentationTime * 1e9));
    const auto telemetry =
        videoClock_.telemetryTimeNs(nanoseconds, videoIdentity_.fileIndex);
    if (!telemetry) return std::nullopt;
    return double(*telemetry) / 1e9;
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
        entry.firstVideoFrame = lap.firstVideoFrame;
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
    populateLaps(
        detectLapsLightweight(telemetryPath_.toStdString(), &eventDriverId));
    applyEventDriverId(eventDriverId);
}

bool SessionHandle::loadSummaryForIndex() {
    if (summaryLoaded_) return true;
    if (!isVideo()) {
        ensureLapSummary();
        return true;
    }
    std::unique_ptr<TelemetrySource> source =
        TelemetrySource::openIndex(telemetryPath_.toStdString());
    if (!source) return false;
    captureSourceChannels(*source);
    populateLaps(source->detectLaps());
    applyEventDriverId(source->detectDriverId());
    captureGpsLocation(*source);
    setVideoClock(source->videoClock());
    utcStartNs_ = source->utcStartNs();
    return true;
}

bool SessionHandle::loadChannelSummaryForIndex() {
    std::unique_ptr<TelemetrySource> source =
        TelemetrySource::openIndex(telemetryPath_.toStdString());
    if (!source) return false;
    captureSourceChannels(*source);
    return !sourceChannels_.isEmpty();
}

bool SessionHandle::loadSummaryForOpen(QString* errorString) {
    if (errorString) errorString->clear();
    std::string error;
    std::unique_ptr<TelemetrySource> source =
        TelemetrySource::open(telemetryPath_.toStdString(), &error);
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
    setVideoClock(source->videoClock());
    utcStartNs_ = source->utcStartNs();
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
    // Version 11 removes inferred media anchors. Video timing is loaded only
    // from the source's persisted frame table and presentation offsets.
    if (metadata.value(QStringLiteral("version")).toInt() != 11) return;
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
        if (object.contains(QStringLiteral("firstVideoFrame"))) {
            const double frame =
                object.value(QStringLiteral("firstVideoFrame")).toDouble(-1);
            if (frame >= 0.0) lap.firstVideoFrame = std::uint64_t(frame);
        }
        laps_.append(lap);
    }
    summaryLoaded_ = true;
}

QJsonObject SessionHandle::metadataForCache() const {
    QJsonArray laps;
    for (const LapEntry& lap : laps_) {
        QJsonObject object{
            {QStringLiteral("id"), lap.lapId},
            {QStringLiteral("start"), lap.startTime},
            {QStringLiteral("end"), lap.endTime},
            {QStringLiteral("timeMs"), lap.timeMs},
            {QStringLiteral("label"), lap.label},
            {QStringLiteral("timeText"), lap.timeText},
            {QStringLiteral("fastest"), lap.isFastest},
            {QStringLiteral("complete"), lap.isComplete},
            {QStringLiteral("pit"), lap.isPitLap},
        };
        if (lap.firstVideoFrame)
            object.insert(QStringLiteral("firstVideoFrame"),
                          double(*lap.firstVideoFrame));
        laps.append(object);
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
    QJsonObject cached{
        {QStringLiteral("version"), 11},
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
    return cached;
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

qint64 SessionHandle::startNs() const {
    qint64 start = 0;
    bool any = false;
    for (const LapEntry& lap : laps_) {
        if (!(lap.endTime > lap.startTime)) continue;
        const qint64 candidate = qint64(std::llround(lap.startTime * 1e9));
        if (!any || candidate < start) {
            start = candidate;
            any = true;
        }
    }
    return any ? start : 0;
}

qint64 SessionHandle::durationNs() const {
    qint64 end = 0;
    for (const LapEntry& lap : laps_) {
        if (!(lap.endTime > lap.startTime)) continue;
        end = std::max(end, qint64(std::llround(lap.endTime * 1e9)));
    }
    if (videoHud_.duration > 0.0)
        end = std::max(end, qint64(std::llround(videoHud_.duration * 1e9)));
    return end;
}

void SessionHandle::adoptLoadedLap(int lapId,
                                   std::unique_ptr<TelemetrySource> source,
                                   std::shared_ptr<const UnifiedLap> unified,
                                   double driverId, bool forceDriverId,
                                   const VideoIdentityResult& videoIdentity) {
    if (!source || !unified || unified->size() == 0) return;
    applyEventDriverId(driverId > 0 ? driverId : source->detectDriverId(),
                       forceDriverId);
    if (!hasGpsLocation()) captureGpsLocation(*source);
    captureSourceChannels(*source);
    setVideoClock(source->videoClock(), videoIdentity);
    captureVideoHud(*source);
    if (source->utcStartNs() >= 0) utcStartNs_ = source->utcStartNs();
    unifiedCache_.insert(lapId, std::move(unified));
    // Decoded raw channel arrays are released here. `videoClock_` retains only
    // the compact persisted frame table and file identities needed by player
    // sync; extraChannelData() re-opens opt-in raw channels on demand.
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
    VideoIdentityResult videoIdentity;
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
    std::vector<double> consistency;
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

struct IndexedSession {
    std::unique_ptr<SessionHandle> handle;
    bool unsupportedVideo = false;
};

bool isRemoteConnection(const RemoteConnection& connection) {
    return connection.type != LocationType::Folder &&
           !connection.target.isEmpty() &&
           !cacheDirectory(connection).isEmpty();
}

void logAimdVsTelemetry(const QString& aimdPath, const QString& telemetryPath);

QString publishTelemetryCompanion(const RemoteConnection& connection,
                                  const QString& videoPath,
                                  const IoCancel& cancel = {}) {
    const QString cachePath = cacheDirectory(connection);
    if (cachePath.isEmpty())
        return QStringLiteral("This file is not on a server.");
    const QString relative = QDir(cachePath).relativeFilePath(videoPath);
    if (relative.isEmpty() || relative.startsWith(QStringLiteral("..")))
        return QStringLiteral("Recording is outside this cache");
    const QString local = recordingTelemetryPath(videoPath);
    QFile file(local);
    if (!file.open(QIODevice::ReadOnly))
        return QStringLiteral("Native companion is missing");
    const QByteArray body = file.readAll();
    qCInfo(lcIo).noquote() << "write publish"
                           << recordingTelemetryRelativePath(relative)
                           << omatrack::formatBytes(body.size());
    return putObject(connection, recordingTelemetryRelativePath(relative), body,
                     cancel);
}

bool materializeStubSession(const RemoteConnection& connection,
                            SessionHandle* handle, const QString& path,
                            QString* error) {
    if (!handle) return false;
    const QString etag = cachedObjectEtag(connection, path);
    qCInfo(lcIo).noquote() << "open stub" << omatrack::displayPath(path)
                           << "etag" << etag;
    const QString extractError = materializeAimExtract(connection, path, etag);
    if (!extractError.isEmpty()) {
        qCInfo(lcIo).noquote()
            << "extract failed" << omatrack::displayPath(path) << extractError;
        if (error) *error = extractError;
        return false;
    }
    const QString extract = telemetryOpenPath(&connection, path);
    if (extract.isEmpty() || isStreamStub(extract)) {
        if (error) *error = QStringLiteral("Unable to materialize AiM extract");
        return false;
    }
    qCInfo(lcIo).noquote() << "extract ready" << omatrack::displayPath(extract);
    handle->setTelemetryPath(extract);
    if (!handle->hasSummary() && !handle->loadSummaryForOpen(error))
        return false;
    const QString companion = recordingTelemetryPath(path);
    std::string convertError;
    const auto source =
        TelemetrySource::open(extract.toStdString(), &convertError);
    if (!source) {
        if (error) *error = QString::fromStdString(convertError);
        return false;
    }
    if (!source->writeTelemetry(companion.toStdString(), &convertError,
                                QFileInfo(path).fileName().toStdString())) {
        if (error) *error = QString::fromStdString(convertError);
        return false;
    }
    handle->setTelemetryPath(companion);
    qCInfo(lcIo).noquote() << "write telemetry"
                           << omatrack::displayPath(companion)
                           << omatrack::formatBytes(QFileInfo(companion).size())
                           << "offset"
                           << source->videoPresentationOffsetSec().value_or(0.0)
                           << "s";
    logAimdVsTelemetry(extract, companion);
    publishTelemetryCompanion(connection, path);
    return true;
}

void logAimdVsTelemetry(const QString& aimdPath, const QString& telemetryPath) {
    if (!isVerbose()) return;
    if (aimdPath.isEmpty() || telemetryPath.isEmpty()) return;
    if (!QFileInfo(aimdPath).isFile() || QFileInfo(aimdPath).size() == 0)
        return;
    if (!QFileInfo(telemetryPath).isFile() ||
        QFileInfo(telemetryPath).size() == 0)
        return;
    if (QFileInfo(aimdPath).canonicalFilePath() ==
        QFileInfo(telemetryPath).canonicalFilePath())
        return;
    std::string error;
    const auto aimd = TelemetrySource::open(aimdPath.toStdString(), &error);
    if (!aimd) {
        qCInfo(lcIo).noquote() << "compare skip aimd" << displayPath(aimdPath)
                               << QString::fromStdString(error);
        return;
    }
    error.clear();
    const auto telemetry =
        TelemetrySource::open(telemetryPath.toStdString(), &error);
    if (!telemetry) {
        qCInfo(lcIo).noquote()
            << "compare skip telemetry" << displayPath(telemetryPath)
            << QString::fromStdString(error);
        return;
    }
    const std::string report =
        compareTelemetrySources(*aimd, *telemetry, "aimd", "telemetry");
    for (const QString& line : QString::fromStdString(report).split('\n')) {
        if (!line.isEmpty()) qCInfo(lcIo).noquote() << line;
    }
}

bool ensureNativeCompanion(const QString& sourcePath, QString* nativePath,
                           QString* error) {
    const QString vendor = vendorSourcePath(sourcePath);
    if (vendor.isEmpty()) {
        if (error)
            *error = QStringLiteral(
                "This Motec layout file has no recording beside it.");
        return false;
    }
    const QString dest = nativeCompanionPath(vendor);
    if (nativePath) *nativePath = dest;
    if (isTelemetryFilePath(vendor)) return QFileInfo(vendor).isFile();
    if (isStreamStub(vendor)) return false;
    if (QFileInfo(dest).isFile() && QFileInfo(dest).size() > 0) {
        if (!isVideoPath(vendor) || telemetryHasVideoClock(dest.toStdString()))
            return true;
        qCInfo(lcIo).noquote() << "rewrite telemetry" << displayPath(dest)
                               << "missing video clock";
    }
    std::string convertError;
    const auto source =
        TelemetrySource::open(vendor.toStdString(), &convertError);
    if (!source) {
        if (error) *error = QString::fromStdString(convertError);
        return false;
    }
    if (!source->writeTelemetry(dest.toStdString(), &convertError)) {
        if (error) *error = QString::fromStdString(convertError);
        return false;
    }
    qCInfo(lcIo).noquote() << "write telemetry" << displayPath(dest)
                           << formatBytes(QFileInfo(dest).size());
    if (isVideoPath(vendor)) logAimdVsTelemetry(vendor, dest);
    return true;
}

IndexedSession indexSession(const QString& path,
                            const RemoteConnection* connection = nullptr,
                            const IoCancel& cancel = {}) {
    Q_UNUSED(cancel);
    const QString cacheRoot =
        connection ? cacheDirectory(*connection) : QString();
    QString parserPath = vendorSourcePath(path);
    if (parserPath.isEmpty()) parserPath = path;

    if (isVideoPath(path)) {
        const auto sidecar = readRecordingSidecar(path, cacheRoot);
        if (sidecar) {
            parserPath = sidecar->telemetryPath;
            qCInfo(lcIo).noquote() << "sidecar" << displayPath(path) << "parser"
                                   << displayPath(parserPath);
            if (isStreamStub(parserPath) || !QFileInfo(parserPath).isFile()) {
                IndexedSession result;
                result.unsupportedVideo = true;
                return result;
            }
        } else if (isStreamStub(path)) {
            IndexedSession result;
            result.unsupportedVideo = true;
            return result;
        } else {
            QString native;
            if (!ensureNativeCompanion(parserPath, &native, nullptr) ||
                !isTelemetryFilePath(native)) {
                IndexedSession result;
                result.unsupportedVideo = true;
                return result;
            }
            parserPath = native;
        }
    } else {
        QString native;
        if (!ensureNativeCompanion(parserPath, &native, nullptr) ||
            !isTelemetryFilePath(native))
            return {};
        parserPath = native;
    }

    auto handle =
        std::make_unique<SessionHandle>(path, QJsonObject{}, parserPath);
    IndexedSession result;
    if (!handle->loadSummaryForIndex()) {
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

VideoIdentityResult verifyVideoIdentity(const TelemetrySource& source,
                                        const QString& parserPath,
                                        const QString& videoPath,
                                        bool trustedRemote) {
    VideoIdentityResult result;
    if (!isVideoPath(videoPath)) return result;

    const VideoClock& clock = source.videoClock();
    if (!clock.valid()) {
        result.status = VideoIdentityStatus::Unverified;
        result.warning =
            QStringLiteral("The recording has no complete video frame clock.");
        return result;
    }

    const QFileInfo parserInfo(parserPath);
    const QFileInfo videoInfo(videoPath);
    if (videoInfo.size() > 0 &&
        parserInfo.canonicalFilePath() == videoInfo.canonicalFilePath()) {
        result.status = VideoIdentityStatus::ExactSource;
        if (!clock.files.empty()) result.fileIndex = clock.files.front().index;
        return result;
    }

    const QString basename = videoInfo.fileName();
    const auto linked = std::find_if(
        clock.files.cbegin(), clock.files.cend(),
        [&basename](const VideoFileReference& file) {
            return QString::fromStdString(file.filename)
                       .compare(basename, Qt::CaseInsensitive) == 0;
        });
    if (linked == clock.files.cend()) {
        result.status = VideoIdentityStatus::Mismatch;
        result.warning =
            QStringLiteral("The telemetry links a different video file.");
        return result;
    }
    result.fileIndex = linked->index;

    // The connection index binds this cache path and its companion to the
    // same remote object version. Hashing a multi-gigabyte recording would
    // require an explicit offline fetch, so the ETag/object generation is the
    // remote identity contract even after that object is pinned locally.
    if (trustedRemote) {
        result.status = VideoIdentityStatus::TrustedRemoteObject;
        return result;
    }
    if (!linked->blake3) {
        result.status = VideoIdentityStatus::Unverified;
        result.warning =
            QStringLiteral("The telemetry has no video identity hash.");
        return result;
    }
    const auto actual = blake3File(videoPath.toStdString());
    if (!actual) {
        result.status = VideoIdentityStatus::Unverified;
        result.warning =
            QStringLiteral("The video identity could not be verified.");
        return result;
    }
    if (*actual != *linked->blake3) {
        result.status = VideoIdentityStatus::Mismatch;
        result.warning =
            QStringLiteral("The video was changed after telemetry conversion.");
        return result;
    }
    result.status = VideoIdentityStatus::VerifiedHash;
    return result;
}

std::shared_ptr<SessionLapLoadResult> loadSessionLap(
    const QString& parserPath, const QString& recordingPath,
    const QString& sessionKey, const LapEntry& lap,
    const QVariantMap& metadata = {}, bool verifyVideo = false,
    bool trustedRemote = false) {
    auto result = std::make_shared<SessionLapLoadResult>();
    result->sessionKey = sessionKey;
    result->lapId = lap.lapId;
    std::string error;
    result->source = TelemetrySource::open(parserPath.toStdString(), &error);
    if (!result->source) {
        result->error = error.empty()
                            ? QStringLiteral("Unable to open telemetry source")
                            : QString::fromStdString(error);
        return result;
    }
    if (verifyVideo)
        result->videoIdentity = verifyVideoIdentity(
            *result->source, parserPath, recordingPath, trustedRemote);
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
    // Composite session variation: each channel's robust p10–p90 spread also
    // includes the active lap, then physical-unit thresholds make the four
    // fields commensurate before their contributions are averaged.
    struct ConsistencyField {
        TraceConfidenceField field;
        QString key;
        double fullHeatSpread;
    };
    const ConsistencyField consistencyFields[] = {
        {TraceConfidenceField::Throttle, QStringLiteral("throttle"), 0.25},
        {TraceConfidenceField::Brake, QStringLiteral("brake"), 25.0},
        {TraceConfidenceField::Steering, QStringLiteral("steering"), 15.0},
        {TraceConfidenceField::Gear, QStringLiteral("gear"), 1.0},
    };
    result->consistency.assign(sampleCount, nan);
    for (size_t sample = 0; sample < sampleCount; ++sample) {
        double score = 0.0;
        int fields = 0;
        const double fraction =
            double(sample) / double(std::max<size_t>(1, sampleCount - 1));
        for (const ConsistencyField& field : consistencyFields) {
            const auto band = result->bands.constFind(field.key);
            if (band == result->bands.cend() || !band->valid()) continue;
            const double active =
                sampleTraceConfidence(*primary, field.field, fraction);
            const double lower = band->lower[sample];
            const double upper = band->upper[sample];
            if (!std::isfinite(active) || !std::isfinite(lower) ||
                !std::isfinite(upper))
                continue;
            const double spread =
                std::max(active, upper) - std::min(active, lower);
            score += std::clamp(spread / field.fullHeatSpread, 0.0, 1.0);
            ++fields;
        }
        if (fields > 0) result->consistency[sample] = score / double(fields);
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
                                 QString* status, const IoCancel& cancel = {}) {
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

    const RemoteSyncResult synced =
        syncConnection(connectionFor(location), cancel);
    *status = synced.status;
    if (!synced.success || synced.cachePath.isEmpty()) return {};
    return synced.cachePath;
}

std::shared_ptr<SessionScanResult> scanLibraryLocations(
    const QVector<LibraryLocation>& locations, const QSet<QString>& extraPaths,
    const QSet<QString>& openPaths, qint64 cacheLimitBytes,
    const IoCancel& cancel = {}) {
    QElapsedTimer timer;
    timer.start();
    auto result = std::make_shared<SessionScanResult>();
    QSet<QString> uniquePaths = extraPaths;
    QSet<QString> metadataPaths;
    const QStringList filters{
        "*.pds",       "*.PDS",  "*.ld",     "*.LD",  "*.vbo", "*.telemetry",
        "*.TELEMETRY", "*.VBO",  "*.mp4",    "*.MP4", "*.mov", "*.MOV",
        "*.mkv",       "*.MKV",  "*.avi",    "*.AVI", "*.m4v", "*.M4V",
        "*.webm",      "*.WEBM", "TRACK.yml"};

    // Every connection syncs before anything is enumerated, so the budget is
    // applied once against the finished cache and the file list that follows
    // cannot name something eviction has already taken away.
    QVector<QString> directories;
    directories.reserve(locations.size());
    for (const LibraryLocation& location : locations) {
        if (ioCancelled(cancel)) break;
        QString status;
        directories.append(resolveLocationDirectory(location, &status, cancel));
        result->locationStatuses.insert(location.id, status);
    }
    drainNetworkIo();
    if (ioCancelled(cancel)) {
        result->elapsedMs = timer.elapsed();
        return result;
    }
    enforceCacheBudget(cacheLimitBytes, openPaths);

    for (int index = 0; index < locations.size(); ++index) {
        const LibraryLocation& location = locations[index];
        const QString& directory = directories[index];
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
            const QString path = vendorSourcePath(it.filePath());
            if (path.isEmpty()) continue;
            if (isSidecarPath(QDir(directory).relativeFilePath(path))) continue;
            const QFileInfo resolved(path);
            const QString canonical = resolved.canonicalFilePath().isEmpty()
                                          ? resolved.absoluteFilePath()
                                          : resolved.canonicalFilePath();
            if (ioCancelled(cancel)) break;
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
    const QString& path, const RemoteConnection& connection) {
    auto result = std::make_shared<SidebarMetadataResult>();
    result->path = path;
    result->metadata =
        omatrack::track_metadata::readHierarchy(QFileInfo(path).absolutePath());
    IndexedSession indexed =
        indexSession(path, connection.id.isEmpty() ? nullptr : &connection);
    result->handle = std::move(indexed.handle);
    result->unsupportedVideo = indexed.unsupportedVideo;
    return result;
}

std::shared_ptr<FileOpenResult> openIndexedFile(
    const QString& path, const QVariantMap& metadata, bool expectTelemetry,
    const RemoteConnection& connection) {
    auto result = std::make_shared<FileOpenResult>();
    result->path = path;
    const RemoteConnection* remote =
        isRemoteConnection(connection) ? &connection : nullptr;
    IndexedSession indexed = indexSession(path, remote);

    const QString companion = nativeCompanionPath(path);
    const bool companionMissing =
        remote && isStreamStub(path) && !QFileInfo(companion).isFile();
    const bool companionStale =
        remote && isStreamStub(path) && QFileInfo(companion).isFile() &&
        !telemetryHasVideoClock(companion.toStdString());

    if (companionMissing || companionStale) {
        if (!indexed.handle)
            indexed.handle = std::make_unique<SessionHandle>(path);
        if (materializeStubSession(connection, indexed.handle.get(), path,
                                   &result->error)) {
            indexed.unsupportedVideo = false;
        } else if (result->error ==
                   QStringLiteral("MP4 has no AiM aimd track")) {
            writeUnsupportedTelemetry(nativeCompanionPath(path).toStdString());
            publishTelemetryCompanion(connection, path);
            indexed.handle.reset();
            indexed.unsupportedVideo = true;
            result->error.clear();
        } else {
            return result;
        }
    }

    if (!indexed.handle && expectTelemetry && isVideoPath(path) &&
        !isStreamStub(path)) {
        auto handle = std::make_unique<SessionHandle>(path);
        if (!handle->loadSummaryForOpen(&result->error)) return result;
        indexed.handle = std::move(handle);
        indexed.unsupportedVideo = false;
    }
    if (!indexed.handle) {
        result->standaloneVideo = indexed.unsupportedVideo || isVideoPath(path);
        return result;
    }
    const LapEntry* lap = bestLap(*indexed.handle);
    if (!lap && indexed.handle->isVideo()) {
        if (!indexed.handle->loadSummaryForOpen(&result->error)) return result;
        lap = bestLap(*indexed.handle);
    }
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
    QString native;
    if (ensureNativeCompanion(indexed.handle->telemetryPath(), &native,
                              &result->error))
        indexed.handle->setTelemetryPath(native);
    const QString parser = indexed.handle->telemetryPath();
    if (!isTelemetryFilePath(parser)) {
        if (result->error.isEmpty())
            result->error = QStringLiteral(
                "Analysis requires a native .telemetry recording");
        return result;
    }
    if (isVideoPath(path)) {
        const QString aimd = remote ? telemetryOpenPath(remote, path) : path;
        if (!aimd.isEmpty() && !isStreamStub(aimd) &&
            QFileInfo(aimd).suffix().compare(QStringLiteral("mp4"),
                                             Qt::CaseInsensitive) == 0)
            logAimdVsTelemetry(aimd, parser);
    }
    qCInfo(lcIo).noquote() << "open" << displayPath(path) << "parser"
                           << displayPath(parser) << "lap" << lap->lapId
                           << lap->timeText;
    const bool trustedRemote =
        remote && !cachedObjectEtag(*remote, indexed.handle->path()).isEmpty();
    result->lap = loadSessionLap(
        indexed.handle->telemetryPath(), indexed.handle->path(),
        indexed.handle->sessionKey(), *lap, metadata, true, trustedRemote);
    if (!result->lap->error.isEmpty()) result->error = result->lap->error;
    result->handle = std::move(indexed.handle);
    return result;
}

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
    // Keep the documented default usable even when an existing config has no
    // folder location yet (for example, after locations was hand-edited to
    // an empty list).
    defaultTelemetryDirectory();
    loadLocations();
    loadChannelsConfig();
    // A fresh install gets a real omatrack.yml immediately, so the defaults
    // are visible and hand-editable instead of implicit.
    if (YamlConfig::instance().isFresh()) savePreferences();

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
    if (scanCancel_) scanCancel_->store(true);
    if (atlasCancel_) atlasCancel_->store(true);
    cancelVideoDownloads();
    if (scanWatcher_ && scanWatcher_->isRunning())
        scanWatcher_->waitForFinished();
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
    // Written by hand, never by the app, so it is parsed the way it would be
    // typed and a value that makes no sense falls back rather than turning
    // the cache off.
    cacheLimitBytes_ = parseByteSize(
        config.value({QStringLiteral("cache"), QStringLiteral("limit")})
            .toString(),
        kDefaultCacheLimitBytes);
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

    bool rewrite = false;
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

        // A hand-edited file may hold the same all-in-one address the dialog
        // accepts. Split it the same way, and write the file back in the split
        // form, so the config and the dialog never describe one connection
        // two different ways. An unparseable address is left exactly as
        // written — the sync will say what is wrong with it, which is more
        // use than dropping the row on the floor at startup.
        const ConnectionAddress address =
            splitAddress(location.type, location.target);
        if (address.error.isEmpty() && address.target != location.target) {
            location.target = address.target;
            if (!address.username.isEmpty())
                location.username = address.username;
            if (!address.password.isEmpty())
                location.password = address.password;
            for (auto it = address.options.cbegin();
                 it != address.options.cend(); ++it)
                location.options.insert(it.key(), it.value());
            rewrite = true;
        }
        location.enabled = yamlBool(row.value(QStringLiteral("enabled")), true);
        location.id = row.value(QStringLiteral("id")).toString().trimmed();
        if (location.id.isEmpty())
            location.id = location.type == LocationType::Folder
                              ? locationId(location.target, QString())
                              : locationId(location.target, location.username);
        if (locationIndex(location.id) < 0)
            locations_.append(std::move(location));
    }
    if (rewrite) savePreferences();
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
    const quint64 generation = atlasGeneration_;
    const IoCancel cancel = atlasCancel_;
    const QString version = QCoreApplication::applicationVersion();
    const QString cachePath = trackAtlasGeometryCachePath(trackSlug, layoutId);
    auto* watcher = new QFutureWatcher<QByteArray>(this);
    connect(watcher, &QFutureWatcher<QByteArray>::finished, this,
            [this, watcher, key, generation, cachePath]() {
                const QByteArray payload = watcher->result();
                watcher->deleteLater();
                atlasGeometryRequests_.remove(key);
                if (generation != atlasGeneration_) return;
                if (payload.isEmpty()) {
                    if (!atlasCenterlines_.contains(key)) {
                        trackAtlasStatus_ = QStringLiteral(
                            "Track-atlas layout geometry unavailable");
                        emit trackAtlasChanged();
                    }
                    return;
                }
                QVector<QPointF> centerline =
                    omatrack::trackatlas::parseCenterline(payload);
                if (centerline.isEmpty()) {
                    if (!atlasCenterlines_.contains(key)) {
                        trackAtlasStatus_ = QStringLiteral(
                            "Track-atlas layout geometry invalid");
                        emit trackAtlasChanged();
                    }
                    return;
                }
                atlasCenterlines_.insert(key, std::move(centerline));
                atlasSpatialMappings_.clear();
                loadCornersForPrimary();
                emit cornersChanged();
            });
    watcher->setFuture(QtConcurrent::run([url, cancel, version, cachePath]() {
        QNetworkAccessManager unused;
        const HttpResponse response = sendFollowing(
            unused, url, "GET",
            [version](const QUrl& hop) {
                QNetworkRequest request = makeRequest(hop);
                request.setHeader(QNetworkRequest::UserAgentHeader,
                                  QStringLiteral("Omatrack/") + version);
                return request;
            },
            {}, cancel);
        if (ioCancelled(cancel) || response.status != 200 ||
            response.body.isEmpty())
            return QByteArray();
        QSaveFile output(cachePath);
        if (output.open(QIODevice::WriteOnly) &&
            output.write(response.body) == qsizetype(response.body.size()))
            output.commit();
        return response.body;
    }));
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
            qCInfo(lcIo).noquote() << "track-atlas cache miss none";
            trackAtlasStatus_ = QStringLiteral("No track-atlas cache");
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

void TelemetryStore::refreshTrackAtlas() { updateTrackAtlas(true); }

void TelemetryStore::updateTrackAtlas(bool force) {
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

    if (atlasCancel_) atlasCancel_->store(true);
    atlasCancel_ = std::make_shared<std::atomic<bool>>(false);
    const quint64 generation = ++atlasGeneration_;
    const IoCancel cancel = atlasCancel_;
    const QString version = QCoreApplication::applicationVersion();
    const QString cachePath = trackAtlasCachePath();
    trackAtlasStatus_ = QStringLiteral("Updating track-atlas…");
    emit trackAtlasChanged();
    auto* watcher = new QFutureWatcher<QByteArray>(this);
    connect(watcher, &QFutureWatcher<QByteArray>::finished, this,
            [this, watcher, generation]() {
                const QByteArray payload = watcher->result();
                watcher->deleteLater();
                if (generation != atlasGeneration_) return;
                if (payload.isEmpty()) {
                    trackAtlasStatus_ =
                        atlasTracks_.isEmpty()
                            ? QStringLiteral("Track-atlas unavailable")
                            : QStringLiteral("Track-atlas cache in use");
                    emit trackAtlasChanged();
                    return;
                }
                if (!parseTrackAtlas(payload)) {
                    trackAtlasStatus_ =
                        QStringLiteral("Track-atlas update invalid");
                    emit trackAtlasChanged();
                }
            });
    watcher->setFuture(QtConcurrent::run([cancel, version, cachePath]() {
        QNetworkAccessManager unused;
        const HttpResponse response = sendFollowing(
            unused, kTrackAtlasUrl, "GET",
            [version](const QUrl& hop) {
                QNetworkRequest request = makeRequest(hop);
                request.setHeader(QNetworkRequest::UserAgentHeader,
                                  QStringLiteral("Omatrack/") + version);
                return request;
            },
            {}, cancel);
        if (ioCancelled(cancel) || response.status != 200) {
            qCInfo(lcIo).noquote() << "track-atlas fetch failed status"
                                   << response.status << response.error;
            return QByteArray();
        }
        QSaveFile output(cachePath);
        if (output.open(QIODevice::WriteOnly) &&
            output.write(response.body) == qsizetype(response.body.size()) &&
            output.commit()) {
            qCInfo(lcIo).noquote()
                << "write track-atlas" << omatrack::displayPath(cachePath)
                << omatrack::formatBytes(response.body.size());
        } else {
            qCInfo(lcIo).noquote() << "write track-atlas failed"
                                   << omatrack::displayPath(cachePath);
        }
        return response.body;
    }));
}

// ── scanning / grouping ─────────────────────────────────────────────

void TelemetryStore::scan() {
    closedTracks_.clear();
    if (loading_) {
        rescanPending_ = true;
        if (scanCancel_) scanCancel_->store(true);
        return;
    }
    startSessionScan();
}

void TelemetryStore::startSessionScan() {
    if (!loading_) {
        loading_ = true;
        emit loadingChanged();
    }
    scanCancel_ = std::make_shared<std::atomic<bool>>(false);
    const QVector<LibraryLocation> locations = locations_;
    const QSet<QString> extraPaths = transientSessionPaths_;
    // Evicting what is on screen would leave the reader holding a file that
    // no longer exists, so the current selection is named as off limits.
    QSet<QString> openPaths;
    for (const SessionHandle* session : {primarySession_, compareSession_})
        if (session) openPaths.insert(session->path());
    const qint64 limit = cacheLimitBytes_;
    const IoCancel cancel = scanCancel_;
    scanWatcher_->setFuture(
        QtConcurrent::run([locations, extraPaths, openPaths, limit, cancel]() {
            return scanLibraryLocations(locations, extraPaths, openPaths, limit,
                                        cancel);
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
    restoreLastSelection();
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
    qCInfo(lcIo).noquote() << "open request" << omatrack::displayPath(filePath);
    if (omatrack::isJsonlPath(filePath.toStdString())) {
        attachSidecarImpl(filePath, true);
        return;
    }
    queueFileOpen(filePath, FileOpenRole::Automatic);
}

bool TelemetryStore::isMtxSidecarPath(const QString& filePath) const {
    return omatrack::isJsonlExtPath(filePath.toStdString()) ||
           omatrack::isJsonlPath(filePath.toStdString());
}

void TelemetryStore::attachSidecar(const QString& filePath) {
    attachSidecarImpl(filePath, false);
}

namespace {

QString overlayGroupId(const QString& path) {
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath().isEmpty()
                                  ? info.absoluteFilePath()
                                  : info.canonicalFilePath();
    return QString::number(qHash(canonical), 16);
}

QColor sidecarChannelColorFor(const QString& name) {
    static const QColor palette[] = {
        QColor(QStringLiteral("#7fbbb3")), QColor(QStringLiteral("#d699b6")),
        QColor(QStringLiteral("#dbbc7f")), QColor(QStringLiteral("#83c092")),
        QColor(QStringLiteral("#e67e80")), QColor(QStringLiteral("#e09d7f")),
        QColor(QStringLiteral("#a7c080")),
    };
    return palette[qHash(name) % (sizeof(palette) / sizeof(palette[0]))];
}

const LapEntry* lapEntryFor(const SessionHandle* session, int lapId) {
    if (!session || lapId < 0) return nullptr;
    for (const LapEntry& lap : session->laps()) {
        if (lap.lapId == lapId) return &lap;
    }
    return nullptr;
}

std::shared_ptr<std::vector<double>> resampleSidecarOntoLap(
    const TelemetrySource& source, size_t channel, const LapEntry& lap,
    const UnifiedLap& unified, qint64 shiftNs, qint64 clipStartNs,
    qint64 clipEndNs) {
    auto values = std::make_shared<std::vector<double>>(
        unified.size(), std::numeric_limits<double>::quiet_NaN());
    if (unified.size() == 0) return values;
    const qint64 lapStartNs = qint64(std::llround(lap.startTime * 1e9));
    for (size_t index = 0; index < unified.size(); ++index) {
        const qint64 hostNs =
            lapStartNs + qint64(std::llround(unified.time[index] * 1e9));
        if (clipEndNs > clipStartNs &&
            (hostNs < clipStartNs || hostNs >= clipEndNs))
            continue;
        const qint64 extNs = hostNs - shiftNs;
        if (extNs < 0) continue;
        double value = 0.0;
        if (source.sampleAtNs(channel, quint64(extNs), &value) &&
            std::isfinite(value))
            (*values)[index] = value;
    }
    return values;
}

struct SidecarLoadResult {
    QString path;
    QString error;
    bool notExtension = false;
    OverlayGroup group;
    QHash<QString, std::shared_ptr<std::vector<double>>> samples;
};

SidecarLoadResult loadSidecarOverlay(const QString& path, qint64 hostUtcNs,
                                     const LapEntry* lap,
                                     const UnifiedLap* unified) {
    SidecarLoadResult result;
    result.path = path;
    std::string error;
    auto source = TelemetrySource::open(path.toStdString(), &error);
    if (!source) {
        result.error = error.empty() ? QStringLiteral("Unable to read sidecar")
                                     : QString::fromStdString(error);
        return result;
    }
    if (!source->isExtension()) {
        result.notExtension = true;
        result.error = QStringLiteral("Not an MTX sidecar");
        return result;
    }
    if (source->utcStartNs() < 1) {
        result.error = QStringLiteral(
            "This sidecar has no utc stamp and cannot be placed.");
        return result;
    }
    OverlayGroup group;
    group.path = QFileInfo(path).absoluteFilePath();
    group.id = overlayGroupId(group.path);
    group.name = QString::fromStdString(source->sidecarName());
    if (group.name.isEmpty()) group.name = QFileInfo(path).completeBaseName();
    group.timezone = QString::fromStdString(source->timezone());
    group.expanded = source->groupVisible();
    group.utcStartNs = source->utcStartNs();
    group.durationNs = qint64(source->durationNs());
    group.shiftNs =
        omatrack::sidecarJoinShiftNs(hostUtcNs, source->utcStartNs());
    for (const SidecarChrome& chrome : source->sidecarChrome()) {
        OverlayChrome row;
        row.kind = chrome.kind == SidecarChrome::Kind::Pill
                       ? QStringLiteral("pill")
                       : QStringLiteral("text");
        row.text = QString::fromStdString(chrome.text);
        row.label = QString::fromStdString(chrome.label);
        row.value = QString::fromStdString(chrome.value);
        group.chrome.append(std::move(row));
    }
    for (const SidecarSpan& span : source->spans()) {
        OverlaySpan row;
        row.startHostNs = qint64(span.startNs) + group.shiftNs;
        row.endHostNs = qint64(span.endNs) + group.shiftNs;
        row.visible = span.visible;
        row.name = QString::fromStdString(span.name);
        row.title = QString::fromStdString(span.title);
        row.subtitle = QString::fromStdString(span.subtitle);
        row.color = QColor(QString::fromStdString(span.color));
        if (!row.color.isValid()) row.color = QColor(QStringLiteral("#7fbbb3"));
        for (const auto& meta : span.meta) {
            row.meta.append(QVariantMap{
                {QStringLiteral("name"), QString::fromStdString(meta.first)},
                {QStringLiteral("value"),
                 QString::fromStdString(meta.second)}});
        }
        group.spans.append(std::move(row));
    }
    QHash<QString, int> laneIndex;
    for (const OverlaySpan& span : group.spans) {
        const auto existing = laneIndex.constFind(span.name);
        if (existing != laneIndex.cend()) {
            if (span.visible) group.spanLanes[existing.value()].visible = true;
            continue;
        }
        OverlaySpanLane lane;
        lane.name = span.name;
        lane.key = QStringLiteral("sidecar:") + group.id +
                   QStringLiteral(":span:") + span.name;
        lane.visible = span.visible;
        laneIndex.insert(span.name, group.spanLanes.size());
        group.spanLanes.append(std::move(lane));
    }
    const auto& channels = source->channels();
    for (size_t index = 0; index < channels.size(); ++index) {
        OverlayChannel row;
        row.name = QString::fromStdString(channels[index].name);
        row.unit = QString::fromStdString(channels[index].unit);
        row.key =
            QStringLiteral("sidecar:") + group.id + QLatin1Char(':') + row.name;
        row.defaultVisible = source->channelDefaultVisible(index);
        row.t0HostNs = qint64(channels[index].startNs) + group.shiftNs;
        if (channels[index].frequencyHz > 0.0)
            row.periodNs =
                qint64(std::llround(1e9 / channels[index].frequencyHz));
        auto values =
            std::make_shared<std::vector<double>>(channels[index].samples);
        row.samples = values;
        if (lap && unified)
            result.samples.insert(
                row.key, resampleSidecarOntoLap(
                             *source, index, *lap, *unified, group.shiftNs,
                             qint64(std::llround(lap->startTime * 1e9)),
                             qint64(std::llround(lap->endTime * 1e9))));
        group.channels.append(std::move(row));
    }
    result.group = std::move(group);
    return result;
}

QStringList mtxNameFilters() {
    return {
        QStringLiteral("*.ext.jsonl"),      QStringLiteral("*.ext.jsonl.zstd"),
        QStringLiteral("*.ext.jsonl.zst"),  QStringLiteral("*.mtx.jsonl"),
        QStringLiteral("*.mtx.jsonl.zstd"), QStringLiteral("*.mtx.jsonl.zst")};
}

QStringList listMtxFiles(const QString& directoryPath, bool recursive) {
    QStringList found;
    const QDir directory(directoryPath);
    if (directoryPath.isEmpty() || !directory.exists()) return found;
    if (!recursive) {
        const QStringList names = directory.entryList(
            mtxNameFilters(), QDir::Files | QDir::Hidden | QDir::Readable,
            QDir::Name);
        found.reserve(names.size());
        for (const QString& name : names)
            found.append(directory.filePath(name));
        return found;
    }
    QDirIterator iterator(directoryPath, mtxNameFilters(),
                          QDir::Files | QDir::Hidden | QDir::Readable,
                          QDirIterator::Subdirectories);
    while (iterator.hasNext()) found.append(iterator.next());
    return found;
}

QString formatWallWindow(qint64 utcNs, qint64 startRelNs, qint64 endRelNs,
                         const QString& timezone) {
    if (endRelNs < startRelNs) std::swap(endRelNs, startRelNs);
    if (utcNs < 0) {
        auto hours = [](qint64 ns) {
            return QString::number(double(ns) / 3.6e12, 'f', 2);
        };
        return QStringLiteral("file-relative %1 – %2 h (no utc)")
            .arg(hours(startRelNs), hours(endRelNs));
    }
    QTimeZone zone =
        timezone.isEmpty() ? QTimeZone::UTC : QTimeZone(timezone.toUtf8());
    if (!zone.isValid()) zone = QTimeZone::UTC;
    const auto at = [&](qint64 relNs) {
        return QDateTime::fromMSecsSinceEpoch((utcNs + relNs) / 1000000, zone);
    };
    const QDateTime from = at(startRelNs);
    const QDateTime to = at(endRelNs);
    const QString zoneName = QString::fromUtf8(zone.id());
    if (from.date() == to.date())
        return QStringLiteral("%1  %2 – %3  %4")
            .arg(from.toString(QStringLiteral("yyyy-MM-dd")),
                 from.toString(QStringLiteral("HH:mm")),
                 to.toString(QStringLiteral("HH:mm")), zoneName);
    return QStringLiteral("%1 – %2  %3")
        .arg(from.toString(QStringLiteral("yyyy-MM-dd HH:mm")),
             to.toString(QStringLiteral("yyyy-MM-dd HH:mm")), zoneName);
}

}  // namespace

bool TelemetryStore::hostWindowNs(qint64* startNs, qint64* endNs,
                                  qint64* utcNs) const {
    if (!startNs || !endNs || !utcNs) return false;
    *startNs = 0;
    *endNs = 0;
    *utcNs = -1;
    if (primarySession_) {
        *utcNs = primarySession_->utcStartNs();
        *startNs = primarySession_->startNs();
        *endNs = primarySession_->durationNs();
        if (*endNs > *startNs) return true;
    }
    if (primaryVideoHud() && !primaryVideoHud()->empty()) {
        *endNs = std::max(
            *endNs, qint64(std::llround(primaryVideoHud()->duration * 1e9)));
        if (*endNs > *startNs) return true;
    }
    return false;
}

bool TelemetryStore::videoClipWindowNs(qint64* startNs, qint64* endNs) const {
    if (!startNs || !endNs) return false;
    const SessionHandle* session = primarySession_;
    if (!session || (session->videoHud().empty() && session->laps().isEmpty()))
        session = compareSession_;
    if (!session) return false;
    *startNs = 0;
    *endNs = session->durationNs();
    if (session->videoHud().duration > 0.0) {
        *endNs = qint64(std::llround(session->videoHud().duration * 1e9));
        *startNs = 0;
    }
    if (*endNs <= *startNs) {
        *startNs = session->startNs();
        *endNs = session->durationNs();
    }
    return *endNs > *startNs;
}

bool TelemetryStore::adoptOverlay(
    OverlayGroup group,
    QHash<QString, std::shared_ptr<std::vector<double>>> samples,
    QString* error) {
    qint64 hostStart = 0;
    qint64 hostEnd = 0;
    qint64 hostUtc = -1;
    if (!hostWindowNs(&hostStart, &hostEnd, &hostUtc)) {
        if (error)
            *error = QStringLiteral(
                "Open a lap, video, or traces first, then drop the sidecar.");
        return false;
    }
    const qint64 sidecarStart = group.shiftNs;
    const qint64 sidecarEnd = group.shiftNs + group.durationNs;
    if (!omatrack::nsRangesOverlap(hostStart, hostEnd, sidecarStart,
                                   sidecarEnd)) {
        if (error) {
            QString event = primaryLabel();
            if (primarySession_) {
                if (!primarySession_->date().isEmpty()) {
                    if (!event.isEmpty()) event += QStringLiteral(" · ");
                    event += primarySession_->date();
                }
                const QString driver = primaryDriverName();
                if (!driver.isEmpty()) {
                    if (!event.isEmpty()) event += QStringLiteral(" · ");
                    event += driver;
                }
            }
            if (event.isEmpty()) event = QStringLiteral("open session");
            const QString sidecarName = group.name.isEmpty()
                                            ? QFileInfo(group.path).fileName()
                                            : group.name;
            *error =
                QStringLiteral(
                    "This sidecar does not overlap the open session.\n\n"
                    "Sidecar “%1”:\n  %2\n\n"
                    "Open session “%3”:\n  %4")
                    .arg(sidecarName,
                         formatWallWindow(group.utcStartNs, 0, group.durationNs,
                                          group.timezone),
                         event,
                         formatWallWindow(hostUtc, hostStart, hostEnd,
                                          group.timezone));
        }
        return false;
    }
    for (int index = 0; index < overlays_.size(); ++index) {
        if (overlays_[index].id != group.id) continue;
        overlays_[index] = std::move(group);
        for (auto it = samples.cbegin(); it != samples.cend(); ++it)
            overlayChannelCache_.insert(it.key(), it.value());
        return true;
    }
    overlays_.append(std::move(group));
    for (auto it = samples.cbegin(); it != samples.cend(); ++it)
        overlayChannelCache_.insert(it.key(), it.value());
    return true;
}

void TelemetryStore::attachSidecarImpl(const QString& filePath, bool fromOpen,
                                       bool silent) {
    const QFileInfo info(filePath);
    if (!info.exists()) {
        if (!silent)
            emit operationError(
                QStringLiteral("Unable to attach sidecar"),
                QStringLiteral("The file no longer exists:\n%1").arg(filePath));
        return;
    }
    const QString path = info.canonicalFilePath().isEmpty()
                             ? info.absoluteFilePath()
                             : info.canonicalFilePath();
    if (overlayLoading_.contains(path)) return;
    qint64 hostStart = 0;
    qint64 hostEnd = 0;
    qint64 hostUtc = -1;
    const bool haveHost = hostWindowNs(&hostStart, &hostEnd, &hostUtc);
    if (!haveHost && !fromOpen) {
        if (!silent)
            emit operationError(
                QStringLiteral("Unable to attach sidecar"),
                QStringLiteral("Open a lap, video, or traces first, then drop "
                               "the sidecar."));
        return;
    }
    overlayLoading_.insert(path);
    const quint64 generation = ++overlayAttachGeneration_;
    auto* watcher = new QFutureWatcher<SidecarLoadResult>(this);
    QObject::connect(
        watcher, &QFutureWatcher<SidecarLoadResult>::finished, this,
        [this, watcher, path, fromOpen, silent, generation]() {
            const SidecarLoadResult result = watcher->result();
            watcher->deleteLater();
            overlayLoading_.remove(path);
            if (generation != overlayAttachGeneration_) return;
            if (result.notExtension) {
                if (fromOpen)
                    queueFileOpen(path, FileOpenRole::Automatic);
                else if (!silent)
                    emit operationError(
                        QStringLiteral("Unable to attach sidecar"),
                        QStringLiteral("This JSONL file is a recording, not an "
                                       "MTX sidecar."));
                return;
            }
            if (!result.error.isEmpty()) {
                if (!silent)
                    emit operationError(
                        QStringLiteral("Unable to attach sidecar"),
                        result.error);
                return;
            }
            QString error;
            if (!adoptOverlay(result.group, result.samples, &error)) {
                if (!silent)
                    emit operationError(
                        QStringLiteral("Unable to attach sidecar"), error);
                return;
            }
            qCInfo(lcIo).noquote()
                << "sidecar overlay" << omatrack::displayPath(path)
                << result.group.name << "channels"
                << result.group.channels.size() << "spans"
                << result.group.spans.size();
            emit overlaysChanged();
            emit channelConfigChanged();
            resampleOverlays();
        });
    watcher->setFuture(QtConcurrent::run([path, hostUtc]() {
        return loadSidecarOverlay(path, hostUtc, nullptr, nullptr);
    }));
}

void TelemetryStore::discoverSidecarSiblings() {
    if (!primarySession_) return;
    QSet<QString> attached;
    for (const OverlayGroup& group : overlays_) attached.insert(group.path);
    QStringList candidates;
    candidates +=
        listMtxFiles(QFileInfo(primarySession_->path()).absolutePath(), false);
    if (primarySession_->telemetryPath() != primarySession_->path())
        candidates += listMtxFiles(
            QFileInfo(primarySession_->telemetryPath()).absolutePath(), false);
    candidates += listMtxFiles(
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation),
        false);
    for (const omatrack::LibraryLocation& location : locations_) {
        if (!location.enabled || location.type != LocationType::Folder)
            continue;
        candidates += listMtxFiles(location.target, true);
    }
    for (const QString& candidate : candidates) {
        const QString canonical = QFileInfo(candidate).canonicalFilePath();
        const QString path = canonical.isEmpty()
                                 ? QFileInfo(candidate).absoluteFilePath()
                                 : canonical;
        if (attached.contains(path) || overlayLoading_.contains(path)) continue;
        if (!omatrack::isJsonlExtPath(path.toStdString()) &&
            !omatrack::isJsonlPath(path.toStdString()))
            continue;
        attachSidecarImpl(path, false, true);
        attached.insert(path);
    }
}

void TelemetryStore::resampleOverlays() {
    if (overlays_.isEmpty()) return;
    const LapEntry* lap = lapEntryFor(primarySession_, primaryLap_);
    const UnifiedLap* unified = primaryUnified();
    if (!lap || !unified || unified->size() < 2) {
        overlayChannelCache_.clear();
        emit channelConfigChanged();
        return;
    }
    struct Job {
        QString path;
        QString id;
        qint64 shiftNs = 0;
        QStringList keys;
        QStringList names;
    };
    QVector<Job> jobs;
    jobs.reserve(overlays_.size());
    for (const OverlayGroup& group : overlays_) {
        Job job;
        job.path = group.path;
        job.id = group.id;
        job.shiftNs = group.shiftNs;
        for (const OverlayChannel& channel : group.channels) {
            job.keys.append(channel.key);
            job.names.append(channel.name);
        }
        jobs.append(std::move(job));
    }
    const double startTime = lap->startTime;
    const std::vector<double> times = unified->time;
    qint64 clipStartNs = qint64(std::llround(lap->startTime * 1e9));
    qint64 clipEndNs = qint64(std::llround(lap->endTime * 1e9));
    qint64 videoStartNs = 0;
    qint64 videoEndNs = 0;
    if (videoClipWindowNs(&videoStartNs, &videoEndNs)) {
        clipStartNs = std::max(clipStartNs, videoStartNs);
        clipEndNs = std::min(clipEndNs, videoEndNs);
    }
    const quint64 generation = ++overlayResampleGeneration_;
    auto* watcher = new QFutureWatcher<
        QHash<QString, std::shared_ptr<std::vector<double>>>>(this);
    QObject::connect(
        watcher,
        &QFutureWatcher<
            QHash<QString, std::shared_ptr<std::vector<double>>>>::finished,
        this, [this, watcher, generation]() {
            auto samples = watcher->result();
            watcher->deleteLater();
            if (generation != overlayResampleGeneration_) return;
            overlayChannelCache_ = std::move(samples);
            emit channelConfigChanged();
        });
    watcher->setFuture(QtConcurrent::run([jobs, startTime, times, clipStartNs,
                                          clipEndNs]() {
        QHash<QString, std::shared_ptr<std::vector<double>>> samples;
        LapEntry lap;
        lap.startTime = startTime;
        UnifiedLap unified;
        unified.time = times;
        for (const Job& job : jobs) {
            std::string error;
            auto source = TelemetrySource::open(job.path.toStdString(), &error);
            if (!source) continue;
            const auto& channels = source->channels();
            for (int index = 0; index < job.names.size(); ++index) {
                size_t channelIndex = channels.size();
                for (size_t candidate = 0; candidate < channels.size();
                     ++candidate) {
                    if (QString::fromStdString(channels[candidate].name) ==
                        job.names.at(index)) {
                        channelIndex = candidate;
                        break;
                    }
                }
                if (channelIndex >= channels.size()) continue;
                samples.insert(job.keys.at(index),
                               resampleSidecarOntoLap(*source, channelIndex,
                                                      lap, unified, job.shiftNs,
                                                      clipStartNs, clipEndNs));
            }
        }
        return samples;
    }));
}

void TelemetryStore::removeOverlay(const QString& id) {
    for (int index = 0; index < overlays_.size(); ++index) {
        if (overlays_[index].id != id) continue;
        for (const OverlayChannel& channel : overlays_[index].channels)
            overlayChannelCache_.remove(channel.key);
        overlays_.removeAt(index);
        emit overlaysChanged();
        emit channelConfigChanged();
        return;
    }
}

void TelemetryStore::setOverlayExpanded(const QString& id, bool expanded) {
    for (OverlayGroup& group : overlays_) {
        if (group.id != id) continue;
        if (group.expanded == expanded) return;
        group.expanded = expanded;
        emit overlaysChanged();
        emit channelConfigChanged();
        return;
    }
}

bool TelemetryStore::overlayExpanded(const QString& id) const {
    for (const OverlayGroup& group : overlays_) {
        if (group.id == id) return group.expanded;
    }
    return true;
}

const std::vector<double>* TelemetryStore::overlayChannelData(
    const QString& key) const {
    auto cached = overlayChannelCache_.constFind(key);
    if (cached == overlayChannelCache_.cend()) return nullptr;
    return cached.value().get();
}

void TelemetryStore::queueFileOpen(const QString& filePath, FileOpenRole role,
                                   int lapId) {
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
    const QString resolvedPath = vendorSourcePath(filePath);
    if (resolvedPath.isEmpty()) {
        emit operationError(
            QStringLiteral("Unable to open file"),
            QStringLiteral("%1 has no Motec recording beside it.")
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
    pendingFileOpens_.append(PendingFileOpen{telemetryPath, role, lapId});
    pauseSidebarMetadataQueue();
    startNextFileOpen();
}

void TelemetryStore::restoreLastSelection() {
    const QString primary = lastPrimaryKey_;
    const int primaryLap = lastPrimaryLap_;
    const QString compare = lastCompareKey_;
    const int compareLap = lastCompareLap_;
    if (!primary.isEmpty() && QFileInfo::exists(primary))
        queueFileOpen(primary, FileOpenRole::Primary, primaryLap);
    if (!compare.isEmpty() && compare != primary && QFileInfo::exists(compare))
        queueFileOpen(compare, FileOpenRole::Compare, compareLap);
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
    markRecentlyUsed(path);
    const QVariantMap metadata =
        recordingMetadataForPath(path, recordingMetadata_);
    auto* watcher = new QFutureWatcher<std::shared_ptr<FileOpenResult>>(this);
    connect(
        watcher, &QFutureWatcher<std::shared_ptr<FileOpenResult>>::finished,
        this,
        [this, watcher, role = request.role, wantedLap = request.lapId]() {
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
                    result->lap->forceDriverId, result->lap->videoIdentity);
                if (added) emit sessionsChanged();
                int selectedLap = result->lap->lapId;
                if (wantedLap >= 0 && wantedLap != selectedLap) {
                    for (const LapEntry& lap : session->laps()) {
                        if (lap.lapId == wantedLap) {
                            selectedLap = wantedLap;
                            break;
                        }
                    }
                }
                if (selectedLap != result->lap->lapId) {
                    requestLapLoad(session, selectedLap,
                                   role == FileOpenRole::Compare);
                } else if (role == FileOpenRole::Compare ||
                           (role == FileOpenRole::Automatic &&
                            session->isVideo() && primarySession_ &&
                            primarySession_->isVideo() &&
                            primarySession_ != session)) {
                    setCompare(session, selectedLap);
                } else {
                    setPrimary(session, selectedLap);
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
    const LibraryLocation* location = connectionHolding(path);
    const RemoteConnection remote =
        location ? connectionFor(*location) : RemoteConnection{};
    watcher->setFuture(
        QtConcurrent::run([path, metadata, expectTelemetry, remote]() {
            return openIndexedFile(path, metadata, expectTelemetry, remote);
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
                 "bucket, or paste a whole address — "
                 "s3://KEY:SECRET@bucket/prefix?region=eu-west-2 — and the "
                 "fields below fill themselves in.")},
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
                 "cached locally and stays available offline. A whole address "
                 "— gs://KEY:SECRET@bucket/prefix — works too.")}}};
}

QVariantMap TelemetryStore::cacheUsage() const {
    const CacheUsage usage = omatrack::cacheUsage();
    return QVariantMap{
        {QStringLiteral("bytes"), usage.bytes},
        {QStringLiteral("text"), QLocale().formattedDataSize(usage.bytes)},
        {QStringLiteral("limitText"),
         QLocale().formattedDataSize(cacheLimitBytes_)},
        {QStringLiteral("videoBytes"), usage.videoBytes},
        {QStringLiteral("videoText"),
         QLocale().formattedDataSize(usage.videoBytes)}};
}

QVariantMap TelemetryStore::videoOffline(const QString& path) const {
    const LibraryLocation* location =
        isVideoFile(path) ? connectionHolding(path) : nullptr;
    if (!location)
        return QVariantMap{{QStringLiteral("remote"), false},
                           {QStringLiteral("offline"), false},
                           {QStringLiteral("busy"), false}};
    return QVariantMap{
        {QStringLiteral("remote"), true},
        {QStringLiteral("offline"),
         offlineVideoPinned(connectionFor(*location), path)},
        {QStringLiteral("busy"),
         path == videoDownloadPath_ || videoDownloadQueue_.contains(path)}};
}

void TelemetryStore::setVideoOffline(const QString& path, bool offline) {
    const LibraryLocation* location =
        isVideoFile(path) ? connectionHolding(path) : nullptr;
    if (!location) return;
    if (offline &&
        (path == videoDownloadPath_ || videoDownloadQueue_.contains(path)))
        return;

    const RemoteConnection connection = connectionFor(*location);
    const QString failure = pinOfflineVideo(connection, path, offline);
    if (!failure.isEmpty()) {
        emit operationError(QStringLiteral("Offline recording"), failure);
        return;
    }
    if (!offline) {
        // The player is holding a local path that just became a stub again,
        // so re-resolve it: what was a file is a stream once more.
        streamUrls_.remove(path);
        emit selectionChanged();
        emit locationsChanged();
        return;
    }
    videoDownloadQueue_.append(path);
    startNextVideoDownload();
    emit videoDownloadChanged();
}

void TelemetryStore::cancelVideoDownloads() {
    videoDownloadQueue_.clear();
    if (videoDownloadCancelled_) videoDownloadCancelled_->store(true);
}

void TelemetryStore::setVideoDownloadStatus(const QString& status) {
    if (videoDownloadStatus_ == status) return;
    videoDownloadStatus_ = status;
    emit videoDownloadChanged();
}

void TelemetryStore::startNextVideoDownload() {
    if (!videoDownloadPath_.isEmpty() || videoDownloadQueue_.isEmpty()) return;
    const QString path = videoDownloadQueue_.takeFirst();
    const LibraryLocation* location = connectionHolding(path);
    if (!location) {
        startNextVideoDownload();
        return;
    }

    videoDownloadPath_ = path;
    videoDownloadName_ = QFileInfo(path).fileName();
    videoDownloadReceived_ = std::make_shared<std::atomic<qint64>>(0);
    videoDownloadTotal_ = std::make_shared<std::atomic<qint64>>(-1);
    videoDownloadCancelled_ = std::make_shared<std::atomic<bool>>(false);
    setVideoDownloadStatus(
        QStringLiteral("Downloading %1…").arg(videoDownloadName_));

    if (!videoDownloadTicker_) {
        // The transfer runs on another thread and cannot touch a property, so
        // the counters it bumps are read here at a rate a person can follow.
        videoDownloadTicker_ = new QTimer(this);
        videoDownloadTicker_->setInterval(500);
        connect(videoDownloadTicker_, &QTimer::timeout, this, [this]() {
            if (videoDownloadPath_.isEmpty() || !videoDownloadReceived_) return;
            const qint64 received = videoDownloadReceived_->load();
            const qint64 total = videoDownloadTotal_->load();
            const QString progress =
                total > 0 ? QStringLiteral("%1 of %2")
                                .arg(QLocale().formattedDataSize(received),
                                     QLocale().formattedDataSize(total))
                          : QLocale().formattedDataSize(received);
            setVideoDownloadStatus(QStringLiteral("Downloading %1 — %2")
                                       .arg(videoDownloadName_, progress));
        });
    }
    videoDownloadTicker_->start();

    const RemoteConnection connection = connectionFor(*location);
    auto* watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this,
            [this, watcher, path]() {
                const QString failure = watcher->result();
                watcher->deleteLater();
                videoDownloadPath_.clear();
                if (videoDownloadTicker_) videoDownloadTicker_->stop();
                if (failure.isEmpty()) {
                    setVideoDownloadStatus(QString());
                    // The next time this recording is opened it comes off the
                    // disk. Whatever is playing right now is left alone:
                    // re-pointing it would restart it at zero to change
                    // nothing the viewer can see.
                    streamUrls_.remove(path);
                    emit locationsChanged();
                } else {
                    setVideoDownloadStatus(QString());
                    // A cancellation is what was asked for, not a fault.
                    if (failure != QStringLiteral("Download cancelled")) {
                        const LibraryLocation* owner = connectionHolding(path);
                        if (owner)
                            pinOfflineVideo(connectionFor(*owner), path, false);
                        emit operationError(
                            QStringLiteral("Offline recording"),
                            QStringLiteral("%1 could not be downloaded: %2")
                                .arg(QFileInfo(path).fileName(), failure));
                    }
                }
                startNextVideoDownload();
                emit videoDownloadChanged();
            });
    const auto received = videoDownloadReceived_;
    const auto total = videoDownloadTotal_;
    const auto cancelled = videoDownloadCancelled_;
    watcher->setFuture(
        QtConcurrent::run([connection, path, received, total, cancelled]() {
            return fetchObject(
                connection, path,
                [received, total, cancelled](qint64 got, qint64 declared) {
                    received->store(got);
                    total->store(declared);
                    return !cancelled->load();
                },
                cancelled);
        }));
}

void TelemetryStore::clearCache() {
    cancelVideoDownloads();
    if (scanCancel_) scanCancel_->store(true);
    auto* watcher = new QFutureWatcher<qint64>(this);
    connect(watcher, &QFutureWatcher<qint64>::finished, this,
            [this, watcher]() {
                watcher->deleteLater();
                streamUrls_.clear();
                streamedPaths_.clear();
                locationStatuses_.clear();
                locationFileCounts_.clear();
                emit locationsChanged();
                scan();
            });
    watcher->setFuture(
        QtConcurrent::run([]() { return omatrack::clearCache(); }));
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

    // An address may arrive with everything in it —
    // `s3://KEY:SECRET@bucket/prefix?region=…&endpoint_override=…` is one
    // pasteable string, and pasting one is the whole point. Whatever it
    // carries is lifted out into the fields that hold such things, so the
    // stored target stays the plain bucket address that names the data.
    const ConnectionAddress address =
        splitAddress(type, fields.value(QStringLiteral("target")).toString());
    if (!address.error.isEmpty()) return address.error;

    // The protocol owns what a usable address looks like, so that the dialog
    // and the sync can never disagree about whether one is acceptable.
    const QString invalid = validateTarget(type, address.target);
    if (!invalid.isEmpty()) return invalid;

    LibraryLocation location;
    location.type = type;
    // Normalization is the protocol's, not QUrl's: QUrl lowercases an
    // authority, and an S3 bucket is not a hostname — a capital letter in one
    // would be silently rewritten into a bucket that does not exist.
    location.target = normalizeTarget(type, address.target);
    location.username =
        fields.value(QStringLiteral("username")).toString().trimmed();
    location.name = fields.value(QStringLiteral("name")).toString().trimmed();
    location.enabled = fields.value(QStringLiteral("enabled"), true).toBool();
    const QVariantMap options = fields.value(QStringLiteral("options")).toMap();
    for (auto it = options.cbegin(); it != options.cend(); ++it) {
        const QString value = it.value().toString().trimmed();
        if (!value.isEmpty()) location.options.insert(it.key(), value);
    }
    QString password = fields.value(QStringLiteral("password")).toString();

    // What the address spells out wins over the separate fields: it is the
    // more recently typed of the two, and a pasted key that lost to a stale
    // one left in the dialog would be a baffling way to fail.
    if (!address.username.isEmpty()) location.username = address.username;
    if (!address.password.isEmpty()) password = address.password;
    for (auto it = address.options.cbegin(); it != address.options.cend(); ++it)
        location.options.insert(it.key(), it.value());
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
        const LibraryLocation* location = connectionHolding(path);
        const RemoteConnection remote =
            location ? connectionFor(*location) : RemoteConnection{};
        watcher->setFuture(QtConcurrent::run(
            &sidebarMetadataPool_,
            [path, remote]() { return loadSidebarMetadata(path, remote); }));
        return;
    }
}

void TelemetryStore::copyFilePath(const QString& path) const {
    const QString filePath = canonicalInputPath(path);
    if (filePath.isEmpty()) return;
    QGuiApplication::clipboard()->setText(filePath);
}

QString TelemetryStore::locationIdForPath(const QString& path) const {
    const LibraryLocation* location = connectionHolding(path);
    return location ? location->id : QString();
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
    const QFileInfo info(path);
    const SessionMeta filenameMeta =
        sessionMetaFromFilename(info.completeBaseName().toStdString());
    QString date = session ? session->date() : QString();
    if (date == QStringLiteral("Unknown")) date.clear();

    QString driver;
    if (session) {
        driver = driverDisplay(session);
        if (driver.startsWith(QStringLiteral("Driver id ")))
            driver = session->driverId();
        else if (driver == QStringLiteral("Unknown driver"))
            driver = session->driverId();
    }

    QString sessionName = nestedText(metadata, {QStringLiteral("session")});
    if (sessionName.isEmpty())
        sessionName =
            inferredSessionName(info.completeBaseName(), info.dir().dirName());

    QDate day = sessionDate(session);
    if (!day.isValid())
        day = QDate::fromString(QString::fromStdString(filenameMeta.date),
                                QStringLiteral("dd/MM/yyyy"));
    if (!day.isValid()) day = info.lastModified().date();
    const QString dayKey =
        day.isValid() ? day.toString(Qt::ISODate) : QStringLiteral("unknown");
    const QString dayHeading =
        day.isValid() ? QLocale(QLocale::English)
                            .toString(day, QStringLiteral("ddd d MMM yyyy"))
                      : QStringLiteral("Unknown date");

    const QString recordedTime =
        session ? session->sessionTime()
                : QString::fromStdString(filenameMeta.time);
    const QDateTime modified = info.lastModified();

    return QVariantMap{
        {QStringLiteral("key"), session ? session->sessionKey() : QString()},
        {QStringLiteral("hasSession"), session != nullptr},
        {QStringLiteral("isVideo"), video},
        {QStringLiteral("driver"), driver},
        {QStringLiteral("mappingKey"),
         session ? session->driverMappingKey() : QString()},
        {QStringLiteral("bestTime"),
         session ? session->bestLapTime() : QStringLiteral("—")},
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
        {QStringLiteral("track"), displayTrack(session)},
        {QStringLiteral("sessionName"), sessionName},
        {QStringLiteral("sessionStart"),
         sessionStartClock(recordedTime, modified)},
        {QStringLiteral("sessionStartSort"),
         sessionStartSortKey(recordedTime, modified)},
        {QStringLiteral("sessionDayKey"), dayKey},
        {QStringLiteral("sessionDayHeading"), dayHeading},
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
        if (role == QStringLiteral("source") ||
            role == QStringLiteral("folder"))
            children = groupFolderChildrenByDay(path, children);
        node.insert(QStringLiteral("children"), children);
        return node;
    };

    QVariantList sources;
    sources.reserve(fileSources_.size() + 2);
    for (const QVariant& source : fileSources_)
        sources.append(enrichNode(enrichNode, source.toMap()));

    QVariantList recentChildren;
    recentChildren.reserve(recentFiles_.size());
    for (const QString& path : recentFiles_) {
        const QFileInfo info(path);
        QVariantMap recentFile{
            {QStringLiteral("role"), QStringLiteral("file")},
            {QStringLiteral("name"), info.fileName()},
            {QStringLiteral("path"), path},
            {QStringLiteral("available"), info.isFile()},
            {QStringLiteral("modifiedMs"),
             info.lastModified().toMSecsSinceEpoch()},
            {QStringLiteral("modified"),
             info.lastModified().toString(QStringLiteral("yyyy-MM-dd HH:mm"))},
            {QStringLiteral("children"), QVariantList{}},
        };
        recentChildren.append(enrichNode(enrichNode, recentFile));
    }
    if (!recentChildren.isEmpty()) {
        sources.prepend(QVariantMap{
            {QStringLiteral("role"), QStringLiteral("recent")},
            {QStringLiteral("name"), QStringLiteral("Recent")},
            {QStringLiteral("path"), QStringLiteral("sidebar-recent")},
            {QStringLiteral("available"), true},
            {QStringLiteral("fileCount"), recentChildren.size()},
            {QStringLiteral("pinned"), false},
            {QStringLiteral("children"), recentChildren},
        });
    }

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
    markRecentlyUsed(session->path());
    pauseSidebarMetadataQueue();
    const std::shared_ptr<const UnifiedLap> cached = session->unifiedLap(lapId);
    if (cached) {
        qCInfo(lcIo).noquote()
            << "unify cache hit" << omatrack::displayPath(session->path())
            << "parser" << omatrack::displayPath(session->telemetryPath())
            << "lap" << lapId;
        if (session->isVideo()) rememberRecentFile(session->path());
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
    const QString parserPath = session->telemetryPath();
    const QString sessionKey = session->sessionKey();
    const LapEntry lap = *wanted;
    const QVariantMap metadata =
        recordingMetadataForPath(path, recordingMetadata_);
    const bool verifyVideo =
        session->isVideo() &&
        session->videoIdentity().status == VideoIdentityStatus::NotChecked;
    const LibraryLocation* remoteLocation = connectionHolding(path);
    const bool trustedRemote =
        remoteLocation &&
        !cachedObjectEtag(connectionFor(*remoteLocation), path).isEmpty();
    auto* watcher =
        new QFutureWatcher<std::shared_ptr<SessionLapLoadResult>>(this);
    connect(watcher,
            &QFutureWatcher<std::shared_ptr<SessionLapLoadResult>>::finished,
            this, [this, watcher, compare, generation]() {
                std::shared_ptr<SessionLapLoadResult> result =
                    watcher->result();
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
                session->adoptLoadedLap(
                    result->lapId, std::move(result->source),
                    std::move(result->unified), result->driverId,
                    result->forceDriverId, result->videoIdentity);
                if (session->isVideo()) rememberRecentFile(session->path());
                if (compare) {
                    setCompare(session, result->lapId);
                    setCompareLapLoading(false);
                } else {
                    setPrimary(session, result->lapId);
                    setPrimaryLapLoading(false);
                }
                resumeSidebarMetadataQueue();
            });
    qCInfo(lcIo).noquote() << "unify parse" << omatrack::displayPath(path)
                           << "parser" << omatrack::displayPath(parserPath)
                           << "lap" << lapId;
    watcher->setFuture(
        QtConcurrent::run([parserPath, path, sessionKey, lap, metadata,
                           verifyVideo, trustedRemote]() {
            return loadSessionLap(parserPath, path, sessionKey, lap, metadata,
                                  verifyVideo, trustedRemote);
        }));
}

static void logSelectedLap(const char* role, const SessionHandle* session,
                           int lapId) {
    if (!session) {
        qCInfo(lcIo).noquote() << role << "cleared";
        return;
    }
    QString lapText = QStringLiteral("L%1").arg(lapId);
    for (const LapEntry& lap : session->laps()) {
        if (lap.lapId != lapId) continue;
        lapText = QStringLiteral("L%1 %2").arg(lap.lapId).arg(lap.timeText);
        if (lap.firstVideoFrame)
            lapText += QStringLiteral(" frame=%1").arg(*lap.firstVideoFrame);
        break;
    }
    const auto unified = session->unifiedLap(lapId);
    QString details;
    if (unified) {
        details = QStringLiteral("samples=%1").arg(unified->size());
        if (!unified->distance.empty())
            details += QStringLiteral(" distance=%1–%2m")
                           .arg(unified->distance.front(), 0, 'f', 0)
                           .arg(unified->distance.back(), 0, 'f', 0);
        details += QStringLiteral(" gear=%1 gps=%2")
                       .arg(unified->gear.empty() ? "no" : "yes")
                       .arg(unified->gpsLat.empty() ? "no" : "yes");
    }
    if (session->videoClock().presentationOffsetNs)
        details +=
            QStringLiteral(" video-offset=%1s")
                .arg(double(*session->videoClock().presentationOffsetNs) / 1e9,
                     0, 'f', 6);
    qCInfo(lcIo).noquote() << role << "library"
                           << omatrack::displayPath(session->path()) << "parser"
                           << omatrack::displayPath(session->telemetryPath())
                           << lapText << details;
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
    if (sessionChanged) {
        qint64 hostStart = 0;
        qint64 hostEnd = 0;
        qint64 hostUtc = -1;
        if (session && hostWindowNs(&hostStart, &hostEnd, &hostUtc)) {
            QVector<OverlayGroup> kept;
            kept.reserve(overlays_.size());
            for (OverlayGroup& group : overlays_) {
                if (omatrack::nsRangesOverlap(hostStart, hostEnd, group.shiftNs,
                                              group.shiftNs + group.durationNs))
                    kept.append(std::move(group));
            }
            if (kept.size() != overlays_.size()) {
                overlays_ = std::move(kept);
                emit overlaysChanged();
            }
        }
        discoverSidecarSiblings();
    }
    resampleOverlays();
    emit selectionChanged();
    emit cornersChanged();
    emit videoTimeChanged();
    logSelectedLap("select primary", session, lapId);
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
    emit cursorFracChanged();
    logSelectedLap("select compare", session, lapId);
}

int TelemetryStore::nextPrimaryLapId() const {
    if (!primarySession_ || primaryLap_ < 0) return -1;
    const QVector<LapEntry>& laps = primarySession_->laps();
    int index = -1;
    for (int i = 0; i < laps.size(); ++i) {
        if (laps[i].lapId == primaryLap_) {
            index = i;
            break;
        }
    }
    if (index < 0) return -1;
    for (int i = index + 1; i < laps.size(); ++i) {
        if (laps[i].endTime > laps[i].startTime) return laps[i].lapId;
    }
    return -1;
}

QString TelemetryStore::lapLabel(const QString& sessionKey, int lapId) const {
    SessionHandle* session = findSession(sessionKey);
    if (!session) return {};
    for (const LapEntry& lap : session->laps()) {
        if (lap.lapId == lapId) return lap.label;
    }
    return {};
}

void TelemetryStore::prefetchLap(const QString& sessionKey, int lapId) {
    SessionHandle* session = findSession(sessionKey);
    if (!session || lapId < 0) return;
    if (session->unifiedLap(lapId)) return;
    const LapEntry* wanted = nullptr;
    for (const LapEntry& lap : session->laps()) {
        if (lap.lapId == lapId) {
            wanted = &lap;
            break;
        }
    }
    if (!wanted) return;
    const QString parserPath = session->telemetryPath();
    const QString path = session->path();
    const LapEntry lap = *wanted;
    const QVariantMap metadata =
        recordingMetadataForPath(path, recordingMetadata_);
    auto* watcher =
        new QFutureWatcher<std::shared_ptr<SessionLapLoadResult>>(this);
    connect(watcher,
            &QFutureWatcher<std::shared_ptr<SessionLapLoadResult>>::finished,
            this, [this, watcher]() {
                const std::shared_ptr<SessionLapLoadResult> result =
                    watcher->result();
                watcher->deleteLater();
                if (!result || !result->error.isEmpty() || !result->source ||
                    !result->unified)
                    return;
                SessionHandle* session = findSession(result->sessionKey);
                if (!session) return;
                session->adoptLoadedLap(
                    result->lapId, std::move(result->source),
                    std::move(result->unified), result->driverId,
                    result->forceDriverId);
            });
    watcher->setFuture(
        QtConcurrent::run([parserPath, path, sessionKey, lap, metadata]() {
            return loadSessionLap(parserPath, path, sessionKey, lap, metadata);
        }));
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
    emit cursorFracChanged();
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
    if (lcSeek().isInfoEnabled()) {
        const UnifiedLap* lap = primaryUnified();
        QString location = QStringLiteral("frac=%1").arg(v, 0, 'f', 4);
        if (lap && lap->size() > 0) {
            const double pos = v * double(lap->size() - 1);
            const size_t index = size_t(std::lround(pos));
            const size_t clamped = std::min(index, lap->size() - 1);
            location +=
                QStringLiteral(" sample=%1/%2").arg(clamped).arg(lap->size());
            if (clamped < lap->time.size())
                location +=
                    QStringLiteral(" t=%1s").arg(lap->time[clamped], 0, 'f', 3);
            if (clamped < lap->distance.size())
                location += QStringLiteral(" d=%1m").arg(lap->distance[clamped],
                                                         0, 'f', 1);
            if (clamped < lap->speed.size())
                location += QStringLiteral(" %1km/h").arg(lap->speed[clamped],
                                                          0, 'f', 1);
            if (clamped < lap->gpsLat.size() &&
                std::isfinite(lap->gpsLat[clamped]) &&
                clamped < lap->gpsLon.size() &&
                std::isfinite(lap->gpsLon[clamped]))
                location += QStringLiteral(" %1,%2")
                                .arg(lap->gpsLat[clamped], 0, 'f', 6)
                                .arg(lap->gpsLon[clamped], 0, 'f', 6);
        }
        qCInfo(lcSeek).noquote() << "seek cursor" << location << "video"
                                 << primaryVideoTime() << "s";
    }
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
        const auto absoluteTime =
            primarySession_->videoTelemetryTime(mediaTime);
        if (!absoluteTime) return;
        const double relativeTime = *absoluteTime - lap.startTime;
        double fraction = 0.0;
        const bool wasAtEnd = cursorFrac_ >= 0.999;
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
        if (qFuzzyCompare(fraction, cursorFrac_)) {
            if (fraction >= 0.999 && !wasAtEnd) emit primaryLapPlaybackEnded();
            return;
        }
        cursorFrac_ = fraction;
        emit cursorFracChanged();
        if (fraction >= 0.999 && !wasAtEnd) emit primaryLapPlaybackEnded();
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
            CornerZone compareCorner = corner;
            compareCorner.start =
                compareFractionForPrimaryFraction(corner.start);
            compareCorner.end = compareFractionForPrimaryFraction(corner.end);
            context.reference = compare;
            context.referenceMetrics = omatrack::measureCorner(
                *compare, compareCorner.start, compareCorner.end);
            // A comparison must use the same turn-in definition on both
            // laps. Lateral G on only one of them would invent a 20–30 m
            // "late" that the steering overlay does not show.
            if (context.primaryMetrics.hasLateralG !=
                context.referenceMetrics.hasLateralG) {
                context.primaryMetrics = omatrack::measureCorner(
                    *primary, corner.start, corner.end, false);
                context.referenceMetrics = omatrack::measureCorner(
                    *compare, compareCorner.start, compareCorner.end, false);
            }
            auto assignPoint = [](QVariantMap* target, const char* key,
                                  double metres) {
                if (std::isfinite(metres))
                    target->insert(QString::fromLatin1(key), metres);
            };
            assignPoint(&row, "brakePoint", context.primaryMetrics.brakePoint);
            assignPoint(&row, "liftPoint", context.primaryMetrics.liftPoint);
            assignPoint(&row, "turnInPoint",
                        context.primaryMetrics.turnInPoint);
            assignPoint(&row, "apexPoint", context.primaryMetrics.apexPoint);
            assignPoint(&row, "throttlePoint",
                        context.primaryMetrics.throttlePoint);
            QVariantMap compareStats = stats(*compare, compareCorner);
            assignPoint(&compareStats, "brakePoint",
                        context.referenceMetrics.brakePoint);
            assignPoint(&compareStats, "liftPoint",
                        context.referenceMetrics.liftPoint);
            assignPoint(&compareStats, "turnInPoint",
                        context.referenceMetrics.turnInPoint);
            assignPoint(&compareStats, "apexPoint",
                        context.referenceMetrics.apexPoint);
            assignPoint(&compareStats, "throttlePoint",
                        context.referenceMetrics.throttlePoint);
            // Map a compare-lap event (metres from that lap's zone start)
            // onto the primary lap's distance axis through the same
            // track-station map traces and delta use. Subtracting each
            // lap's own window metres treats a late event in a short
            // window as equal to an early event in a long one.
            auto compareEventPrimaryFraction = [&](const QVariant& point) {
                const double absMetres =
                    sample(compare->distance, compareCorner.start) +
                    point.toDouble();
                return primaryFractionForCompareFraction(
                    fractionAtDistance(*compare, absMetres));
            };
            auto compareEventPrimaryMetres = [&](const QVariant& point) {
                return sample(primary->distance,
                              compareEventPrimaryFraction(point));
            };
            auto pointDelta = [&](const QString& key) {
                const double primaryMetres = row.value(key).toDouble();
                const double compareMetres = compareStats.value(key).toDouble();
                if (!std::isfinite(primaryMetres) ||
                    !std::isfinite(compareMetres))
                    return std::numeric_limits<double>::quiet_NaN();
                return startDistance + primaryMetres -
                       compareEventPrimaryMetres(compareMetres);
            };
            // The reference apex, expressed on the primary lap's distance
            // axis, so both markers land on the same zoomed viewport.
            row.insert(QStringLiteral("compareApexFraction"),
                       compareEventPrimaryFraction(
                           compareStats.value(QStringLiteral("apexPoint"))));

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
                pointDelta(QStringLiteral("brakePoint"));
            const double liftPointDelta =
                pointDelta(QStringLiteral("liftPoint"));
            const double turnInDelta =
                pointDelta(QStringLiteral("turnInPoint"));
            const double apexPointDelta =
                pointDelta(QStringLiteral("apexPoint"));
            const double throttlePointDelta =
                pointDelta(QStringLiteral("throttlePoint"));
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

            context.timeDelta = timeDelta;
            context.entryTimeDelta = entryTimeDelta;
            context.exitTimeDelta = exitTimeDelta;
            context.brakePointDelta = brakePointDelta;
            context.turnInDelta = turnInDelta;
            context.throttlePointDelta = throttlePointDelta;

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
    const QString parserPath = primarySession_->telemetryPath();
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
        watcher->setFuture(QtConcurrent::run([parserPath, path, sessionKey, lap,
                                              metadata]() {
            return loadSessionLap(parserPath, path, sessionKey, lap, metadata);
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
    setHighlightedCornerMarker(QString());
    ++cornerConsistencyGeneration_;
    cornerConsistency_ = {};
    emit cornerConsistencyChanged();
    viewStart_ = focusReturnStart_;
    viewEnd_ = focusReturnEnd_;
    emit viewChanged();
    emit cornerFocusChanged();
}

void TelemetryStore::setHighlightedCornerMarker(const QString& key) {
    if (highlightedCornerMarker_ == key) return;
    highlightedCornerMarker_ = key;
    emit highlightedCornerMarkerChanged();
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
    traceConsistency_.clear();
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
    const QString parserPath = primarySession_->telemetryPath();
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
            traceConsistency_ = std::move(result->consistency);
            if (!result->error.isEmpty())
                qWarning() << "Unable to build session trace confidence"
                           << result->error;
            emit traceConfidenceChanged();
        });
    watcher->setFuture(
        QtConcurrent::run([parserPath, key, ranked, metadata, primary]() {
            return loadSessionConfidence(parserPath, key, ranked, metadata,
                                         primary);
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
    const QString parserPath = primarySession_->telemetryPath();
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
        QtConcurrent::run([parserPath, sessionKey, key, ranked, metadata,
                           startDistance, endDistance]() {
            return loadCornerConsistency(parserPath, sessionKey, key, ranked,
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
        const UnifiedLap* compare = compareUnified();
        auto sampleDistanceOnLap = [](const UnifiedLap& lap, double fraction) {
            if (lap.distance.empty()) return 0.0;
            const double position =
                qBound(0.0, fraction, 1.0) * (lap.distance.size() - 1);
            const int lo = std::clamp(int(std::floor(position)), 0,
                                      int(lap.distance.size()) - 1);
            const int hi = std::min(lo + 1, int(lap.distance.size()) - 1);
            return lap.distance[size_t(lo)] +
                   (lap.distance[size_t(hi)] - lap.distance[size_t(lo)]) *
                       (position - lo);
        };
        const double compareStart =
            hasCompare && compare
                ? sampleDistanceOnLap(
                      *compare,
                      compareFractionForPrimaryFraction(
                          row.value(QStringLiteral("start")).toDouble()))
                : 0.0;
        auto compareFractionAtMetres = [&](double metres) {
            if (!compare || compare->distance.size() < 2) return 0.0;
            if (metres <= compare->distance.front()) return 0.0;
            if (metres >= compare->distance.back()) return 1.0;
            const auto it = std::lower_bound(compare->distance.begin(),
                                             compare->distance.end(), metres);
            const int hi = int(it - compare->distance.begin());
            const int lo = std::max(0, hi - 1);
            const double span =
                compare->distance[size_t(hi)] - compare->distance[size_t(lo)];
            const double local =
                span > 0.0 ? (metres - compare->distance[size_t(lo)]) / span
                           : 0.0;
            return (lo + local) / double(compare->distance.size() - 1);
        };

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
            if (hasCompare && compare)
                marker.referenceFraction =
                    primaryFractionForCompareFraction(compareFractionAtMetres(
                        compareStart +
                        row.value(QLatin1String(spec.compareField))
                            .toDouble()));
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
    const double nextStart = qBound(0.0, start, 1.0);
    const double nextEnd = qBound(nextStart, end, 1.0);
    if (qFuzzyCompare(corners_[index].start + 1.0, nextStart + 1.0) &&
        qFuzzyCompare(corners_[index].end + 1.0, nextEnd + 1.0))
        return;
    corners_[index].start = nextStart;
    corners_[index].end = nextEnd;
    emit cornersChanged();
    if (index == focusedCorner_) rebuildCornerMarkers();
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
    if (key.startsWith(QStringLiteral("sidecar:"))) {
        for (const OverlayGroup& group : overlays_) {
            for (const OverlayChannel& channel : group.channels) {
                if (channel.key != key) continue;
                const bool visible =
                    yamlBool(YamlConfig::instance().value(
                                 {QStringLiteral("channels"), key,
                                  QStringLiteral("visible")}),
                             channel.defaultVisible);
                channelVisible_.insert(key, visible);
                return visible;
            }
            for (const OverlaySpanLane& lane : group.spanLanes) {
                if (lane.key != key) continue;
                const bool visible =
                    yamlBool(YamlConfig::instance().value(
                                 {QStringLiteral("channels"), key,
                                  QStringLiteral("visible")}),
                             lane.visible);
                channelVisible_.insert(key, visible);
                return visible;
            }
        }
        return false;
    }
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
    if (key.startsWith(QStringLiteral("raw:")) ||
        key.startsWith(QStringLiteral("sidecar:"))) {
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
    if (extraChannelLoading_.contains(cacheKey)) return nullptr;

    const QString rawName = key.mid(4);
    const QString parserPath = session->telemetryPath();
    const LapEntry* lap = nullptr;
    for (const LapEntry& candidate : session->laps()) {
        if (candidate.lapId == lapId) {
            lap = &candidate;
            break;
        }
    }
    if (!lap || lap->endTime <= lap->startTime) return nullptr;
    const double startTime = lap->startTime;
    const double endTime = lap->endTime;
    extraChannelLoading_.insert(cacheKey);
    auto* self = const_cast<TelemetryStore*>(this);
    auto* watcher =
        new QFutureWatcher<std::shared_ptr<std::vector<double>>>(self);
    QObject::connect(
        watcher,
        &QFutureWatcher<std::shared_ptr<std::vector<double>>>::finished, self,
        [self, watcher, cacheKey]() {
            auto values = watcher->result();
            watcher->deleteLater();
            self->extraChannelLoading_.remove(cacheKey);
            if (values) self->extraChannelCache_.insert(cacheKey, values);
            emit self->channelConfigChanged();
        });
    watcher->setFuture(QtConcurrent::run([parserPath, rawName, startTime,
                                          endTime]() {
        std::string error;
        auto source = TelemetrySource::open(parserPath.toStdString(), &error);
        if (!source) return std::shared_ptr<std::vector<double>>{};
        int channelIndex = -1;
        const auto& channels = source->channels();
        for (int index = 0; index < int(channels.size()); ++index) {
            if (QString::fromStdString(channels[size_t(index)].name) ==
                rawName) {
                channelIndex = index;
                break;
            }
        }
        if (channelIndex < 0) return std::shared_ptr<std::vector<double>>{};
        const int sampleCount =
            int(std::floor((endTime - startTime) * 50.0)) + 1;
        auto values =
            std::make_shared<std::vector<double>>(size_t(sampleCount), 0.0);
        for (int sample = 0; sample < sampleCount; ++sample) {
            double value = 0.0;
            const double time = startTime + double(sample) / 50.0;
            if (source->sampleAt(size_t(channelIndex), time, &value))
                (*values)[size_t(sample)] = value;
            else if (sample > 0)
                (*values)[size_t(sample)] = (*values)[size_t(sample - 1)];
        }
        return values;
    }));
    return nullptr;
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
    for (const OverlayGroup& group : overlays_) {
        for (const OverlaySpanLane& lane : group.spanLanes) {
            out.append(QVariantMap{
                {QStringLiteral("key"), lane.key},
                {QStringLiteral("title"),
                 group.name + QStringLiteral(" / ") + lane.name},
                {QStringLiteral("unit"), QString()},
                {QStringLiteral("visible"), channelVisible(lane.key)},
                {QStringLiteral("color"), channelColor(lane.key)},
                {QStringLiteral("weight"), channelWeight(lane.key)},
                {QStringLiteral("sidecar"), true},
                {QStringLiteral("span"), true}});
        }
        for (const OverlayChannel& channel : group.channels) {
            out.append(QVariantMap{
                {QStringLiteral("key"), channel.key},
                {QStringLiteral("title"),
                 group.name + QStringLiteral(" / ") + channel.name},
                {QStringLiteral("unit"), channel.unit},
                {QStringLiteral("visible"), channelVisible(channel.key)},
                {QStringLiteral("color"), channelColor(channel.key)},
                {QStringLiteral("weight"), channelWeight(channel.key)},
                {QStringLiteral("source"), false},
                {QStringLiteral("sidecar"), true}});
        }
    }
    return out;
}

QString TelemetryStore::channelColor(const QString& key) const {
    if (channelColors_.contains(key))
        return channelColors_.value(key).name(QColor::HexRgb);
    if (key.startsWith(QStringLiteral("raw:")) ||
        key.startsWith(QStringLiteral("sidecar:"))) {
        const QColor fallback =
            key.startsWith(QStringLiteral("sidecar:"))
                ? sidecarChannelColorFor(key.section(QLatin1Char(':'), 2))
                : defaultChannelColor(key);
        const QColor parsed(YamlConfig::instance()
                                .value({QStringLiteral("channels"), key,
                                        QStringLiteral("color")},
                                       fallback.name(QColor::HexRgb))
                                .toString());
        const QColor result = parsed.isValid() ? parsed : fallback;
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

const VideoHudSeries* TelemetryStore::primaryVideoHud() const {
    if (!primarySession_ || primarySession_->videoHud().empty()) return nullptr;
    return &primarySession_->videoHud();
}

int TelemetryStore::primaryLapOrdinal() const {
    return lapOrdinalInSession(primarySession_, primaryLap_);
}

int TelemetryStore::primaryLapTotal() const {
    return primarySession_ ? int(primarySession_->laps().size()) : 0;
}

QString TelemetryStore::primaryFuelLoad() const {
    const UnifiedLap* unified = primaryUnified();
    if (!unified) return {};
    return formatFuelLoad(unified->fuel, cursorFrac_);
}

QString TelemetryStore::compareDriverName() const {
    return compareSession_ ? driverDisplay(compareSession_) : QString();
}

int TelemetryStore::compareLapOrdinal() const {
    return lapOrdinalInSession(compareSession_, compareLap_);
}

int TelemetryStore::compareLapTotal() const {
    return compareSession_ ? int(compareSession_->laps().size()) : 0;
}

QString TelemetryStore::compareFuelLoad() const {
    const UnifiedLap* unified = compareUnified();
    if (!unified) return {};
    return formatFuelLoad(unified->fuel,
                          compareFractionForPrimaryFraction(cursorFrac_));
}

double TelemetryStore::primaryFractionForVideoTime(double mediaTime) const {
    if (!std::isfinite(mediaTime) || !primarySession_ || primaryLap_ < 0)
        return std::numeric_limits<double>::quiet_NaN();
    const UnifiedLap* unified = primaryUnified();
    if (!unified || unified->time.size() < 2)
        return std::numeric_limits<double>::quiet_NaN();
    for (const LapEntry& lap : primarySession_->laps()) {
        if (lap.lapId != primaryLap_) continue;
        const auto absoluteTime =
            primarySession_->videoTelemetryTime(mediaTime);
        if (!absoluteTime) return std::numeric_limits<double>::quiet_NaN();
        const double relativeTime = *absoluteTime - lap.startTime;
        if (relativeTime < unified->time.front() ||
            relativeTime > unified->time.back())
            return std::numeric_limits<double>::quiet_NaN();
        if (relativeTime <= unified->time.front()) return 0.0;
        const auto upper = std::lower_bound(unified->time.begin(),
                                            unified->time.end(), relativeTime);
        const size_t high = size_t(upper - unified->time.begin());
        if (high == 0) return 0.0;
        if (high >= unified->time.size()) return 1.0;
        const size_t low = high - 1;
        const double span = unified->time[high] - unified->time[low];
        const double local =
            span > 0.0 ? (relativeTime - unified->time[low]) / span : 0.0;
        return (double(low) + local) / double(unified->time.size() - 1);
    }
    return std::numeric_limits<double>::quiet_NaN();
}

void SessionHandle::captureVideoHud(const TelemetrySource& source) {
    videoHud_ = {};
    double duration = 0.0;
    for (const RawChannel& channel : source.channels())
        duration = std::max(duration, channel.durationSec);
    if (!(duration > 0.0)) return;

    constexpr double kRate = 25.0;
    const int count = std::max(2, int(std::llround(duration * kRate)) + 1);
    videoHud_.duration = duration;
    videoHud_.time.resize(size_t(count));
    for (int i = 0; i < count; ++i)
        videoHud_.time[size_t(i)] = double(i) / kRate;

    const auto mapping = source.mapChannels();
    const auto fill = [&](const char* field, std::vector<double>* values,
                          bool linear) {
        values->assign(size_t(count), std::numeric_limits<double>::quiet_NaN());
        const auto it = mapping.find(field);
        if (it == mapping.end() || it->second < 0 ||
            it->second >= int(source.channels().size()))
            return;
        const RawChannel& channel = source.channels()[size_t(it->second)];
        for (int i = 0; i < count; ++i) {
            const double timeSec = videoHud_.time[size_t(i)];
            double value = 0.0;
            bool ok = false;
            if (channel.frequencyHz > 0.0 && channel.samples.size() >= 2) {
                const double position = timeSec * channel.frequencyHz;
                if (position >= 0.0 &&
                    position <= double(channel.samples.size() - 1)) {
                    const size_t low = size_t(std::floor(position));
                    const size_t high =
                        std::min(low + 1, channel.samples.size() - 1);
                    value = !linear ? channel.samples[low]
                                    : channel.samples[low] +
                                          (channel.samples[high] -
                                           channel.samples[low]) *
                                              (position - double(low));
                    ok = std::isfinite(value);
                }
            } else {
                ok = source.sampleAt(size_t(it->second), timeSec, &value,
                                     linear) &&
                     std::isfinite(value);
            }
            if (ok) (*values)[size_t(i)] = value;
        }
    };
    fill("speed", &videoHud_.speed, true);
    fill("throttle", &videoHud_.throttle, true);
    fill("brake", &videoHud_.brake, true);
    fill("steering", &videoHud_.steering, true);
    fill("gear", &videoHud_.gear, false);
    fill("fuel", &videoHud_.fuel, true);
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
    out["dist"] = sampleAt(u->distance, frac) -
                  (u->distance.empty() ? 0.0 : u->distance.front());
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

double TelemetryStore::cursorTimeDelta() const {
    const QVector<double>& delta = deltaTrace();
    if (delta.size() < 2) return std::numeric_limits<double>::quiet_NaN();
    const double position =
        qBound(0.0, cursorFrac_, 1.0) * double(delta.size() - 1);
    const int low = int(std::floor(position));
    const int high = std::min(low + 1, int(delta.size()) - 1);
    const double value =
        delta[low] + (delta[high] - delta[low]) * (position - low);
    return std::isfinite(value) ? value
                                : std::numeric_limits<double>::quiet_NaN();
}

double TelemetryStore::cursorSpeedDelta() const {
    const UnifiedLap* primary = primaryUnified();
    const UnifiedLap* compare = compareUnified();
    if (!primary || !compare || primary->speed.size() < 2 ||
        compare->speed.size() < 2)
        return std::numeric_limits<double>::quiet_NaN();

    const auto sampleAt = [](const std::vector<double>& values,
                             double fraction) {
        const double position =
            qBound(0.0, fraction, 1.0) * double(values.size() - 1);
        const size_t low = size_t(std::floor(position));
        const size_t high = std::min(low + 1, values.size() - 1);
        const double value = values[low] + (values[high] - values[low]) *
                                               (position - double(low));
        return std::isfinite(value) ? value
                                    : std::numeric_limits<double>::quiet_NaN();
    };

    const double primaryFrac = qBound(0.0, cursorFrac_, 1.0);
    const double compareFrac = compareFractionForPrimaryFraction(
        qBound(0.0, primaryFrac - referenceAlignment_, 1.0));
    const double primarySpeed = sampleAt(primary->speed, primaryFrac);
    const double compareSpeed = sampleAt(compare->speed, compareFrac);
    if (!std::isfinite(primarySpeed) || !std::isfinite(compareSpeed))
        return std::numeric_limits<double>::quiet_NaN();
    return primarySpeed - compareSpeed;
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
    // and video. Station is lap progress (50 Hz distance). Δt starts at zero.
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

void TelemetryStore::markRecentlyUsed(const QString& path) const {
    // The cache budget evicts by modification time, and this is the only
    // thing that ever refreshes it. Only deliberate opens count: the sidebar
    // metadata queue touches every visible file, which would flatten the
    // signal into noise. A local file is left alone — nothing evicts those,
    // and rewriting a timestamp in the driver's own folder is not this
    // application's business.
    if (path.isEmpty() || !path.startsWith(cacheRoot() + QLatin1Char('/')))
        return;
    QFile file(path);
    if (!file.exists() || file.size() == 0) return;
    file.setFileTime(QDateTime::currentDateTime(),
                     QFileDevice::FileModificationTime);
}

const LibraryLocation* TelemetryStore::connectionHolding(
    const QString& path) const {
    const QString normalized = QFileInfo(path).absoluteFilePath();
    for (const LibraryLocation& location : locations_) {
        if (!location.isConnection()) continue;
        const QString cache =
            QFileInfo(cachePathFor(location)).absoluteFilePath();
        if (cache.isEmpty()) continue;
        if (normalized == cache ||
            normalized.startsWith(cache + QLatin1Char('/')))
            return &location;
    }
    return nullptr;
}

QUrl TelemetryStore::videoSourceFor(const QString& path) const {
    return videoSourceFor(path, false);
}

QUrl TelemetryStore::videoSourceFor(const QString& path, bool renew) const {
    const LibraryLocation* location = connectionHolding(path);
    if (!location) return QUrl::fromLocalFile(path);

    // Signing on every read would hand out a different URL each time, and
    // everything that asks "is the player already showing this?" — the
    // telemetry sync, the dual-video test — would answer no and reload. So a
    // signature is kept and reused, and replaced well before it runs out.
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const auto known = streamUrls_.constFind(path);
    if (!renew && known != streamUrls_.cend() &&
        now - known->signedAtMs < qint64(kStreamExpirySeconds) * 500) {
        qCInfo(lcIo).noquote()
            << "cache hit stream-url" << omatrack::displayPath(path);
        return known->url;
    }

    const QUrl stream = streamSource(connectionFor(*location), path);
    // A connection also caches real files, holds a recording downloaded for a
    // flight, and after "Clear cache" holds neither — in every one of those
    // the local path is still the answer.
    if (!stream.isValid()) {
        streamUrls_.remove(path);
        qCInfo(lcIo).noquote() << "video local" << omatrack::displayPath(path)
                               << omatrack::formatBytes(QFileInfo(path).size());
        return QUrl::fromLocalFile(path);
    }
    qCInfo(lcIo).noquote() << (renew ? "stream-url renew" : "stream-url sign")
                           << omatrack::displayPath(path)
                           << omatrack::displayUrl(stream);
    streamUrls_.insert(path, StreamUrl{stream, now});
    // Remembered in reverse so that an address the server stops honouring can
    // be replaced without the player having to know where it came from.
    streamedPaths_.insert(stream.toString(QUrl::RemoveQuery), path);
    return stream;
}

QUrl TelemetryStore::refreshedVideoSource(const QUrl& source) const {
    const auto known =
        streamedPaths_.constFind(source.toString(QUrl::RemoveQuery));
    if (known == streamedPaths_.cend()) return {};
    // The same recording, signed afresh: an address the server will accept
    // for another twelve hours.
    return videoSourceFor(known.value(), true);
}

QUrl TelemetryStore::primaryVideoSource() const {
    if (!primarySession_ || !primarySession_->isVideo()) return {};
    return videoSourceFor(primarySession_->path());
}

QString TelemetryStore::primaryVideoSyncWarning() const {
    return primarySession_ ? primarySession_->videoIdentity().warning
                           : QString();
}

QString TelemetryStore::compareVideoSyncWarning() const {
    return compareSession_ ? compareSession_->videoIdentity().warning
                           : QString();
}

QString TelemetryStore::localPathForVideoSource(const QUrl& source) const {
    if (source.isLocalFile()) return source.toLocalFile();
    const auto known =
        streamedPaths_.constFind(source.toString(QUrl::RemoveQuery));
    if (known != streamedPaths_.cend()) return known.value();
    return {};
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
        if (lap.lapId == primaryLap_) {
            const auto media = primarySession_->videoPresentationTime(
                lap.startTime + relativeTime);
            return media.value_or(0.0);
        }
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
    return interpolateAlignmentFraction(comparisonAlignmentFraction_, fraction);
}

double TelemetryStore::primaryFractionForCompareFraction(
    double fraction) const {
    return invertAlignmentFraction(comparisonAlignmentFraction_, fraction);
}

double TelemetryStore::compareVideoTime() const {
    return compareVideoTimeAtFraction(qBound(0.0, cursorFrac_, 1.0));
}

double TelemetryStore::primaryTimeAtFraction(double fraction) const {
    const UnifiedLap* primary = primaryUnified();
    if (!primary || primary->time.size() < 2) return 0.0;
    const double position =
        std::clamp(fraction, 0.0, 1.0) * double(primary->time.size() - 1);
    const size_t low = size_t(std::floor(position));
    const size_t high = std::min(low + 1, primary->time.size() - 1);
    return primary->time[low] + (primary->time[high] - primary->time[low]) *
                                    (position - double(low));
}

double TelemetryStore::compareVideoTimeAtFraction(double fraction) const {
    if (!compareSession_ || !compareSession_->isVideo() || compareLap_ < 0)
        return 0.0;
    const UnifiedLap* primary = primaryUnified();
    if (!primary || primary->time.size() < 2) return 0.0;

    const double relativeTime = compareTimeForPrimaryFraction(fraction);
    if (relativeTime < 0.0) return 0.0;
    for (const LapEntry& lap : compareSession_->laps()) {
        if (lap.lapId == compareLap_) {
            const auto media = compareSession_->videoPresentationTime(
                lap.startTime + relativeTime);
            return media.value_or(0.0);
        }
    }
    return 0.0;
}

double TelemetryStore::nextCornerStartFraction() const {
    constexpr double kEps = 1e-4;
    for (const CornerZone& corner : corners_) {
        if (corner.start > cursorFrac_ + kEps) return corner.start;
    }
    return -1.0;
}

bool TelemetryStore::cursorInCorner() const {
    for (const CornerZone& corner : corners_) {
        if (cursorFrac_ + 1e-6 >= corner.start &&
            cursorFrac_ <= corner.end + 1e-6)
            return true;
    }
    return false;
}

double TelemetryStore::referencePlaybackRate(double refMediaTime) const {
    if (!std::isfinite(refMediaTime)) return 1.0;
    if (cursorInCorner()) return 1.0;

    constexpr double kLockSeconds = 0.25;
    constexpr double kMinRate = 0.70;
    constexpr double kMaxRate = 1.80;
    constexpr double kUnguidedHorizon = 4.0;

    if (corners_.isEmpty()) {
        const double error = compareVideoTime() - refMediaTime;
        const double remaining =
            primaryTimeAtFraction(1.0) - primaryTimeAtFraction(cursorFrac_);
        const double horizon =
            std::min(kUnguidedHorizon, std::max(kLockSeconds, remaining));
        return std::clamp(1.0 + error / horizon, kMinRate, kMaxRate);
    }

    double horizonFrac = nextCornerStartFraction();
    if (horizonFrac < 0.0) horizonFrac = 1.0;

    const double remainingPrimary =
        primaryTimeAtFraction(horizonFrac) - primaryTimeAtFraction(cursorFrac_);
    if (remainingPrimary < kLockSeconds) return 1.0;

    const double arrive = compareVideoTimeAtFraction(horizonFrac);
    if (arrive <= 0.0) return 1.0;
    const double remainingCompare = arrive - refMediaTime;
    if (remainingCompare <= 0.0) return kMinRate;
    return std::clamp(remainingCompare / remainingPrimary, kMinRate, kMaxRate);
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
double TelemetryStore::sessionStartUnixTime() const {
    if (!primarySession_ || primarySession_->utcStartNs() < 0) return 0.0;
    return double(primarySession_->utcStartNs()) / 1e9;
}

bool TelemetryStore::hasGlobalTime() const {
    return primarySession_ && primarySession_->utcStartNs() >= 0;
}
