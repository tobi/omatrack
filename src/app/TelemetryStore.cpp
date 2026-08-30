#include "TelemetryStore.h"

#include "TraceSnapshot.h"

#include "ComparisonAlignment.h"

#include "TrackMetadata.h"
#include "TrackAtlasSpatial.h"
#include "core/CornerAnalysis.h"
#include "core/MonotonicSeries.h"
#include "core/TelemetryEngine.h"
#include "YamlConfig.h"
#include "RemoteCache.h"
#include "VerboseLog.h"
#include "OverlayManager.h"
#include "PreferencesStore.h"
#include "TrackAtlasManager.h"
#include "IndexCache.h"
#include "LuaRename.h"
#include "PathJail.h"
#include "SwapRoles.h"
#include "UsbMedia.h"

#include <QCoreApplication>
#include <QClipboard>
#include <QDateTime>
#include <QDebug>
#include <QDesktopServices>
#include <QElapsedTimer>
#include <QDate>
#include <QDir>
#include <QFileSystemWatcher>
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
#include <QTemporaryFile>

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
bool isTelemetryFilePath(const QString& path) {
    return QFileInfo(path).suffix().compare(QStringLiteral("telemetry"),
                                            Qt::CaseInsensitive) == 0;
}

bool isMotecLayoutPath(const QString& path) {
    return QFileInfo(path).suffix().compare(QStringLiteral("ldx"),
                                            Qt::CaseInsensitive) == 0;
}

/// Motec layout files are not recordings. A sibling `.ld` is the recording
/// the parser opens instead.
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

QString formatFuelLoad(const std::vector<double>& fuel, double fraction) {
    if (fuel.size() < 2) return {};
    const double value = omatrack::interpolateFraction(fuel, fraction);
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
    // Classify pit laps using the shared core rule (median * 1.35).
    std::vector<Lap> classified = detected;
    omatrack::classifyLaps(classified);
    int lapNumber = 0;
    for (size_t i = 0; i < classified.size(); ++i) {
        const Lap& lap = classified[i];
        LapEntry entry;
        entry.lapId = lap.id;
        entry.startTime = lap.startTime;
        entry.endTime = lap.endTime;
        entry.timeMs = lap.timeMs;
        entry.isComplete = lap.complete;
        entry.isPitLap = lap.isPitLap;
        entry.firstVideoFrame = lap.firstVideoFrame;
        const int sequentialNumber = lap.complete ? ++lapNumber : lapNumber;
        entry.label = lap.complete
                          ? QStringLiteral("L%1").arg(
                                lap.sourceNumber.value_or(sequentialNumber))
                      : i == 0                     ? QStringLiteral("Out")
                      : i + 1 == classified.size() ? QStringLiteral("In")
                                                   : QStringLiteral("Frag");
        entry.timeText = QString::fromStdString(formatLapTime(lap.timeMs));
        laps_.append(entry);
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
    // Reuse the same TelemetrySource path as the full open: openIndex decodes
    // channels lazily, so detectLaps/detectDriverId only decode what they need
    // and there is no double decode when the source is opened fully later.
    auto source = TelemetrySource::openIndex(telemetryPath_.toStdString());
    if (!source) return;
    populateLaps(source->detectLaps());
    applyEventDriverId(source->detectDriverId());
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
    for (size_t ci = 0; ci < channels.size(); ++ci) {
        if (channels[ci].sampleCount == 0) continue;
        source.ensureDecoded(ci);
        const RawChannel& channel = channels[ci];
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

/// Remote recordings only: resolve the ETag-keyed normalized `.telemetry`
/// for `sourcePath`, consulting this machine's cache, then the cache
/// published beside the recordings on the server, and converting from the
/// complete source only when both miss. Local files never come here — they
/// are opened directly by the parser, which is cheaper than fingerprinting a
/// multi-gigabyte onboard recording to look up a copy of it.
///
/// `lookupOnly` stops at the two cache lookups: the sidebar pass runs it for
/// every listed recording right after a sync, so a video that nobody has
/// converted yet must stay a plain row rather than trigger a 5–30 GB
/// download. The explicit click on that row converts, writes the local
/// mirror, and publishes to the server for the next machine.
bool ensureRemoteNativeTelemetry(const QString& sourcePath,
                                 const RemoteConnection& connection,
                                 QString* nativePath, QString* error,
                                 const IoCancel& cancel = {},
                                 bool lookupOnly = false) {
    if (ioCancelled(cancel)) return false;
    const QString vendor = vendorSourcePath(sourcePath);
    if (vendor.isEmpty()) {
        if (error)
            *error = QStringLiteral(
                "This Motec layout file has no recording beside it.");
        return false;
    }

    const QString key = cachedObjectEtag(connection, vendor);
    if (key.isEmpty()) {
        if (error) *error = QStringLiteral("Remote file has no ETag.");
        return false;
    }

    const QString cached = telemetryCachePath(key);
    if (cached.isEmpty()) {
        if (error) *error = QStringLiteral("Invalid telemetry cache key.");
        return false;
    }
    if (QFileInfo(cached).isFile() && QFileInfo(cached).size() > 0) {
        if (nativePath) *nativePath = cached;
        return true;
    }
    if (!QDir().mkpath(QFileInfo(cached).absolutePath())) {
        if (error)
            *error = QStringLiteral("Unable to create the telemetry cache.");
        return false;
    }

    // First consume a cache published by another machine. A miss is the
    // only case that justifies reading the complete remote recording.
    const QString remoteCache = telemetryCacheRelativeDirectory() +
                                QLatin1Char('/') + etagFileKey(key) +
                                QStringLiteral(".telemetry");
    const QString fetchError =
        fetchRemoteObject(connection, remoteCache, cached, cancel);
    if (!fetchError.isEmpty()) {
        if (error) *error = fetchError;
        return false;
    }
    if (QFileInfo(cached).isFile() && QFileInfo(cached).size() > 0) {
        if (nativePath) *nativePath = cached;
        qCInfo(lcIo).noquote()
            << "telemetry cache hit remote" << etagFileKey(key);
        return true;
    }
    if (lookupOnly) {
        if (error)
            *error = QStringLiteral(
                "This recording has not been converted yet; open it once.");
        return false;
    }

    const QString relative =
        QDir(cacheDirectory(connection)).relativeFilePath(vendor);
    if (relative.isEmpty() || relative.startsWith(QStringLiteral(".."))) {
        if (error)
            *error = QStringLiteral("Remote source is outside its cache.");
        return false;
    }
    QTemporaryFile remoteSource;
    remoteSource.setFileTemplate(
        QDir(QDir::tempPath())
            .filePath(QStringLiteral("omatrack-source-XXXXXX.") +
                      QFileInfo(vendor).suffix()));
    if (!remoteSource.open()) {
        if (error)
            *error = QStringLiteral("Unable to create remote source temp.");
        return false;
    }
    const QString inputPath = remoteSource.fileName();
    remoteSource.close();
    const QString downloadError =
        fetchRemoteObject(connection, relative, inputPath, cancel);
    if (!downloadError.isEmpty() || !QFileInfo(inputPath).isFile() ||
        QFileInfo(inputPath).size() == 0) {
        if (error)
            *error = downloadError.isEmpty()
                         ? QStringLiteral("Unable to load remote source.")
                         : downloadError;
        return false;
    }

    if (ioCancelled(cancel)) return false;
    std::string convertError;
    if (isTelemetryFilePath(inputPath)) {
        QFile input(inputPath);
        QSaveFile output(cached);
        if (!input.open(QIODevice::ReadOnly) ||
            !output.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            if (error)
                *error = QStringLiteral("Unable to copy telemetry cache.");
            return false;
        }
        while (!input.atEnd()) {
            const QByteArray chunk = input.read(1 << 20);
            if (chunk.isEmpty() && !input.atEnd()) {
                if (error)
                    *error = QStringLiteral("Unable to read telemetry source.");
                return false;
            }
            if (output.write(chunk) != chunk.size()) {
                if (error)
                    *error = QStringLiteral("Unable to write telemetry cache.");
                return false;
            }
        }
        if (!output.commit()) {
            if (error)
                *error = QStringLiteral("Unable to commit telemetry cache.");
            return false;
        }
    } else {
        const auto source =
            TelemetrySource::open(inputPath.toStdString(), &convertError);
        if (!source) {
            if (error) *error = QString::fromStdString(convertError);
            return false;
        }
        if (!source->writeTelemetry(cached.toStdString(), &convertError)) {
            if (error) *error = QString::fromStdString(convertError);
            return false;
        }
    }
    if (ioCancelled(cancel)) return false;
    if (!QFileInfo(cached).isFile() || QFileInfo(cached).size() == 0) {
        if (error) *error = QStringLiteral("Telemetry cache is empty.");
        return false;
    }

    QFile body(cached);
    if (body.open(QIODevice::ReadOnly)) {
        const QString publishError = publishRemoteObject(
            connection, remoteCache, body.readAll(), cancel);
        if (!publishError.isEmpty())
            qCWarning(lcIo).noquote()
                << "telemetry cache publish failed" << publishError;
    }
    if (nativePath) *nativePath = cached;
    qCInfo(lcIo).noquote() << "telemetry cache generated" << etagFileKey(key)
                           << formatBytes(QFileInfo(cached).size());
    return true;
}

/// The file the parser opens for a library entry: the vendor recording
/// itself for local files, the ETag-keyed `.telemetry` for remote ones.
/// Empty when there is nothing to parse (a Motec layout without its `.ld`,
/// a remote miss that could not be converted).
QString parserPathFor(const QString& path, const RemoteConnection* connection,
                      QString* error = nullptr, const IoCancel& cancel = {},
                      bool lookupOnly = false) {
    const QString vendor = vendorSourcePath(path);
    if (vendor.isEmpty()) {
        if (error)
            *error = QStringLiteral(
                "This Motec layout file has no recording beside it.");
        return {};
    }
    if (!connection) return vendor;
    QString native;
    if (!ensureRemoteNativeTelemetry(vendor, *connection, &native, error,
                                     cancel, lookupOnly))
        return {};
    return native;
}

IndexedSession indexSession(const QString& path,
                            const RemoteConnection* connection = nullptr,
                            const IoCancel& cancel = {},
                            bool sidebarPass = false) {
    if (ioCancelled(cancel)) return {};
    // The sidebar never pulls a remote video to describe it; a remote `.pds`
    // or `.ld` is kilobytes and converts on the spot.
    const bool lookupOnly = sidebarPass && connection && isVideoPath(path);
    const QString parserPath =
        parserPathFor(path, connection, nullptr, cancel, lookupOnly);
    if (parserPath.isEmpty()) {
        IndexedSession result;
        result.unsupportedVideo = isVideoPath(path);
        return result;
    }

    if (ioCancelled(cancel)) return {};
    const QJsonObject cached = loadIndexCache(parserPath);
    if (!cached.isEmpty()) {
        auto cachedHandle =
            std::make_unique<SessionHandle>(path, cached, parserPath);
        if (cachedHandle->hasSummary()) {
            IndexedSession result;
            result.handle = std::move(cachedHandle);
            return result;
        }
    }
    auto handle =
        std::make_unique<SessionHandle>(path, QJsonObject{}, parserPath);
    IndexedSession result;
    if (ioCancelled(cancel) || !handle->loadSummaryForIndex()) {
        result.unsupportedVideo = isVideoPath(path);
        return result;
    }
    storeIndexCache(parserPath, handle->metadataForCache());
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
    const omatrack::VideoClock& clock = source.videoClock();
    const QFileInfo parserInfo(parserPath);
    const QFileInfo videoInfo(videoPath);
    const std::string filename = videoInfo.fileName().toStdString();
    const omatrack::VideoFileReference* expected = clock.fileNamed(filename);
    if (!expected) {
        const auto linked = std::find_if(
            clock.files.cbegin(), clock.files.cend(),
            [&videoInfo](const omatrack::VideoFileReference& file) {
                return QString::fromStdString(file.filename)
                           .compare(videoInfo.fileName(),
                                    Qt::CaseInsensitive) == 0;
            });
        if (linked != clock.files.cend()) expected = &*linked;
    }
    // Older single-video catalogs may carry a stale filename after the
    // recording was renamed. There is still no ambiguity about which clock
    // belongs to the open recording.
    if (!expected && clock.files.size() == 1) expected = &clock.files.front();

    if (videoInfo.size() > 0 &&
        parserInfo.canonicalFilePath() == videoInfo.canonicalFilePath()) {
        result.status = VideoIdentityStatus::ExactSource;
        if (expected) result.fileIndex = expected->index;
        return result;
    }
    if (!expected) {
        result.status = clock.files.empty() ? VideoIdentityStatus::Unverified
                                            : VideoIdentityStatus::Mismatch;
        result.warning =
            clock.files.empty()
                ? QStringLiteral("The telemetry has no linked video identity.")
                : QStringLiteral("The telemetry links a different video file.");
        return result;
    }
    result.fileIndex = expected->index;

    // The connection index binds this cache path and its companion to the
    // same remote object version. Hashing a multi-gigabyte recording would
    // require an explicit offline fetch, so the ETag/object generation is the
    // remote identity contract even after that object is pinned locally.
    if (trustedRemote) {
        result.status = VideoIdentityStatus::TrustedRemoteObject;
        return result;
    }
    if (!expected->blake3) {
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
    if (*actual != *expected->blake3) {
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
        return omatrack::invertFraction(lap.distance, distance);
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
    return omatrack::interpolateFraction(*values, fraction);
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
        return omatrack::interpolateFraction(sorted, amount);
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
    // Normalizations written by another converter generation are dead weight
    // by definition; drop them before the budget is measured.
    if (const int pruned = pruneStaleTelemetryCaches())
        qCInfo(lcIo).noquote()
            << "telemetry cache pruned" << pruned << "stale generation entries";
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
    IndexedSession indexed = indexSession(
        path, connection.id.isEmpty() ? nullptr : &connection, {}, true);
    result->handle = std::move(indexed.handle);
    result->unsupportedVideo = indexed.unsupportedVideo;
    return result;
}

std::shared_ptr<FileOpenResult> openIndexedFile(
    const QString& path, const QVariantMap& metadata, bool expectTelemetry,
    const RemoteConnection& connection, const IoCancel& cancel) {
    QElapsedTimer timer;
    timer.start();
    qCInfo(lcIo).noquote() << "open begin" << displayPath(path);
    auto result = std::make_shared<FileOpenResult>();
    result->path = path;
    if (ioCancelled(cancel)) return result;

    const RemoteConnection* remote =
        isRemoteConnection(connection) ? &connection : nullptr;
    IndexedSession indexed = indexSession(path, remote, cancel);
    if (ioCancelled(cancel)) {
        qCInfo(lcIo).noquote() << "open cancelled after index"
                               << displayPath(path) << timer.elapsed() << "ms";
        return result;
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
    const QString parser = indexed.handle->telemetryPath();

    if (ioCancelled(cancel)) return result;

    qCInfo(lcIo).noquote() << "open" << displayPath(path) << "parser"
                           << displayPath(parser) << "lap" << lap->lapId
                           << lap->timeText;
    const bool trustedRemote =
        remote && !cachedObjectEtag(*remote, indexed.handle->path()).isEmpty();
    result->lap = loadSessionLap(
        indexed.handle->telemetryPath(), indexed.handle->path(),
        indexed.handle->sessionKey(), *lap, metadata, true, trustedRemote);
    if (ioCancelled(cancel)) {
        qCInfo(lcIo).noquote() << "open cancelled after lap load"
                               << displayPath(path) << timer.elapsed() << "ms";
        return result;
    }
    if (!result->lap->error.isEmpty()) result->error = result->lap->error;
    result->handle = std::move(indexed.handle);
    qCInfo(lcIo).noquote() << "open finished" << displayPath(path)
                           << timer.elapsed() << "ms"
                           << (result->error.isEmpty() ? "ok" : result->error);
    return result;
}

// ── TelemetryStore ──────────────────────────────────────────────────

TelemetryStore::TelemetryStore(QObject* parent)
    : QObject(parent),
      folderMetadataJob_(this),
      scanJob_(this),
      usbScanJob_(this),
      usbCopyJob_(this),
      fileOpenQueue_(this),
      primaryLapJob_(this),
      compareLapJob_(this),
      lapPrefetchQueue_(this),
      sidebarMetadataQueue_(this, &sidebarMetadataPool_),
      folderChannelSampleJob_(this),
      traceConfidenceJob_(this),
      cornerConsistencyJob_(this),
      videoDownloadJob_(this),
      clearCacheJob_(this) {
    if (s_storeInstance)
        qFatal("Only one TelemetryStore may exist in a process");
    s_storeInstance = this;
    sidebarMetadataPool_.setMaxThreadCount(1);
    sidebarMetadataPool_.setThreadPriority(QThread::LowPriority);
    // Loading Q_PROPERTYs mirror their pipeline's running state.
    connect(&scanJob_, &AsyncJobBase::runningChanged, this, [this]() {
        const bool busy = scanJob_.running() || rescanPending_;
        if (loading_ == busy) return;
        loading_ = busy;
        emit loadingChanged();
    });
    connect(&fileOpenQueue_.inner(), &AsyncJobBase::runningChanged, this,
            [this]() {
                const bool busy = fileOpenQueue_.busy();
                fileOpenLoading_ = busy;
                fileOpenPath_ = busy ? fileOpenQueue_.runningKey() : QString();
                emit fileOpenChanged();
                emit lapLoadingChanged();
                if (!busy) resumeSidebarMetadataQueue();
            });
    connect(&primaryLapJob_, &AsyncJobBase::runningChanged, this,
            [this]() { setPrimaryLapLoading(primaryLapJob_.running()); });
    connect(&compareLapJob_, &AsyncJobBase::runningChanged, this,
            [this]() { setCompareLapLoading(compareLapJob_.running()); });
    connect(&traceConfidenceJob_, &AsyncJobBase::runningChanged, this,
            [this]() {
                const bool loading = traceConfidenceJob_.running();
                if (traceConfidenceLoading_ == loading) return;
                traceConfidenceLoading_ = loading;
                emit traceConfidenceChanged();
            });
    // Corner focus follows the data it points at: a corner list that no
    // longer contains the focused zone drops the focus, and a new lap
    // selection recomputes its markers.
    connect(this, &TelemetryStore::cornersChanged, this, [this]() {
        invalidateComparisonAlignment();
        rebuildComparisonAlignment();
        emit cursorFracChanged();
        emit videoTimeChanged();
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
    // Extracted collaborators — plain QObjects owned by the store.
    prefs_ = new PreferencesStore(this);
    connect(prefs_, &PreferencesStore::operationError, this,
            &TelemetryStore::operationError);
    atlas_ = new TrackAtlasManager(this);
    connect(atlas_, &TrackAtlasManager::changed, this, [this]() {
        emit trackAtlasChanged();
        emit sessionsChanged();
    });
    connect(atlas_, &TrackAtlasManager::cornersNeedReload, this, [this]() {
        loadCornersForPrimary();
        emit cornersChanged();
    });
    overlays_ = new OverlayManager(this);
    overlays_->setLocations(&prefs_->locations());
    connect(overlays_, &OverlayManager::overlaysChanged, this,
            &TelemetryStore::overlaysChanged);
    connect(overlays_, &OverlayManager::sidecarLibraryChanged, this,
            &TelemetryStore::sidecarLibraryChanged);
    connect(overlays_, &OverlayManager::channelConfigChanged, this,
            &TelemetryStore::channelConfigChanged);
    connect(overlays_, &OverlayManager::openAsFile, this,
            [this](const QString& path) {
                queueFileOpen(path, FileOpenRole::Automatic);
            });
    connect(overlays_, &OverlayManager::operationError, this,
            &TelemetryStore::operationError);
    // Flush any pending preference write synchronously at shutdown.
    connect(qApp, &QCoreApplication::aboutToQuit, this,
            [this]() { flushPreferences(); });
    // Typed list models backing the Q_PROPERTYs that replace QVariantList
    // builders. Created before preferences load so the first signal burst
    // refreshes them; refreshed on the matching store signal.
    primaryLapsModel_ = std::make_unique<LapListModel>(this);
    compareLapsModel_ = std::make_unique<LapListModel>(this);
    channelsModel_ = std::make_unique<ChannelListModel>(this);
    cornersModel_ = std::make_unique<CornerListModel>(this);
    driverMappingsModel_ = std::make_unique<DriverMappingModel>(this);
    syncStrategyModel_ = std::make_unique<SyncStrategyModel>(this);
    libraryModel_ = std::make_unique<LibraryModel>(this);
    connect(this, &TelemetryStore::selectionChanged, this, [this]() {
        refreshLapModels();
        refreshCornersModel();
        refreshSyncStrategyModel();
    });
    connect(this, &TelemetryStore::sessionsChanged, this, [this]() {
        refreshLibraryModel();
        refreshDriverMappingsModel();
        refreshLapModels();
    });
    connect(this, &TelemetryStore::channelConfigChanged, this,
            [this]() { refreshChannelsModel(); });
    connect(this, &TelemetryStore::overlaysChanged, this,
            [this]() { refreshChannelsModel(); });
    connect(this, &TelemetryStore::cornersChanged, this,
            [this]() { refreshCornersModel(); });
    connect(this, &TelemetryStore::driverMappingsChanged, this,
            [this]() { refreshDriverMappingsModel(); });
    connect(this, &TelemetryStore::comparisonSyncStrategyChanged, this,
            [this]() { refreshSyncStrategyModel(); });
    connect(this, &TelemetryStore::referenceAlignmentChanged, this,
            [this]() { refreshCornersModel(); });
    connect(this, &TelemetryStore::filePinsChanged, this,
            [this]() { refreshLibraryModel(); });
    connect(this, &TelemetryStore::recentFilesChanged, this,
            [this]() { refreshLibraryModel(); });
    connect(this, &TelemetryStore::selectionChanged, this, [this]() {
        libraryModel_->updateSelection(primarySessionKey(),
                                       compareSessionKey());
    });
    connect(this, &TelemetryStore::sidebarMetadataChanged, this,
            [this](const QString& path, const QVariantMap& details) {
                libraryModel_->updateFileMetadata(path, details);
            });

    prefs_->loadAll(defaultTelemetryDirectory());
    // Keep the documented default usable even when an existing config has no
    // folder location yet (for example, after locations was hand-edited to
    // an empty list). The directory itself is created once, off the GUI
    // thread, so a QML call never blocks on mkdir.
    QThreadPool::globalInstance()->start(
        [dir = defaultTelemetryDirectory()]() { QDir().mkpath(dir); });

    atlas_->startup();

    scan();
    setupLibraryWatch();
}

TelemetryStore::~TelemetryStore() {
    flushPreferences();
    cancelVideoDownloads();
    // AsyncJob / SerialJobQueue members cancel their workers and wait for the
    // in-flight future in their own destructors (run after this body, in
    // reverse declaration order, before the sidebar pool itself is destroyed),
    // so store teardown never leaves a worker writing into freed state.
    if (s_storeInstance == this) s_storeInstance.clear();
}

void TelemetryStore::flushPreferences() { prefs_->flush(); }

void TelemetryStore::schedulePreferencesSave() { prefs_->scheduleSave(); }

int TelemetryStore::locationIndex(const QString& id) const {
    return prefs_->locationIndex(id);
}

bool TelemetryStore::appendFolderLocation(const QString& dirPath,
                                          bool requireExists) {
    return prefs_->appendFolderLocation(dirPath, requireExists);
}

QStringList TelemetryStore::cornerConfigPath(const QString& track) {
    return PreferencesStore::cornerConfigPath(track);
}

void TelemetryStore::refreshTrackAtlas() { atlas_->refreshTrackAtlas(); }

// ── scanning / grouping ─────────────────────────────────────────────

void TelemetryStore::scan() {
    closedTracks_.clear();
    if (loading_) {
        rescanPending_ = true;
        if (const auto c = scanJob_.cancel()) c->store(true);
        return;
    }
    startSessionScan();
}

void TelemetryStore::startSessionScan() {
    const QVector<LibraryLocation> locations = prefs_->locations();
    const QSet<QString> extraPaths = transientSessionPaths_;
    // Evicting what is on screen would leave the reader holding a file that
    // no longer exists, so the current selection is named as off limits.
    QSet<QString> openPaths;
    for (const SessionHandle* session : {primarySession_, compareSession_})
        if (session) openPaths.insert(session->path());
    const qint64 limit = prefs_->cacheLimitBytes();
    scanJob_.start(
        [locations, extraPaths, openPaths, limit](IoCancel cancel) {
            return scanLibraryLocations(locations, extraPaths, openPaths, limit,
                                        cancel);
        },
        [this](std::shared_ptr<SessionScanResult> result) {
            finishSessionScan(std::move(result));
        });
}

void TelemetryStore::finishSessionScan(
    std::shared_ptr<SessionScanResult> result) {
    // A cancelled/superseded scan never reaches here: reset() bumps the
    // generation so its result is discarded by AsyncJob before delivery.
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
    emit sessionsChanged();
    emit locationsChanged();
    restoreLastSelection();
}

void TelemetryStore::cancelFileOpen() {
    if (fileOpenQueue_.busy())
        qCInfo(lcIo).noquote()
            << "open cancelled by user" << displayPath(fileOpenPath_);
    fileOpenQueue_.clear();  // cancels in-flight + drops pending
    resumeSidebarMetadataQueue();
}

void TelemetryStore::addSessionDirectory(const QString& dirPath) {
    if (!appendFolderLocation(dirPath)) return;
    schedulePreferencesSave();
    emit locationsChanged();
    scan();
}

void TelemetryStore::removeSessionDirectory(const QString& dirPath) {
    const QFileInfo info(dirPath);
    const QString absolute =
        info.absoluteFilePath().isEmpty() ? dirPath : info.absoluteFilePath();
    for (int i = 0; i < prefs_->locations().size(); ++i) {
        if (prefs_->locations()[i].type != LocationType::Folder) continue;
        if (prefs_->locations()[i].target != absolute &&
            prefs_->locations()[i].target != dirPath)
            continue;
        removeLocation(prefs_->locations()[i].id);
        return;
    }
}

void TelemetryStore::openFile(const QString& filePath) {
    qCInfo(lcIo).noquote() << "open request" << omatrack::displayPath(filePath);
    if (omatrack::isJsonlPath(filePath.toStdString())) {
        overlays_->attachSidecar(filePath, true, false);
        return;
    }
    queueFileOpen(filePath, FileOpenRole::Automatic);
}

bool TelemetryStore::isMtxSidecarPath(const QString& filePath) const {
    return OverlayManager::isMtxSidecarPath(filePath);
}

void TelemetryStore::attachSidecar(const QString& filePath) {
    overlays_->attachSidecar(filePath, false, false);
}

namespace {
QColor sidecarChannelColorFor(const QString& name) {
    static const QColor palette[] = {
        QColor(QStringLiteral("#7fbbb3")), QColor(QStringLiteral("#d699b6")),
        QColor(QStringLiteral("#dbbc7f")), QColor(QStringLiteral("#83c092")),
        QColor(QStringLiteral("#e67e80")), QColor(QStringLiteral("#e09d7f")),
        QColor(QStringLiteral("#a7c080")),
    };
    return palette[qHash(name) % (sizeof(palette) / sizeof(palette[0]))];
}
}  // namespace

bool TelemetryStore::hostWindowNs(qint64* startNs, qint64* endNs,
                                  qint64* utcNs) const {
    return overlays_->hostWindowNs(startNs, endNs, utcNs);
}

bool TelemetryStore::videoClipWindowNs(qint64* startNs, qint64* endNs) const {
    return overlays_->videoClipWindowNs(startNs, endNs);
}

QVariantList TelemetryStore::sidecarLibrary() const {
    return overlays_->sidecarLibrary();
}

void TelemetryStore::removeOverlay(const QString& id) {
    overlays_->removeOverlay(id);
}

void TelemetryStore::setOverlayExpanded(const QString& id, bool expanded) {
    overlays_->setOverlayExpanded(id, expanded);
}

bool TelemetryStore::overlayExpanded(const QString& id) const {
    return overlays_->overlayExpanded(id);
}

const std::vector<double>* TelemetryStore::overlayChannelData(
    const QString& key) const {
    return overlays_->overlayChannelData(key);
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
    pauseSidebarMetadataQueue();
    const QVariantMap metadata =
        recordingMetadataForPath(telemetryPath, prefs_->recordingMetadata());
    const bool expectTelemetry = role != FileOpenRole::Automatic;
    const LibraryLocation* location = connectionHolding(telemetryPath);
    const RemoteConnection remote =
        location ? connectionFor(*location) : RemoteConnection{};
    fileOpenQueue_.enqueue(
        telemetryPath,
        [telemetryPath, metadata, expectTelemetry, remote](IoCancel cancel) {
            return openIndexedFile(telemetryPath, metadata, expectTelemetry,
                                   remote, cancel);
        },
        [this, role,
         wantedLap = lapId](std::shared_ptr<FileOpenResult> result) {
            markRecentlyUsed(result->path);
            qCInfo(lcIo).noquote()
                << "open result" << displayPath(result->path)
                << (result->standaloneVideo ? "standalone"
                    : result->handle        ? "ok"
                                            : "unsupported")
                << (result->error.isEmpty() ? "no-error" : result->error);
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
                if (added) {
                    // A remote video that the sidebar could only list as a
                    // plain file now has laps; let its row catch up.
                    sidebarMetadataLoaded_.insert(result->path);
                    emit sessionsChanged();
                    emit sidebarMetadataChanged(
                        result->path, sidebarFileDetails(result->path));
                }
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
            // fileOpenLoading_ / fileOpenPath_ / lapLoadingChanged and the
            // sidebar resume are driven by fileOpenQueue_.runningChanged;
            // the next queued open is pumped by SerialJobQueue itself.
        });
}

void TelemetryStore::restoreLastSelection() {
    const QString primary = prefs_->lastPrimaryKey();
    const int primaryLap = prefs_->lastPrimaryLap();
    const QString compare = prefs_->lastCompareKey();
    const int compareLap = prefs_->lastCompareLap();
    if (!primary.isEmpty() && QFileInfo::exists(primary))
        queueFileOpen(primary, FileOpenRole::Primary, primaryLap);
    if (!compare.isEmpty() && compare != primary && QFileInfo::exists(compare))
        queueFileOpen(compare, FileOpenRole::Compare, compareLap);
}

void TelemetryStore::rememberRecentFile(const QString& filePath) {
    prefs_->recentFiles().removeAll(filePath);
    prefs_->recentFiles().prepend(filePath);
    while (prefs_->recentFiles().size() > kMaximumRecentFiles)
        prefs_->recentFiles().removeLast();
    schedulePreferencesSave();
    emit recentFilesChanged();
}

void TelemetryStore::clearSessions() {
    sessions_.clear();
    sidebarMetadataQueue_.clear();
    sidebarMetadataQueued_.clear();
    sidebarMetadataLoaded_.clear();
    fileSources_.clear();
    discoveredFilePaths_.clear();
    folderDisplayNames_.clear();
    folderChannelSamples_.clear();
    folderChannelSampleRequests_.clear();
    folderChannelSampleJob_.reset();
    fileMetadata_.clear();
    primarySession_ = nullptr;
    compareSession_ = nullptr;
    primaryLap_ = -1;
    compareLap_ = -1;
    primaryLapJob_.reset();
    compareLapJob_.reset();
    lapPrefetchQueue_.clear();
    invalidateExtraChannelCache();
    setPrimaryLapLoading(false);
    setCompareLapLoading(false);
    deltaCacheValid_ = false;
    damperAlignmentValid_ = false;
    atlas_->clearSpatialMappings();
    setReferenceAlignment(0.0);
    corners_.clear();
    emit selectionChanged();
    emit videoTimeChanged();
    emit cornersChanged();
}

QStringList TelemetryStore::sessionDirectories() const {
    QStringList directories;
    directories.reserve(prefs_->locations().size());
    for (const LibraryLocation& location : std::as_const(prefs_->locations())) {
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
         QLocale().formattedDataSize(prefs_->cacheLimitBytes())},
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
    if (const auto c = videoDownloadJob_.cancel()) c->store(true);
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
    videoDownloadJob_.start(
        [connection, path, received = videoDownloadReceived_,
         total = videoDownloadTotal_](IoCancel cancel) {
            return fetchObject(
                connection, path,
                [received, total, cancel](qint64 got, qint64 declared) {
                    received->store(got);
                    total->store(declared);
                    return !ioCancelled(cancel);
                },
                cancel);
        },
        [this, path](QString failure) {
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
}

void TelemetryStore::clearCache() {
    cancelVideoDownloads();
    // Drop any in-flight scan so its partial result is never published after
    // the cache it scanned is wiped.
    scanJob_.reset();
    clearCacheJob_.start([](IoCancel) { return omatrack::clearCache(); },
                         [this](qint64) {
                             streamUrls_.clear();
                             streamedPaths_.clear();
                             locationStatuses_.clear();
                             locationFileCounts_.clear();
                             emit locationsChanged();
                             scan();
                         });
}

QVariantList TelemetryStore::libraryLocations() const {
    QVariantList rows;
    rows.reserve(prefs_->locations().size());
    for (const LibraryLocation& location : std::as_const(prefs_->locations())) {
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
        location.password = password.isEmpty()
                                ? prefs_->locations()[editing].password
                                : password;
        prefs_->locations()[editing] = location;
    } else {
        location.password = password;
        prefs_->locations().append(location);
    }
    locationStatuses_.remove(location.id);
    locationFileCounts_.remove(location.id);
    schedulePreferencesSave();
    emit locationsChanged();
    scan();
    return {};
}

void TelemetryStore::removeLocation(const QString& id) {
    const int index = locationIndex(id);
    if (index < 0) return;
    // Disconnecting a server should not leave its downloads on the disk
    // forever: nothing can reach them again once the location is gone.
    const QString cache = cachePathFor(prefs_->locations()[index]);
    if (!cache.isEmpty())
        QThreadPool::globalInstance()->start(
            [cache]() { QDir(cache).removeRecursively(); });
    prefs_->locations().remove(index);
    locationStatuses_.remove(id);
    locationFileCounts_.remove(id);
    schedulePreferencesSave();
    emit locationsChanged();
    scan();
}

void TelemetryStore::setLocationEnabled(const QString& id, bool enabled) {
    const int index = locationIndex(id);
    if (index < 0 || prefs_->locations()[index].enabled == enabled) return;
    prefs_->locations()[index].enabled = enabled;
    locationStatuses_.remove(id);
    locationFileCounts_.remove(id);
    schedulePreferencesSave();
    emit locationsChanged();
    scan();
}

void TelemetryStore::setLocationName(const QString& id, const QString& name) {
    const int index = locationIndex(id);
    if (index < 0) return;
    const QString trimmed = name.trimmed();
    if (prefs_->locations()[index].name == trimmed) return;
    prefs_->locations()[index].name = trimmed;
    schedulePreferencesSave();
    emit locationsChanged();
    // Folder display names feed the session tree, so refresh discovery too.
    scan();
}

void TelemetryStore::moveLocation(const QString& id, int delta) {
    const int index = locationIndex(id);
    if (index < 0 || delta == 0) return;
    const int target = std::clamp(
        index + delta, 0, static_cast<int>(prefs_->locations().size()) - 1);
    if (target == index) return;
    prefs_->locations().move(index, target);
    schedulePreferencesSave();
    emit locationsChanged();
    scan();
}

void TelemetryStore::requestSidebarMetadata(const QString& path, bool visible) {
    const QString canonical = canonicalInputPath(path);
    if (canonical.isEmpty() || !visible) return;
    if (findSession(canonical) || sidebarMetadataLoaded_.contains(canonical) ||
        sidebarMetadataQueued_.contains(canonical))
        return;
    sidebarMetadataQueued_.insert(canonical);
    const LibraryLocation* location = connectionHolding(canonical);
    const RemoteConnection remote =
        location ? connectionFor(*location) : RemoteConnection{};
    sidebarMetadataQueue_.enqueue(
        canonical,
        [canonical, remote](IoCancel) {
            return loadSidebarMetadata(canonical, remote);
        },
        [this, canonical](std::shared_ptr<SidebarMetadataResult> result) {
            sidebarMetadataQueued_.remove(canonical);
            const QString& resolved = result ? result->path : canonical;
            if (result) {
                sidebarMetadataLoaded_.insert(resolved);
                fileMetadata_.insert(resolved, result->metadata);
                if (result->handle && !findSession(resolved))
                    sessions_.push_back(std::move(result->handle));
            }
            emit sidebarMetadataChanged(resolved, sidebarFileDetails(resolved));
        });
}

void TelemetryStore::pauseSidebarMetadataQueue() {
    sidebarMetadataQueue_.pause();
}

void TelemetryStore::resumeSidebarMetadataQueue() {
    // Defer to any in-flight file open or lap load: those touch the same
    // parser files and are what the user is actually waiting on.
    if (fileOpenQueue_.busy() || !fileOpenQueue_.empty() ||
        primaryLapJob_.running() || compareLapJob_.running())
        return;
    sidebarMetadataQueue_.resume();
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
    for (int index = 0; index < prefs_->sidebarPins().size(); ++index) {
        const SidebarPin& pin = prefs_->sidebarPins().at(index);
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
        prefs_->sidebarPins().prepend(SidebarPin{kind, normalizedPath});
    } else if (!pinned && existing >= 0) {
        prefs_->sidebarPins().removeAt(existing);
    } else {
        return;
    }
    schedulePreferencesSave();
    emit filePinsChanged();
}

QVariantMap TelemetryStore::sidebarFileDetails(const QString& path) const {
    SessionHandle* session = findSession(path);
    const bool video = isVideoPath(path);
    QVariantMap metadata = fileMetadata_.value(path);
    omatrack::track_metadata::merge(&metadata,
                                    prefs_->recordingMetadata().value(path));
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

QVariantList TelemetryStore::buildFileSourceTree() const {
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
    sources.reserve(fileSources_.size() + usbFileSources_.size() + 2);
    for (const QVariant& source : usbFileSources_)
        sources.append(enrichNode(enrichNode, source.toMap()));
    for (const QVariant& source : fileSources_)
        sources.append(enrichNode(enrichNode, source.toMap()));

    QVariantList recentChildren;
    recentChildren.reserve(prefs_->recentFiles().size());
    for (const QString& path : prefs_->recentFiles()) {
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
    pinnedChildren.reserve(prefs_->sidebarPins().size());
    for (const SidebarPin& pin : prefs_->sidebarPins()) {
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

void TelemetryStore::refreshLibraryModel() {
    if (!libraryModel_) return;
    libraryModel_->setTree(buildFileSourceTree());
}

SessionInfoRow TelemetryStore::sessionInfo(const QString& sessionKey) const {
    SessionInfoRow row;
    SessionHandle* session = findSession(sessionKey);
    if (!session) return row;
    row.name = session->stem();
    row.driver = driverDisplay(session);
    row.track = displayTrack(session);
    row.date = session->date();
    row.lapCount = session->laps().size();
    row.bestLapText = session->bestLapTime();
    return row;
}

QString TelemetryStore::firstSessionKey() const {
    // Walk sessions in the same track→date→session order as the old
    // trackGroups() builder and return the first key.
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
        dateSessions[track][date].append(session->sessionKey());
    }
    std::sort(trackNames.begin(), trackNames.end());
    for (const QString& trackName : trackNames) {
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
            for (const QString& key : dateSessions[trackName][dateName]) {
                SessionHandle* session = findSession(key);
                if (session && !session->laps().isEmpty()) return key;
            }
        }
    }
    return {};
}

QStringList TelemetryStore::sessionKeys() const {
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
        dateSessions[track][date].append(session->sessionKey());
    }
    std::sort(trackNames.begin(), trackNames.end());
    QStringList keys;
    for (const QString& trackName : trackNames) {
        QStringList dateNames = dateSessions[trackName].keys();
        std::sort(dateNames.begin(), dateNames.end(),
                  [](const QString& a, const QString& b) {
                      const QDate da = QDate::fromString(a, "dd/MM/yyyy");
                      const QDate db = QDate::fromString(b, "dd/MM/yyyy");
                      if (da.isValid() && db.isValid() && da != db)
                          return da > db;
                      return a > b;
                  });
        for (const QString& dateName : dateNames)
            for (const QString& key : dateSessions[trackName][dateName])
                keys.append(key);
    }
    return keys;
}

QStringList TelemetryStore::libraryFilePaths() const {
    if (!libraryModel_) return {};
    return libraryModel_->filePaths();
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
    // The directory is created once at startup off the GUI thread (see the
    // constructor); this accessor only computes the path.
    return directory;
}

QString TelemetryStore::localPathFromUrl(const QString& value) const {
    const QString text = value.trimmed();
    if (!text.startsWith(QStringLiteral("file:"), Qt::CaseInsensitive))
        return text;
    return QUrl(text).toLocalFile();
}

QUrl TelemetryStore::defaultTelemetryDirectoryUrl() const {
    return QUrl::fromLocalFile(defaultTelemetryDirectory());
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
        recordingMetadataForPath(session->path(), prefs_->recordingMetadata());
    const QString recordingName =
        driverNameForId(metadata, session->driverId());
    if (!recordingName.isEmpty()) return recordingName;
    const QString mapped =
        prefs_->driverMappings().value(session->driverMappingKey()).trimmed();
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
    const QVariantMap metadata = configuredRecordingMetadataForPath(
        session->path(), prefs_->recordingMetadata());
    const QString recordingSlug =
        nestedText(metadata, {QStringLiteral("track"), QStringLiteral("slug")})
            .toLower();
    if (!recordingSlug.isEmpty()) return recordingSlug;
    QString slug =
        prefs_->trackAssignments().value(trackAssignmentKey(session));
    // Compatibility with the first track-assignment build, which scoped the
    // mapping to a source folder rather than an event date.
    if (slug.isEmpty())
        slug = prefs_->trackAssignments().value(
            QFileInfo(session->path()).absolutePath());
    return slug.trimmed().toLower();
}

QString TelemetryStore::detectedAtlasSlug(const SessionHandle* session) const {
    if (!session || atlas_->tracks().isEmpty()) return {};

    auto nearestGpsTrack = [&](const SessionHandle* candidate) {
        if (!candidate || !candidate->hasGpsLocation()) return QString();
        QString nearestSlug;
        double nearestDistance = std::numeric_limits<double>::max();
        for (auto it = atlas_->tracks().cbegin(); it != atlas_->tracks().cend();
             ++it) {
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
        for (auto it = atlas_->tracks().cbegin(); it != atlas_->tracks().cend();
             ++it) {
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
    for (auto it = atlas_->tracks().cbegin(); it != atlas_->tracks().cend();
         ++it) {
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

    // IMSA stems use short venue codes (IND, RAM) that are unique prefixes
    // of an atlas slug/aka, not exact tokens.
    QSet<QString> prefixMatches;
    for (auto it = atlas_->tracks().cbegin(); it != atlas_->tracks().cend();
         ++it) {
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
            const QString normalized = normalizeAtlasName(name);
            for (const QString& token : filenameTokens) {
                if (token.size() >= 3 && normalized.startsWith(token)) {
                    prefixMatches.insert(it.key());
                    break;
                }
            }
        }
    }
    if (prefixMatches.size() == 1) return *prefixMatches.cbegin();

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
    const QVariantMap metadata = configuredRecordingMetadataForPath(
        session->path(), prefs_->recordingMetadata());
    const QString recordingName =
        nestedText(metadata, {QStringLiteral("track"), QStringLiteral("name")});
    if (!recordingName.isEmpty()) return recordingName;
    const QString slug = resolvedTrackSlug(session);
    if (!slug.isEmpty()) {
        const QJsonObject track = atlas_->tracks().value(slug);
        const QString name = track.value(QStringLiteral("name")).toString();
        return name.isEmpty() ? slug : name;
    }
    return session->track();
}

QVariantList TelemetryStore::trackAtlasChoices() const {
    QVector<QVariantMap> rows;
    rows.reserve(atlas_->tracks().size());
    for (auto it = atlas_->tracks().cbegin(); it != atlas_->tracks().cend();
         ++it) {
        const QString name =
            it.value().value(QStringLiteral("name")).toString(it.key());
        QStringList search{name, it.key()};
        for (const QJsonValue& alias :
             it.value().value(QStringLiteral("aka")).toArray())
            search.append(alias.toString());
        const QJsonObject externalIds =
            it.value().value(QStringLiteral("external_ids")).toObject();
        for (auto external = externalIds.begin(); external != externalIds.end();
             ++external)
            search.append(external.value().toString());
        rows.append(QVariantMap{
            {QStringLiteral("name"), name},
            {QStringLiteral("slug"), it.key()},
            {QStringLiteral("search"), search.join(QLatin1Char(' '))}});
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
    return atlas_->tracks()
        .value(slug)
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
            &effectiveMetadata,
            prefs_->recordingMetadata().value(session->path()));
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
            trackSlug = prefs_->trackAssignments()
                            .value(trackAssignmentKey(session.get()))
                            .trimmed()
                            .toLower();
        if (trackSlug.isEmpty()) trackSlug = detectedAtlasSlug(session.get());
        trackSlugConsensus.add(trackSlug);

        QString trackName =
            nestedText(effectiveMetadata,
                       {QStringLiteral("track"), QStringLiteral("name")});
        if (trackName.isEmpty() && !trackSlug.isEmpty())
            trackName = atlas_->tracks()
                            .value(trackSlug)
                            .value(QStringLiteral("name"))
                            .toString(trackSlug);
        if (trackName.isEmpty()) trackName = session->venue();
        trackNameConsensus.add(trackName);

        const QString driverId = normalizedDriverId(session->driverId());
        if (!driverId.isEmpty()) {
            detectedDriverIds.insert(driverId);
            QString driverName = driverNameForId(effectiveMetadata, driverId);
            if (driverName.isEmpty())
                driverName = prefs_->driverMappings()
                                 .value(session->driverMappingKey())
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
    folderChannelSampleJob_.start(
        [candidates, candidateCount](IoCancel) {
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
        },
        [this, canonical](std::shared_ptr<FolderChannelSample> sample) {
            folderChannelSampleRequests_.remove(canonical);
            folderChannelSamples_.insert(canonical, sample);
            emit folderChannelSampleReady(folderMetadata(canonical));
        },
        &sidebarMetadataPool_);
}

QVariantMap TelemetryStore::videoMetadata(const QString& videoPath) const {
    const QString canonical = canonicalInputPath(videoPath);
    if (canonical.isEmpty() || !isVideoPath(canonical)) return {};

    const SessionHandle* session = findSession(canonical);
    const QVariantMap metadata = prefs_->recordingMetadata().value(canonical);
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

    const QVariantMap document = metadataDocument(metadata);
    const QString sidecarPath = omatrack::track_metadata::filePath(canonical);
    const QString displayName = nestedText(
        document, {QStringLiteral("folder"), QStringLiteral("name")});
    const QString requestedPath = normalizedSidebarPinPath(folderPath);
    const QString canonicalPath = normalizedSidebarPinPath(canonical);

    // Descendant recordings whose inherited metadata must be re-read after
    // the write; collected here so the worker does the file reads.
    QStringList descendants;
    for (auto it = fileMetadata_.cbegin(); it != fileMetadata_.cend(); ++it) {
        const QString relative = QDir(canonical).relativeFilePath(it.key());
        if (relative == QStringLiteral("..") ||
            relative.startsWith(QStringLiteral("../")))
            continue;
        descendants.append(it.key());
    }

    // The document is serialised on the GUI thread; the TRACK.yml write and
    // the hierarchy re-read run on a worker; the completion applies state.
    folderMetadataJob_.start(
        [canonical, document, descendants](IoCancel) {
            FolderMetadataWrite out;
            if (!omatrack::track_metadata::update(canonical, document,
                                                  &out.error))
                return out;
            for (const QString& path : descendants)
                out.refreshed.insert(path,
                                     omatrack::track_metadata::readHierarchy(
                                         QFileInfo(path).absolutePath()));
            return out;
        },
        [this, canonical, sidecarPath, displayName, requestedPath,
         canonicalPath](FolderMetadataWrite result) {
            if (!result.error.isEmpty()) {
                qWarning().noquote() << result.error;
                emit operationError(
                    QStringLiteral("Folder metadata"),
                    QStringLiteral("Could not save TRACK.yml: %1")
                        .arg(result.error));
                return;
            }
            if (displayName.isEmpty()) {
                folderDisplayNames_.remove(requestedPath);
                folderDisplayNames_.remove(canonicalPath);
            } else {
                folderDisplayNames_.insert(requestedPath, displayName);
                folderDisplayNames_.insert(canonicalPath, displayName);
            }
            if (!trackMetadataPaths_.contains(sidecarPath)) {
                trackMetadataPaths_.append(sidecarPath);
                std::sort(trackMetadataPaths_.begin(),
                          trackMetadataPaths_.end());
            }
            for (auto it = result.refreshed.cbegin();
                 it != result.refreshed.cend(); ++it)
                if (fileMetadata_.contains(it.key()))
                    fileMetadata_[it.key()] = it.value();
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
                if (isDescendantVideo(session.get()))
                    session->clearUnifiedCache();
            if (reloadPrimary)
                requestLapLoad(primarySession_, primaryLap, false);
            if (reloadCompare)
                requestLapLoad(compareSession_, compareLap, true);
            emit videoMetadataChanged(canonical);
            emit sessionsChanged();
            emit selectionChanged();
        });
    return true;
}

bool TelemetryStore::saveVideoMetadata(const QString& videoPath,
                                       const QVariantMap& metadata) {
    const QString canonical = canonicalInputPath(videoPath);
    if (canonical.isEmpty() || !isVideoPath(canonical)) return false;

    const QVariantMap document = metadataDocument(metadata);
    if (document.isEmpty())
        prefs_->recordingMetadata().remove(canonical);
    else
        prefs_->recordingMetadata().insert(canonical, document);
    schedulePreferencesSave();

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
    if (!slug.isEmpty() && !atlas_->tracks().contains(slug)) return;

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
            prefs_->trackAssignments().remove(key);
        else
            prefs_->trackAssignments().insert(key, slug);
    }
    // Remove the obsolete folder-scoped value once this event is edited.
    prefs_->trackAssignments().remove(selectedDirectory);
    schedulePreferencesSave();
    if (primarySession_ && keys.contains(trackAssignmentKey(primarySession_))) {
        loadCornersForPrimary();
        emit cornersChanged();
        emit selectionChanged();
    }
    emit sessionsChanged();
}

void TelemetryStore::loadCornersForPrimary() {
    corners_.clear();
    if (!primarySession_) return;

    corners_ = atlas_->cornersForPrimary(primarySession_, primaryLap_,
                                         resolvedTrackSlug(primarySession_));

    const QString assignedSlug = resolvedTrackSlug(primarySession_);
    const QString cornerTrack =
        assignedSlug.isEmpty() ? primarySession_->track() : assignedSlug;
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
            compareLapJob_.reset();
            setCompare(session, lapId);
        } else {
            primaryLapJob_.reset();
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

    if (compare)
        setCompareLapLoading(true);
    else
        setPrimaryLapLoading(true);
    const QString path = session->path();
    const QString parserPath = session->telemetryPath();
    const QString sessionKey = session->sessionKey();
    const LapEntry lap = *wanted;
    const QVariantMap metadata =
        recordingMetadataForPath(path, prefs_->recordingMetadata());
    const bool verifyVideo =
        session->isVideo() &&
        session->videoIdentity().status == VideoIdentityStatus::NotChecked;
    const LibraryLocation* remoteLocation = connectionHolding(path);
    const bool trustedRemote =
        remoteLocation &&
        !cachedObjectEtag(connectionFor(*remoteLocation), path).isEmpty();
    qCInfo(lcIo).noquote() << "unify parse" << omatrack::displayPath(path)
                           << "parser" << omatrack::displayPath(parserPath)
                           << "lap" << lapId;
    auto& job = compare ? compareLapJob_ : primaryLapJob_;
    job.start(
        [parserPath, path, sessionKey, lap, metadata, verifyVideo,
         trustedRemote](IoCancel) {
            return loadSessionLap(parserPath, path, sessionKey, lap, metadata,
                                  verifyVideo, trustedRemote);
        },
        [this, compare](std::shared_ptr<SessionLapLoadResult> result) {
            SessionHandle* session = findSession(result->sessionKey);
            if (!session || !result->error.isEmpty() || !result->source ||
                !result->unified) {
                qWarning() << "Unable to load lap" << result->sessionKey
                           << result->lapId << result->error;
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
                                    result->driverId, result->forceDriverId,
                                    result->videoIdentity);
            if (session->isVideo()) rememberRecentFile(session->path());
            if (compare)
                setCompare(session, result->lapId);
            else
                setPrimary(session, result->lapId);
            resumeSidebarMetadataQueue();
        });
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
    overlays_->setPrimarySession(session);
    overlays_->setPrimaryLap(lapId);
    overlays_->setEventLabel(primaryLabel());
    overlays_->setDriverName(primaryDriverName());
    deltaCacheValid_ = false;
    damperAlignmentValid_ = false;
    invalidateComparisonAlignment();
    rebuildComparisonAlignment();
    prefs_->lastPrimaryKey() = session ? session->sessionKey() : QString();
    prefs_->lastPrimaryLap() = session ? lapId : -1;
    schedulePreferencesSave();
    invalidateExtraChannelCache();
    if (sessionChanged) setReferenceAlignment(0.0);
    cursorFrac_ = 0.0;
    loadCornersForPrimary();
    if (sessionChanged) {
        // Sidecars are global library entries, not session preferences. A
        // session switch starts with no attached overlays and refreshes the
        // matching library for the new host time window.
        overlays_->clear();
        for (auto it = prefs_->channelVisible().begin();
             it != prefs_->channelVisible().end();) {
            if (it.key().startsWith(QStringLiteral("sidecar:")))
                it = prefs_->channelVisible().erase(it);
            else
                ++it;
        }
        for (auto it = prefs_->channelColors().begin();
             it != prefs_->channelColors().end();) {
            if (it.key().startsWith(QStringLiteral("sidecar:")))
                it = prefs_->channelColors().erase(it);
            else
                ++it;
        }
        for (auto it = prefs_->channelWeights().begin();
             it != prefs_->channelWeights().end();) {
            if (it.key().startsWith(QStringLiteral("sidecar:")))
                it = prefs_->channelWeights().erase(it);
            else
                ++it;
        }
        overlays_->discoverSidecarSiblings();
    }
    overlays_->resampleOverlays();
    emit cornersChanged();
    emit videoTimeChanged();
    logSelectedLap("select primary", session, lapId);
}

void TelemetryStore::setCompare(SessionHandle* session, int lapId) {
    const bool sessionChanged = compareSession_ != session;
    if (sessionChanged && compareSession_ && compareSession_ != session &&
        compareSession_ != primarySession_)
        compareSession_->clearUnifiedCache();
    prefs_->lastCompareKey() = session ? session->sessionKey() : QString();
    prefs_->lastCompareLap() = session ? lapId : -1;
    schedulePreferencesSave();
    compareSession_ = session;
    compareLap_ = lapId;
    overlays_->setCompareSession(session);
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

QString TelemetryStore::lapTimeText(const QString& sessionKey,
                                    int lapId) const {
    SessionHandle* session = findSession(sessionKey);
    if (!session) return {};
    for (const LapEntry& lap : session->laps()) {
        if (lap.lapId == lapId) return lap.timeText;
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
        recordingMetadataForPath(path, prefs_->recordingMetadata());
    lapPrefetchQueue_.enqueue(
        sessionKey + QLatin1Char('#') + QString::number(lapId),
        [parserPath, path, sessionKey, lap, metadata](IoCancel) {
            return loadSessionLap(parserPath, path, sessionKey, lap, metadata);
        },
        [this](std::shared_ptr<SessionLapLoadResult> result) {
            if (!result || !result->error.isEmpty() || !result->source ||
                !result->unified)
                return;
            SessionHandle* session = findSession(result->sessionKey);
            if (!session) return;  // session replaced while the prefetch ran
            session->adoptLoadedLap(result->lapId, std::move(result->source),
                                    std::move(result->unified),
                                    result->driverId, result->forceDriverId);
        });
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
    compareLapJob_.reset();
    invalidateExtraChannelCache();
    setCompareLapLoading(false);
    compareSession_ = nullptr;
    compareLap_ = -1;
    overlays_->setCompareSession(nullptr);
    prefs_->lastCompareKey().clear();
    prefs_->lastCompareLap() = -1;
    deltaCacheValid_ = false;
    damperAlignmentValid_ = false;
    invalidateComparisonAlignment();
    setReferenceAlignment(0.0);
    schedulePreferencesSave();
    emit selectionChanged();
    emit cursorFracChanged();
    emit videoTimeChanged();
}

void TelemetryStore::clearPrimary() {
    primaryLapJob_.reset();
    compareLapJob_.reset();
    invalidateExtraChannelCache();
    setPrimaryLapLoading(false);
    setCompareLapLoading(false);
    primarySession_ = nullptr;
    primaryLap_ = -1;
    compareSession_ = nullptr;
    overlays_->setPrimarySession(nullptr);
    overlays_->setPrimaryLap(-1);
    overlays_->setCompareSession(nullptr);
    overlays_->setEventLabel(QString());
    overlays_->setDriverName(QString());
    compareLap_ = -1;
    prefs_->lastPrimaryKey().clear();
    prefs_->lastPrimaryLap() = -1;
    prefs_->lastCompareKey().clear();
    prefs_->lastCompareLap() = -1;
    deltaCacheValid_ = false;
    damperAlignmentValid_ = false;
    invalidateComparisonAlignment();
    corners_.clear();
    schedulePreferencesSave();
    emit selectionChanged();
    emit videoTimeChanged();
    emit cornersChanged();
}

int TelemetryStore::bestLapIdForSession(const QString& sessionKey) const {
    SessionHandle* s = findSession(sessionKey);
    if (!s || s->laps().isEmpty()) return -1;
    for (const LapEntry& l : s->laps())
        if (l.isFastest) return l.lapId;
    for (const LapEntry& l : s->laps())
        if (l.countsForBest()) return l.lapId;
    for (const LapEntry& l : s->laps())
        if (l.isComplete) return l.lapId;
    return s->laps().first().lapId;
}

QVector<LapRow> TelemetryStore::buildLapRows(SessionHandle* session) const {
    QVector<LapRow> rows;
    if (!session) return rows;
    for (const LapEntry& l : session->laps()) {
        LapRow row;
        row.lapId = l.lapId;
        row.label = l.label;
        row.timeText = l.timeText;
        row.timeMs = l.timeMs;
        row.startTime = l.startTime;
        row.isFastest = l.isFastest;
        row.isComplete = l.isComplete;
        row.isPitLap = l.isPitLap;
        row.countsForBest = l.countsForBest();
        rows.append(row);
    }
    return rows;
}

QVector<LapRow> TelemetryStore::lapRowsForSession(
    const QString& sessionKey) const {
    return buildLapRows(findSession(sessionKey));
}

void TelemetryStore::refreshLapModels() {
    primaryLapsModel_->refresh(buildLapRows(primarySession_));
    compareLapsModel_->refresh(buildLapRows(compareSession_));
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
    const bool was = videoMuted();
    videoMutedOverride_.reset();
    if (muted != prefs_->videoMuted()) {
        prefs_->setVideoMuted(muted);
        schedulePreferencesSave();
    }
    if (was != muted) emit videoMutedChanged();
}

void TelemetryStore::overrideVideoMuted(bool muted) {
    if (videoMuted() == muted) {
        videoMutedOverride_ = muted;
        return;
    }
    videoMutedOverride_ = muted;
    emit videoMutedChanged();
}
QString TelemetryStore::effectiveComparisonSyncStrategy() const {
    const UnifiedLap* primary = primaryUnified();
    const UnifiedLap* compare = compareUnified();
    const bool gps = primary && compare &&
                     comparisonGpsAlignmentAvailable(*primary, *compare);
    const bool dampers = primary && compare &&
                         comparisonDamperAlignmentAvailable(*primary, *compare);
    const bool corners = !corners_.isEmpty();
    auto available = [&](const QString& strategy) {
        if (strategy == QStringLiteral("gps-continuous")) return gps;
        if (strategy == QStringLiteral("pre-corner-gps")) return gps && corners;
        if (strategy == QStringLiteral("pre-corner-dampers"))
            return dampers && corners;
        if (strategy == QStringLiteral("manual-dampers")) return dampers;
        return strategy == QStringLiteral("lap-percentage");
    };
    if (available(prefs_->requestedSyncStrategy()))
        return prefs_->requestedSyncStrategy();
    if (gps) return QStringLiteral("gps-continuous");
    if (dampers && corners) return QStringLiteral("pre-corner-dampers");
    if (dampers) return QStringLiteral("manual-dampers");
    return QStringLiteral("lap-percentage");
}

ComparisonAlignmentStrategy TelemetryStore::comparisonAlignmentStrategy(
    const QString& strategy) {
    if (strategy == QStringLiteral("pre-corner-gps"))
        return ComparisonAlignmentStrategy::PreCornerGps;
    if (strategy == QStringLiteral("pre-corner-dampers"))
        return ComparisonAlignmentStrategy::PreCornerDampers;
    if (strategy == QStringLiteral("manual-dampers"))
        return ComparisonAlignmentStrategy::ManualDampers;
    if (strategy == QStringLiteral("lap-percentage"))
        return ComparisonAlignmentStrategy::LapPercentage;
    return ComparisonAlignmentStrategy::GpsContinuous;
}

QVector<SyncStrategyRow> TelemetryStore::buildSyncStrategyRows() const {
    QVector<SyncStrategyRow> rows;
    const UnifiedLap* primary = primaryUnified();
    const UnifiedLap* compare = compareUnified();
    if (!primary || !compare) return rows;
    const bool gps = comparisonGpsAlignmentAvailable(*primary, *compare);
    const bool dampers = comparisonDamperAlignmentAvailable(*primary, *compare);
    auto add = [&](const QString& id, const QString& label,
                   const QString& shortLabel, const QString& detail) {
        SyncStrategyRow row;
        row.id = id;
        row.label = label;
        row.shortLabel = shortLabel;
        row.detail = detail;
        rows.append(row);
    };
    if (gps)
        add(QStringLiteral("gps-continuous"),
            QStringLiteral("GPS · variable speed"),
            QStringLiteral("GPS adaptive"),
            QStringLiteral("Continuously follows matched track position"));
    if (gps && !corners_.isEmpty())
        add(QStringLiteral("pre-corner-gps"),
            QStringLiteral("GPS · pre-corner"), QStringLiteral("GPS turn-in"),
            QStringLiteral("Pins the reference at each turn-in"));
    if (dampers && !corners_.isEmpty())
        add(QStringLiteral("pre-corner-dampers"),
            QStringLiteral("Dampers · pre-corner"),
            QStringLiteral("Damper auto"),
            QStringLiteral("Matches the damper signature before each corner"));
    if (dampers)
        add(QStringLiteral("manual-dampers"),
            QStringLiteral("Dampers · manual"), QStringLiteral("Damper manual"),
            QStringLiteral("Shows both damper traces for manual alignment"));
    add(QStringLiteral("lap-percentage"), QStringLiteral("Lap percentage"),
        QStringLiteral("Lap %"), QStringLiteral("Simple whole-lap fallback"));
    return rows;
}

void TelemetryStore::refreshSyncStrategyModel() {
    syncStrategyModel_->refresh(buildSyncStrategyRows());
}

QString TelemetryStore::comparisonSyncStrategyField(
    const QString& field) const {
    const QString active = effectiveComparisonSyncStrategy_;
    for (const SyncStrategyRow& row : buildSyncStrategyRows()) {
        if (row.id == active) {
            if (field == QStringLiteral("label")) return row.label;
            if (field == QStringLiteral("shortLabel")) return row.shortLabel;
            if (field == QStringLiteral("detail")) return row.detail;
            return row.id;
        }
    }
    return QString();
}

void TelemetryStore::setComparisonSyncStrategy(const QString& strategy) {
    static const QStringList known{
        QStringLiteral("gps-continuous"), QStringLiteral("pre-corner-gps"),
        QStringLiteral("pre-corner-dampers"), QStringLiteral("manual-dampers"),
        QStringLiteral("lap-percentage")};
    if (!known.contains(strategy) ||
        prefs_->requestedSyncStrategy() == strategy)
        return;
    prefs_->requestedSyncStrategy() = strategy;
    if (!qFuzzyIsNull(referenceAlignment_)) {
        referenceAlignment_ = 0.0;
        emit referenceAlignmentChanged();
    }
    schedulePreferencesSave();
    invalidateComparisonAlignment();
    rebuildComparisonAlignment();
    emit cursorFracChanged();
    emit videoTimeChanged();
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
    if (!qFuzzyIsNull(fraction) &&
        effectiveComparisonSyncStrategy_ != QStringLiteral("manual-dampers"))
        return;
    if (qFuzzyCompare(referenceAlignment_ + 1.0, fraction + 1.0)) return;
    referenceAlignment_ = fraction;
    deltaCacheValid_ = false;
    emit referenceAlignmentChanged();
    emit cursorFracChanged();
    emit videoTimeChanged();
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
    schedulePreferencesSave();
}

QString TelemetryStore::cornerName(int index) const {
    if (index < 0 || index >= corners_.size()) return QString();
    return corners_[index].name;
}
QVector<CornerRow> TelemetryStore::buildCornerRows() const {
    QVector<CornerRow> out;
    const UnifiedLap* primary = primaryUnified();
    if (!primary || primary->size() < 2 || primary->distance.size() < 2 ||
        primary->time.size() < 2)
        return out;

    struct Stats {
        double entrySpeed = 0.0, apexSpeed = 0.0, exitSpeed = 0.0;
        double speedDrop = 0.0, speedGain = 0.0, time = 0.0;
        int minGear = 0;
        double maxSteering = 0.0, maxBrake = 0.0, minThrottle = 1.0;
        double brakePoint = 0.0, liftPoint = 0.0;
        double turnInPosition = 0.0, apexPosition = 0.0;
        double throttlePosition = 0.0;
        double turnInPoint = 0.0, apexPoint = 0.0, throttlePoint = 0.0;
        double apexFraction = 0.0;
        double cornerStartPosition = 0.0, cornerEndPosition = 0.0;
        double contextWindowMeters = 0.0, cornerLengthMeters = 0.0;
    };

    auto sample = [](const std::vector<double>& values, double fraction) {
        return omatrack::interpolateFraction(values, fraction);
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

        Stats s;
        s.entrySpeed = sample(lap.speed, corner.start);
        s.apexSpeed = apex;
        s.exitSpeed = sample(lap.speed, corner.end);
        s.speedDrop = s.entrySpeed - apex;
        s.speedGain = s.exitSpeed - apex;
        s.time = sample(lap.time, corner.end) - sample(lap.time, corner.start);
        s.minGear = minGear;
        s.maxSteering = maxSteering;
        s.maxBrake = maxBrake;
        s.minThrottle = minThrottle;
        s.brakePoint = brakePoint;
        s.liftPoint = liftPoint;
        s.turnInPosition = windowPosition(turnInPoint);
        s.apexPosition = windowPosition(apexPoint);
        s.throttlePosition = windowPosition(throttlePoint);
        s.turnInPoint = turnInPoint;
        s.apexPoint = apexPoint;
        s.throttlePoint = throttlePoint;
        s.apexFraction = last > 0 ? double(apexIndex) / double(last) : 0.0;
        s.cornerStartPosition = windowPosition(0.0);
        s.cornerEndPosition = windowPosition(cornerLength);
        s.contextWindowMeters = windowMeters;
        s.cornerLengthMeters = cornerLength;
        return s;
    };
    auto fractionAtDistance = [](const UnifiedLap& lap, double distance) {
        return omatrack::invertFraction(lap.distance, distance);
    };

    const UnifiedLap* compare = compareUnified();
    if (compare && (compare->distance.size() < 2 || compare->time.size() < 2))
        compare = nullptr;
    const QVector<double>& delta = deltaTrace();
    auto deltaAt = [&delta](double fraction) {
        if (delta.isEmpty()) return 0.0;
        return omatrack::interpolateFraction(
            std::vector<double>(delta.begin(), delta.end()), fraction);
    };
    for (int ci = 0; ci < corners_.size(); ++ci) {
        const CornerZone& corner = corners_[ci];
        const Stats primaryStats = stats(*primary, corner);
        CornerRow row;
        row.name = corner.name;
        row.start = corner.start;
        row.end = corner.end;
        row.entrySpeed = primaryStats.entrySpeed;
        row.apexSpeed = primaryStats.apexSpeed;
        row.exitSpeed = primaryStats.exitSpeed;
        row.speedDrop = primaryStats.speedDrop;
        row.speedGain = primaryStats.speedGain;
        row.time = primaryStats.time;
        row.minGear = primaryStats.minGear;
        row.maxSteering = primaryStats.maxSteering;
        row.maxBrake = primaryStats.maxBrake;
        row.minThrottle = primaryStats.minThrottle;
        row.brakePoint = primaryStats.brakePoint;
        row.liftPoint = primaryStats.liftPoint;
        row.turnInPosition = primaryStats.turnInPosition;
        row.apexPosition = primaryStats.apexPosition;
        row.throttlePosition = primaryStats.throttlePosition;
        row.turnInPoint = primaryStats.turnInPoint;
        row.apexPoint = primaryStats.apexPoint;
        row.throttlePoint = primaryStats.throttlePoint;
        row.apexFraction = primaryStats.apexFraction;
        row.cornerStartPosition = primaryStats.cornerStartPosition;
        row.cornerEndPosition = primaryStats.cornerEndPosition;
        row.contextWindowMeters = primaryStats.contextWindowMeters;
        row.cornerLengthMeters = primaryStats.cornerLengthMeters;
        row.hasCompare = compare != nullptr;

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
            auto assignPoint = [](double* target, double metres) {
                if (std::isfinite(metres)) *target = metres;
            };
            assignPoint(&row.brakePoint, context.primaryMetrics.brakePoint);
            assignPoint(&row.liftPoint, context.primaryMetrics.liftPoint);
            assignPoint(&row.turnInPoint, context.primaryMetrics.turnInPoint);
            assignPoint(&row.apexPoint, context.primaryMetrics.apexPoint);
            assignPoint(&row.throttlePoint,
                        context.primaryMetrics.throttlePoint);
            Stats compareStats = stats(*compare, compareCorner);
            assignPoint(&compareStats.brakePoint,
                        context.referenceMetrics.brakePoint);
            assignPoint(&compareStats.liftPoint,
                        context.referenceMetrics.liftPoint);
            assignPoint(&compareStats.turnInPoint,
                        context.referenceMetrics.turnInPoint);
            assignPoint(&compareStats.apexPoint,
                        context.referenceMetrics.apexPoint);
            assignPoint(&compareStats.throttlePoint,
                        context.referenceMetrics.throttlePoint);
            // Map a compare-lap event (metres from that lap's zone start)
            // onto the primary lap's distance axis through the same
            // track-station map traces and delta use. Subtracting each
            // lap's own window metres treats a late event in a short
            // window as equal to an early event in a long one.
            auto compareEventPrimaryFraction = [&](double point) {
                const double absMetres =
                    sample(compare->distance, compareCorner.start) + point;
                return primaryFractionForCompareFraction(
                    fractionAtDistance(*compare, absMetres));
            };
            auto compareEventPrimaryMetres = [&](double point) {
                return sample(primary->distance,
                              compareEventPrimaryFraction(point));
            };
            auto pointDelta = [&](double primaryMetres, double compareMetres) {
                if (!std::isfinite(primaryMetres) ||
                    !std::isfinite(compareMetres))
                    return std::numeric_limits<double>::quiet_NaN();
                return startDistance + primaryMetres -
                       compareEventPrimaryMetres(compareMetres);
            };
            // The reference apex, expressed on the primary lap's distance
            // axis, so both markers land on the same zoomed viewport.
            row.compareApexFraction =
                compareEventPrimaryFraction(compareStats.apexPoint);

            // Time through the corner comes from the one cached delta trace
            // whenever it exists, so the panel, the Δ lane and the cursor
            // readout cannot disagree. Raw corner times are the fallback for
            // laps the track-station map could not align.
            const double timeDelta =
                delta.isEmpty() ? primaryStats.time - compareStats.time
                                : deltaAt(corner.end) - deltaAt(corner.start);
            const double entryDelta =
                primaryStats.entrySpeed - compareStats.entrySpeed;
            const double apexDelta =
                primaryStats.apexSpeed - compareStats.apexSpeed;
            const double exitDelta =
                primaryStats.exitSpeed - compareStats.exitSpeed;
            const double brakePointDelta =
                pointDelta(row.brakePoint, compareStats.brakePoint);
            const double liftPointDelta =
                pointDelta(row.liftPoint, compareStats.liftPoint);
            const double turnInDelta =
                pointDelta(row.turnInPoint, compareStats.turnInPoint);
            const double apexPointDelta =
                pointDelta(row.apexPoint, compareStats.apexPoint);
            const double throttlePointDelta =
                pointDelta(row.throttlePoint, compareStats.throttlePoint);
            // Entry and exit time come from the one cached delta trace, so
            // the corner overlay can never disagree with the Δ lane.
            const double apexFraction = primaryStats.apexFraction;
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

            row.compareEntrySpeed = compareStats.entrySpeed;
            row.compareApexSpeed = compareStats.apexSpeed;
            row.compareExitSpeed = compareStats.exitSpeed;
            row.compareTime = delta.isEmpty() ? compareStats.time
                                              : primaryStats.time - timeDelta;
            row.compareMinGear = compareStats.minGear;
            row.compareMaxSteering = compareStats.maxSteering;
            row.compareMaxBrake = compareStats.maxBrake;
            row.compareMinThrottle = compareStats.minThrottle;
            row.compareBrakePoint = compareStats.brakePoint;
            row.compareLiftPoint = compareStats.liftPoint;
            row.compareTurnInPosition = compareStats.turnInPosition;
            row.compareApexPosition = compareStats.apexPosition;
            row.compareThrottlePosition = compareStats.throttlePosition;
            row.compareTurnInPoint = compareStats.turnInPoint;
            row.compareApexPoint = compareStats.apexPoint;
            row.compareThrottlePoint = compareStats.throttlePoint;
            row.delta = timeDelta;
            row.entryTimeDelta = entryTimeDelta;
            row.exitTimeDelta = exitTimeDelta;
            row.entryDelta = entryDelta;
            row.apexDelta = apexDelta;
            row.exitDelta = exitDelta;
            row.brakePointDelta = brakePointDelta;
            row.liftPointDelta = liftPointDelta;
            row.turnInDelta = turnInDelta;
            row.apexPointDelta = apexPointDelta;
            row.throttlePointDelta = throttlePointDelta;
            row.score = score;
        }
        const std::vector<omatrack::CornerNote> cornerNotes =
            omatrack::CornerAnalysisRegistry::instance().run(context);
        QVariantList notesList;
        QStringList noteParts;
        for (const omatrack::CornerNote& n : cornerNotes) {
            CornerNoteRow note;
            note.id = QString::fromStdString(n.id);
            note.text = QString::fromStdString(n.text);
            note.severity =
                QString::fromLatin1(omatrack::severityName(n.severity));
            notesList.append(QVariant::fromValue(note));
            noteParts << QString::fromStdString(n.text);
        }
        if (cornerNotes.empty() && compare) {
            const QString matched = QStringLiteral("Closely matched");
            CornerNoteRow note;
            note.id = QStringLiteral("matched");
            note.text = matched;
            note.severity = QString::fromLatin1(
                omatrack::severityName(omatrack::NoteSeverity::Info));
            notesList.append(QVariant::fromValue(note));
            noteParts << matched;
        }
        row.notes = notesList;
        row.note = noteParts.join(QStringLiteral(" · "));

        out.append(row);
    }
    return out;
}

void TelemetryStore::refreshCornersModel() {
    cornersModel_->refresh(buildCornerRows());
}

// ── corner focus ────────────────────────────────────────────────────

void TelemetryStore::resetView() {
    if (focusedCorner_ >= 0) {
        focusedCorner_ = -1;
        markers_.clear();
        cornerConsistencyJob_.reset();
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
        recordingMetadataForPath(path, prefs_->recordingMetadata());

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
        lapPrefetchQueue_.enqueue(
            token,
            [parserPath, path, sessionKey, lap, metadata](IoCancel) {
                return loadSessionLap(parserPath, path, sessionKey, lap,
                                      metadata);
            },
            [this, token](std::shared_ptr<SessionLapLoadResult> result) {
                neighbourPrefetch_.remove(token);
                if (!result || !result->error.isEmpty() || !result->source ||
                    !result->unified)
                    return;
                SessionHandle* session = findSession(result->sessionKey);
                if (!session) return;  // session replaced while prefetching
                session->adoptLoadedLap(
                    result->lapId, std::move(result->source),
                    std::move(result->unified), result->driverId,
                    result->forceDriverId);
                // Only the viewport needs to know: this lap is context, not
                // a selection change.
                if (session == primarySession_) emit viewChanged();
            });
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
    cornerConsistencyJob_.reset();
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

CornerFocusSummary TelemetryStore::cornerFocusSummary() const {
    CornerFocusSummary summary;
    if (focusedCorner_ < 0) return summary;
    const QVector<CornerRow> rows = buildCornerRows();
    if (focusedCorner_ >= rows.size()) return summary;

    const CornerRow& row = rows[focusedCorner_];
    // Copy all CornerRow fields into the summary gadget.
    summary.name = row.name;
    summary.start = row.start;
    summary.end = row.end;
    summary.entrySpeed = row.entrySpeed;
    summary.apexSpeed = row.apexSpeed;
    summary.exitSpeed = row.exitSpeed;
    summary.speedDrop = row.speedDrop;
    summary.speedGain = row.speedGain;
    summary.time = row.time;
    summary.minGear = row.minGear;
    summary.maxSteering = row.maxSteering;
    summary.maxBrake = row.maxBrake;
    summary.minThrottle = row.minThrottle;
    summary.brakePoint = row.brakePoint;
    summary.liftPoint = row.liftPoint;
    summary.turnInPosition = row.turnInPosition;
    summary.apexPosition = row.apexPosition;
    summary.throttlePosition = row.throttlePosition;
    summary.turnInPoint = row.turnInPoint;
    summary.apexPoint = row.apexPoint;
    summary.throttlePoint = row.throttlePoint;
    summary.apexFraction = row.apexFraction;
    summary.cornerStartPosition = row.cornerStartPosition;
    summary.cornerEndPosition = row.cornerEndPosition;
    summary.contextWindowMeters = row.contextWindowMeters;
    summary.cornerLengthMeters = row.cornerLengthMeters;
    summary.hasCompare = row.hasCompare;
    summary.compareApexFraction = row.compareApexFraction;
    summary.compareEntrySpeed = row.compareEntrySpeed;
    summary.compareApexSpeed = row.compareApexSpeed;
    summary.compareExitSpeed = row.compareExitSpeed;
    summary.compareTime = row.compareTime;
    summary.compareMinGear = row.compareMinGear;
    summary.compareMaxSteering = row.compareMaxSteering;
    summary.compareMaxBrake = row.compareMaxBrake;
    summary.compareMinThrottle = row.compareMinThrottle;
    summary.compareBrakePoint = row.compareBrakePoint;
    summary.compareLiftPoint = row.compareLiftPoint;
    summary.compareTurnInPosition = row.compareTurnInPosition;
    summary.compareApexPosition = row.compareApexPosition;
    summary.compareThrottlePosition = row.compareThrottlePosition;
    summary.compareTurnInPoint = row.compareTurnInPoint;
    summary.compareApexPoint = row.compareApexPoint;
    summary.compareThrottlePoint = row.compareThrottlePoint;
    summary.delta = row.delta;
    summary.entryTimeDelta = row.entryTimeDelta;
    summary.exitTimeDelta = row.exitTimeDelta;
    summary.entryDelta = row.entryDelta;
    summary.apexDelta = row.apexDelta;
    summary.exitDelta = row.exitDelta;
    summary.brakePointDelta = row.brakePointDelta;
    summary.liftPointDelta = row.liftPointDelta;
    summary.turnInDelta = row.turnInDelta;
    summary.apexPointDelta = row.apexPointDelta;
    summary.throttlePointDelta = row.throttlePointDelta;
    summary.score = row.score;
    summary.notes = row.notes;
    summary.note = row.note;

    summary.consistencyLoading = cornerConsistency_.loading;
    summary.consistencyLapCount = cornerConsistency_.lapCount;
    summary.consistencyValidLapCount = cornerConsistency_.validLapCount;
    summary.consistencyBrakeLapCount = cornerConsistency_.brakingLapCount;
    const bool available = cornerConsistency_.brakingLapCount >= 2 &&
                           std::isfinite(cornerConsistency_.medianBrakePoint) &&
                           std::isfinite(cornerConsistency_.brakePointStdDev) &&
                           std::isfinite(cornerConsistency_.brakePointRange);
    summary.brakeConsistencyAvailable = available;
    if (available) {
        summary.brakePointMedian = cornerConsistency_.medianBrakePoint;
        summary.brakePointStdDev = cornerConsistency_.brakePointStdDev;
        summary.brakePointRange = cornerConsistency_.brakePointRange;
        if (row.maxBrake > 2.0)
            summary.brakePointVsMedian =
                row.brakePoint - cornerConsistency_.medianBrakePoint;
    }
    return summary;
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
    traceConfidenceJob_.reset();  // discard any in-flight confidence build
    traceConfidenceKey_ = key;
    traceConfidenceBands_.clear();
    traceConsistency_.clear();
    traceConfidenceLapIds_.clear();
    traceConfidenceLapCount_ = 0;
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

    const QString path = primarySession_->path();
    const QString parserPath = primarySession_->telemetryPath();
    const QVariantMap metadata =
        recordingMetadataForPath(path, prefs_->recordingMetadata());
    // start() flips traceConfidenceLoading_ to true through runningChanged.
    traceConfidenceJob_.start(
        [parserPath, key, ranked, metadata, primary](IoCancel) {
            return loadSessionConfidence(parserPath, key, ranked, metadata,
                                         primary);
        },
        [this, key](std::shared_ptr<SessionConfidenceLoadResult> result) {
            if (traceConfidenceKey_ != key || !result) return;
            traceConfidenceReady_ = true;
            traceConfidenceLapCount_ = result->lapCount;
            traceConfidenceBands_ = std::move(result->bands);
            traceConsistency_ = std::move(result->consistency);
            if (!result->error.isEmpty())
                qWarning() << "Unable to build session trace confidence"
                           << result->error;
            emit traceConfidenceChanged();
        });
}

void TelemetryStore::requestCornerConsistency() {
    const UnifiedLap* primary = primaryUnified();
    if (focusedCorner_ < 0 || focusedCorner_ >= corners_.size() ||
        !primarySession_ || !primary || primary->distance.size() < 2) {
        cornerConsistencyJob_.reset();
        cornerConsistency_ = {};
        emit cornerConsistencyChanged();
        return;
    }

    const CornerZone& corner = corners_[focusedCorner_];
    const auto sampleDistance = [primary](double fraction) {
        return omatrack::interpolateFraction(primary->distance, fraction);
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

    cornerConsistencyJob_.reset();  // supersede any prior in-flight measurement
    cornerConsistency_ = {};
    cornerConsistency_.key = key;
    cornerConsistency_.loading = !ranked.isEmpty();
    cornerConsistency_.lapCount = ranked.size();
    emit cornerConsistencyChanged();
    if (ranked.isEmpty()) return;

    const QString path = primarySession_->path();
    const QString parserPath = primarySession_->telemetryPath();
    const QVariantMap metadata =
        recordingMetadataForPath(path, prefs_->recordingMetadata());
    cornerConsistencyJob_.start(
        [parserPath, sessionKey, key, ranked, metadata, startDistance,
         endDistance](IoCancel) {
            return loadCornerConsistency(parserPath, sessionKey, key, ranked,
                                         metadata, startDistance, endDistance);
        },
        [this, key](std::shared_ptr<CornerConsistencyLoadResult> result) {
            if (cornerConsistency_.key != key || !result) return;
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
}

// Brake, turn-in, apex and throttle pickup for the focused corner. Both laps
// are placed on the primary lap's distance axis so the zoomed viewport can
// draw them against one x scale.
void TelemetryStore::rebuildCornerMarkers() {
    markers_.clear();
    const UnifiedLap* primary = primaryUnified();
    const QVector<CornerRow> rows =
        focusedCorner_ >= 0 ? buildCornerRows() : QVector<CornerRow>();
    if (focusedCorner_ >= 0 && focusedCorner_ < rows.size() && primary &&
        primary->distance.size() > 1) {
        const std::vector<double>& distance = primary->distance;
        const CornerRow& row = rows[focusedCorner_];
        const bool hasCompare = row.hasCompare;
        const int startIndex = std::clamp(
            int(std::lround(row.start * double(distance.size() - 1))), 0,
            int(distance.size()) - 1);
        const double startDistance = distance[size_t(startIndex)];
        const UnifiedLap* compare = compareUnified();
        auto sampleDistanceOnLap = [](const UnifiedLap& lap, double fraction) {
            return omatrack::interpolateFraction(lap.distance, fraction);
        };
        const double compareStart =
            hasCompare && compare
                ? sampleDistanceOnLap(
                      *compare, compareFractionForPrimaryFraction(row.start))
                : 0.0;
        auto fractionAt = [&distance](double metres) {
            return omatrack::invertFraction(distance, metres);
        };
        auto compareFractionAtMetres = [&](double metres) {
            if (!compare || compare->distance.size() < 2) return 0.0;
            return omatrack::invertFraction(compare->distance, metres);
        };

        // An alias rather than spelling `double CornerRow::*` twice:
        // clang-format 14 (CI) and 22 disagree on the spacing of a
        // pointer-to-member declarator.
        using CornerField = double CornerRow::*;
        struct MarkerSpec {
            const char* key;
            const char* label;
            CornerField primaryField;
            CornerField compareField;
        };
        static constexpr MarkerSpec kSpecs[] = {
            {"brake", "BRAKE", &CornerRow::brakePoint,
             &CornerRow::compareBrakePoint},
            {"turnin", "TURN-IN", &CornerRow::turnInPoint,
             &CornerRow::compareTurnInPoint},
            {"apex", "APEX", &CornerRow::apexPoint,
             &CornerRow::compareApexPoint},
            {"pickup", "THROTTLE", &CornerRow::throttlePoint,
             &CornerRow::compareThrottlePoint}};
        for (const MarkerSpec& spec : kSpecs) {
            CornerMarker marker;
            marker.key = QLatin1String(spec.key);
            marker.label = QLatin1String(spec.label);
            marker.fraction =
                fractionAt(startDistance + row.*spec.primaryField);
            if (hasCompare && compare)
                marker.referenceFraction =
                    primaryFractionForCompareFraction(compareFractionAtMetres(
                        compareStart + row.*spec.compareField));
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
    if (prefs_->channelVisible().contains(key))
        return prefs_->channelVisible().value(key);
    if (key.startsWith(QStringLiteral("sidecar:"))) {
        for (const OverlayGroup& group : overlays_->overlayGroups()) {
            for (const OverlayChannel& channel : group.channels) {
                if (channel.key == key) return channel.defaultVisible;
            }
            for (const OverlaySpanLane& lane : group.spanLanes) {
                if (lane.key == key) return lane.visible;
            }
        }
        return false;
    }
    if (key.startsWith(QStringLiteral("raw:"))) {
        const bool visible = YamlConfig::instance()
                                 .value({QStringLiteral("channels"), key,
                                         QStringLiteral("visible")},
                                        false)
                                 .toBool();
        prefs_->channelVisible().insert(key, visible);
        return visible;
    }
    return false;
}

void TelemetryStore::setChannelVisible(const QString& key, bool visible) {
    if (channelVisible(key) == visible) return;
    prefs_->channelVisible()[key] = visible;
    if (key.startsWith(QStringLiteral("raw:"))) {
        YamlConfig& config = YamlConfig::instance();
        config.setValue(
            {QStringLiteral("channels"), key, QStringLiteral("visible")},
            visible);
        schedulePreferencesSave();
    } else if (!key.startsWith(QStringLiteral("sidecar:"))) {
        schedulePreferencesSave();
    }
    emit channelConfigChanged();
}
const std::vector<double>* TelemetryStore::extraChannelData(const QString& key,
                                                            bool reference) {
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
    AsyncJob<std::shared_ptr<std::vector<double>>>*& job =
        extraChannelJobs_[cacheKey];
    if (!job) job = new AsyncJob<std::shared_ptr<std::vector<double>>>(this);
    job->start(
        [parserPath, rawName, startTime, endTime](IoCancel) {
            // Index-only open: sampleAt() goes through the bridge handle,
            // so there is no reason to decode every channel of the file
            // to read one raw trace.
            std::string error;
            auto source =
                TelemetrySource::openIndex(parserPath.toStdString(), &error);
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
        },
        [this, cacheKey](std::shared_ptr<std::vector<double>> values) {
            extraChannelLoading_.remove(cacheKey);
            if (values) extraChannelCache_.insert(cacheKey, values);
            // One job per (session, lap, channel) would otherwise accumulate
            // for the life of the store; the job has delivered, release it.
            if (auto* finished = extraChannelJobs_.take(cacheKey))
                finished->deleteLater();
            emit channelConfigChanged();
        });
    return nullptr;
}

void TelemetryStore::invalidateExtraChannelCache() {
    for (auto it = extraChannelJobs_.begin(); it != extraChannelJobs_.end();
         ++it)
        it.value()->reset();  // cancel any in-flight raw-channel load
    extraChannelLoading_.clear();
    extraChannelCache_.clear();
}

QVector<ChannelRow> TelemetryStore::buildChannelRows() const {
    QVector<ChannelRow> out;
    for (const QString& key : prefs_->channelOrder()) {
        if (key == QStringLiteral("delta")) continue;
        const auto meta = channelMetadata(key);
        ChannelRow row;
        row.key = key;
        row.title = meta.first;
        row.unit = meta.second;
        row.visible = channelVisible(key);
        row.color = channelColor(key);
        row.weight = channelWeight(key);
        out.append(row);
    }
    if (primarySession_) {
        for (const SourceChannelSummary& channel :
             primarySession_->sourceChannels()) {
            const QString key = QStringLiteral("raw:") + channel.name;
            ChannelRow row;
            row.key = key;
            row.title = channel.name;
            row.unit = channel.unit;
            row.visible = channelVisible(key);
            row.color = channelColor(key);
            row.weight = channelWeight(key);
            row.source = true;
            out.append(row);
        }
    }
    for (const OverlayGroup& group : overlays_->overlayGroups()) {
        for (const OverlaySpanLane& lane : group.spanLanes) {
            ChannelRow row;
            row.key = lane.key;
            row.title = group.name + QStringLiteral(" / ") + lane.name;
            row.visible = channelVisible(lane.key);
            row.color = channelColor(lane.key);
            row.weight = channelWeight(lane.key);
            row.sidecar = true;
            row.span = true;
            out.append(row);
        }
        for (const OverlayChannel& channel : group.channels) {
            ChannelRow row;
            row.key = channel.key;
            row.title = group.name + QStringLiteral(" / ") + channel.name;
            row.unit = channel.unit;
            row.visible = channelVisible(channel.key);
            row.color = channelColor(channel.key);
            row.weight = channelWeight(channel.key);
            row.sidecar = true;
            out.append(row);
        }
    }
    return out;
}

void TelemetryStore::refreshChannelsModel() {
    channelsModel_->refresh(buildChannelRows());
}
QString TelemetryStore::channelExample(const QString& key) {
    const UnifiedLap* lap = primaryUnified();
    if (!lap || lap->size() == 0) return QStringLiteral("—");
    const size_t index =
        std::min(lap->size() - 1,
                 size_t(std::llround(cursorFrac_ * double(lap->size() - 1))));
    const std::vector<double>* values = nullptr;
    if (key.startsWith(QStringLiteral("sidecar:")))
        values = overlayChannelData(key);
    else if (key.startsWith(QStringLiteral("raw:"))) {
        values = extraChannelData(key, false);
        if (!values) {
            const QString rawName = key.mid(4);
            for (const SourceChannelSummary& channel :
                 primarySession_->sourceChannels()) {
                if (channel.name == rawName && !channel.examples.isEmpty())
                    return channel.examples.front();
            }
        }
    } else if (key == QStringLiteral("speed"))
        values = &lap->speed;
    else if (key == QStringLiteral("throttle"))
        values = &lap->throttle;
    else if (key == QStringLiteral("brake"))
        values = &lap->brake;
    else if (key == QStringLiteral("clutch"))
        values = &lap->clutch;
    else if (key == QStringLiteral("steering"))
        values = &lap->steering;
    else if (key == QStringLiteral("distance"))
        values = &lap->distance;
    else if (key == QStringLiteral("g_long"))
        values = &lap->gForceLong;
    else if (key == QStringLiteral("driver_throttle"))
        values = &lap->driverThrottle;
    else if (key == QStringLiteral("gps_lat"))
        values = &lap->gpsLat;
    else if (key == QStringLiteral("gps_lon"))
        values = &lap->gpsLon;
    else if (key == QStringLiteral("gear"))
        return QString::number(lap->gear.empty() ? 0 : lap->gear[index]);
    if (!values || index >= values->size() || !std::isfinite(values->at(index)))
        return QStringLiteral("—");
    const double value = values->at(index);
    if (key == QStringLiteral("throttle") || key == QStringLiteral("clutch") ||
        key == QStringLiteral("driver_throttle"))
        return QStringLiteral("%1%").arg(qRound(value * 100.0));
    if (std::fabs(value) >= 100.0) return QString::number(value, 'f', 0);
    if (std::fabs(value) >= 10.0) return QString::number(value, 'f', 1);
    return QString::number(value, 'f', 2);
}

QString TelemetryStore::channelColor(const QString& key) const {
    if (prefs_->channelColors().contains(key))
        return prefs_->channelColors().value(key).name(QColor::HexRgb);
    if (key.startsWith(QStringLiteral("raw:"))) {
        const QColor fallback = defaultChannelColor(key);
        const QColor parsed(YamlConfig::instance()
                                .value({QStringLiteral("channels"), key,
                                        QStringLiteral("color")},
                                       fallback.name(QColor::HexRgb))
                                .toString());
        const QColor result = parsed.isValid() ? parsed : fallback;
        prefs_->channelColors().insert(key, result);
        return result.name(QColor::HexRgb);
    }
    if (key.startsWith(QStringLiteral("sidecar:"))) {
        const QColor fallback =
            sidecarChannelColorFor(key.section(QLatin1Char(':'), 2));
        prefs_->channelColors().insert(key, fallback);
        return fallback.name(QColor::HexRgb);
    }
    return defaultChannelColor(key).name(QColor::HexRgb);
}

void TelemetryStore::setChannelColor(const QString& key, const QString& color) {
    QColor parsed(color);
    if (!parsed.isValid() || channelColor(key) == parsed.name(QColor::HexRgb))
        return;
    prefs_->channelColors()[key] = parsed;
    if (key.startsWith(QStringLiteral("raw:"))) {
        YamlConfig& config = YamlConfig::instance();
        config.setValue(
            {QStringLiteral("channels"), key, QStringLiteral("color")},
            parsed.name(QColor::HexRgb));
        schedulePreferencesSave();
    } else if (!key.startsWith(QStringLiteral("sidecar:"))) {
        schedulePreferencesSave();
    }
    emit channelConfigChanged();
}

double TelemetryStore::channelWeight(const QString& key) const {
    if (prefs_->channelWeights().contains(key))
        return prefs_->channelWeights().value(key);
    if (key.startsWith(QStringLiteral("raw:"))) {
        const double value = qBound(0.5,
                                    YamlConfig::instance()
                                        .value({QStringLiteral("channels"), key,
                                                QStringLiteral("weight")},
                                               1.0)
                                        .toDouble(),
                                    2.0);
        prefs_->channelWeights().insert(key, value);
        return value;
    }
    return 1.0;
}

void TelemetryStore::setChannelWeight(const QString& key, double weight) {
    weight = qBound(0.5, weight, 2.0);
    if (qFuzzyCompare(channelWeight(key), weight)) return;
    prefs_->channelWeights()[key] = weight;
    if (key.startsWith(QStringLiteral("raw:"))) {
        YamlConfig& config = YamlConfig::instance();
        config.setValue(
            {QStringLiteral("channels"), key, QStringLiteral("weight")},
            weight);
        schedulePreferencesSave();
    } else if (!key.startsWith(QStringLiteral("sidecar:"))) {
        schedulePreferencesSave();
    }
    emit channelConfigChanged();
}

QStringList TelemetryStore::channelOrder() const {
    return prefs_->channelOrder();
}

QVector<DriverMappingRow> TelemetryStore::buildDriverMappingRows() const {
    QVector<DriverMappingRow> out;
    QHash<QString, QString> all = prefs_->driverMappings();
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
        DriverMappingRow row;
        row.key = key;
        row.carNumber = parts.value(0);
        row.carClass = parts.value(1);
        row.driverId = parts.value(2);
        row.display = all.value(key);
        out.append(row);
    }
    return out;
}

void TelemetryStore::refreshDriverMappingsModel() {
    driverMappingsModel_->refresh(buildDriverMappingRows());
}

void TelemetryStore::setDriverMapping(const QString& key,
                                      const QString& display) {
    const QString cleanKey = key.trimmed();
    if (cleanKey.isEmpty()) return;
    if (display.trimmed().isEmpty())
        prefs_->driverMappings().remove(cleanKey);
    else
        prefs_->driverMappings()[cleanKey] = display.trimmed();
    schedulePreferencesSave();
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
        return omatrack::invertFraction(unified->time, relativeTime);
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

CursorReadout TelemetryStore::cursorReadout() const {
    CursorReadout out;
    const UnifiedLap* u = primaryUnified();
    if (!u || u->size() < 1) return out;
    auto sampleAt = [&](const std::vector<double>& arr, double frac) {
        return omatrack::interpolateFraction(arr, frac);
    };
    auto sampleQtAt = [&](const QVector<double>& arr, double frac) {
        if (arr.isEmpty()) return 0.0;
        return omatrack::interpolateFraction(
            std::vector<double>(arr.begin(), arr.end()), frac);
    };
    const double frac = cursorFrac_;
    out.dist = sampleAt(u->distance, frac) -
               (u->distance.empty() ? 0.0 : u->distance.front());
    out.time = sampleAt(u->time, frac);
    out.speed = sampleAt(u->speed, frac);
    out.gear = u->gear.empty()
                   ? 0
                   : u->gear[qBound(0.0, frac, 1.0) * (u->gear.size() - 1)];
    out.corner = cornerNameAt(frac);
    // Δ vs compare lap (array from the shared cached deltaTrace())
    const QVector<double>& d = deltaTrace();
    if (!d.isEmpty()) {
        out.delta = sampleQtAt(d, frac);
        out.hasDelta = true;
    }
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
        const double value = omatrack::interpolateFraction(values, fraction);
        return std::isfinite(value) ? value
                                    : std::numeric_limits<double>::quiet_NaN();
    };

    const double primaryFrac = qBound(0.0, cursorFrac_, 1.0);
    const double compareFrac = compareFractionForPrimaryFraction(primaryFrac);
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

    // One selected alignment map feeds delta, comparison traces, cursor
    // readouts, and video. Δt starts at zero.
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

TraceSnapshot TelemetryStore::traceSnapshot() const {
    TraceSnapshot s;
    s.primary = primaryUnified();
    s.compare = compareUnified();
    s.deltaTrace = &deltaTrace();
    s.viewStart = viewStart_;
    s.viewEnd = viewEnd_;
    s.cursorFrac = cursorFrac_;
    s.corners = &corners_;
    s.markers = &markers_;
    s.neighbourPrev = neighbourUnified(-1);
    s.neighbourNext = neighbourUnified(1);
    qint64 startNs = 0;
    qint64 endNs = 0;
    s.videoClipValid = videoClipWindowNs(&startNs, &endNs);
    s.videoClipStartNs = startNs;
    s.videoClipEndNs = endNs;
    s.compareFractionForPrimaryFraction = [this](double fraction) {
        return compareFractionForPrimaryFraction(fraction);
    };
    return s;
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

    const QVariantMap metadata = recordingMetadataForPath(
        primarySession_->path(), prefs_->recordingMetadata());
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
    for (const LibraryLocation& location : prefs_->locations()) {
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
    const double relativeTime =
        omatrack::interpolateFraction(unified->time, cursorFrac_);
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
    deltaCacheValid_ = false;
}

void TelemetryStore::rebuildComparisonAlignment() {
    comparisonAlignmentTime_.clear();
    comparisonAlignmentFraction_.clear();
    comparisonAlignmentBasis_.clear();
    comparisonGpsAnchors_ = 0;

    const QString effective = effectiveComparisonSyncStrategy();
    if (effectiveComparisonSyncStrategy_ != effective) {
        effectiveComparisonSyncStrategy_ = effective;
        if (effective != QStringLiteral("manual-dampers") &&
            !qFuzzyIsNull(referenceAlignment_)) {
            referenceAlignment_ = 0.0;
            emit referenceAlignmentChanged();
        }
    }

    const UnifiedLap* primary = primaryUnified();
    const UnifiedLap* compare = compareUnified();
    if (!primary || !compare || primary->time.size() < 2 ||
        compare->time.size() < 2) {
        emit comparisonSyncStrategyChanged();
        return;
    }

    ComparisonAlignmentOptions options;
    options.strategy = comparisonAlignmentStrategy(effective);
    options.cornerStarts.reserve(corners_.size());
    for (const CornerZone& corner : corners_)
        options.cornerStarts.append(corner.start);

    // Static for the selected lap pair and strategy. Never rebuild from a
    // paint, cursor-readout, or playback callback.
    ComparisonAlignmentResult result =
        computeComparisonAlignment(*primary, *compare, options);
    comparisonAlignmentTime_ = std::move(result.time);
    comparisonAlignmentFraction_ = std::move(result.fraction);
    comparisonAlignmentBasis_ = std::move(result.basis);
    comparisonGpsAnchors_ = result.gpsAnchors;
    qCInfo(lcIo).noquote() << "comparison alignment"
                           << effectiveComparisonSyncStrategy_
                           << comparisonAlignmentBasis_ << "anchors"
                           << comparisonGpsAnchors_;
    emit comparisonSyncStrategyChanged();
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

double TelemetryStore::comparisonPrimaryFraction(double fraction) const {
    const double shift =
        effectiveComparisonSyncStrategy_ == QStringLiteral("manual-dampers")
            ? referenceAlignment_
            : 0.0;
    return qBound(0.0, fraction - shift, 1.0);
}

double TelemetryStore::compareTimeForPrimaryFraction(double fraction) const {
    const UnifiedLap* primary = primaryUnified();
    if (!primary ||
        comparisonAlignmentTime_.size() != qsizetype(primary->time.size()) ||
        comparisonAlignmentTime_.isEmpty())
        return -1.0;
    return omatrack::interpolateFraction(
        std::vector<double>(comparisonAlignmentTime_.begin(),
                            comparisonAlignmentTime_.end()),
        comparisonPrimaryFraction(fraction));
}

double TelemetryStore::compareFractionForPrimaryFraction(
    double fraction) const {
    return interpolateAlignmentFraction(comparisonAlignmentFraction_,
                                        comparisonPrimaryFraction(fraction));
}

double TelemetryStore::primaryFractionForCompareFraction(
    double fraction) const {
    const double primary =
        invertAlignmentFraction(comparisonAlignmentFraction_, fraction);
    const double shift =
        effectiveComparisonSyncStrategy_ == QStringLiteral("manual-dampers")
            ? referenceAlignment_
            : 0.0;
    return qBound(0.0, primary + shift, 1.0);
}

double TelemetryStore::compareVideoTime() const {
    return compareVideoTimeAtFraction(qBound(0.0, cursorFrac_, 1.0));
}

double TelemetryStore::primaryTimeAtFraction(double fraction) const {
    const UnifiedLap* primary = primaryUnified();
    if (!primary || primary->time.size() < 2) return 0.0;
    return omatrack::interpolateFraction(primary->time, fraction);
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

bool TelemetryStore::trackAtlasReady() const { return !atlas_->isEmpty(); }
QString TelemetryStore::trackAtlasStatus() const { return atlas_->status(); }
bool TelemetryStore::videoMuted() const {
    return videoMutedOverride_.value_or(prefs_->videoMuted());
}
QStringList TelemetryStore::recentFiles() const {
    return prefs_->recentFiles();
}
const QVector<OverlayGroup>& TelemetryStore::overlayGroups() const {
    return overlays_->overlayGroups();
}

#include "IndexCache.h"
#include "LuaRename.h"
#include "PathJail.h"
#include "SwapRoles.h"
#include "UsbMedia.h"

#include <QFileSystemWatcher>
#include <QStorageInfo>

namespace {

QStringList usbWatchRoots() {
    QStringList roots;
    const QString user = QDir::home().dirName();
    const QString runMedia = QStringLiteral("/run/media/") + user;
    if (QFileInfo::exists(runMedia)) roots.append(runMedia);
    if (QFileInfo::exists(QStringLiteral("/media")))
        roots.append(QStringLiteral("/media"));
    return roots;
}

QSet<QString> configuredFolderTargets(
    const QVector<LibraryLocation>& locations) {
    QSet<QString> targets;
    for (const LibraryLocation& location : locations) {
        if (location.type != LocationType::Folder) continue;
        const QString absolute =
            QDir::cleanPath(QFileInfo(location.target).absoluteFilePath());
        if (!absolute.isEmpty()) targets.insert(absolute);
    }
    return targets;
}

QString usbSectionName(const UsbVolume& volume) {
    return QStringLiteral("USB — %1").arg(volume.name);
}

std::shared_ptr<SessionScanResult> scanUsbVolumes(
    const QVector<UsbVolume>& volumes, const QSet<QString>& skipRoots,
    const IoCancel& cancel) {
    auto result = std::make_shared<SessionScanResult>();
    const QStringList filters{
        "*.pds",       "*.PDS",  "*.ld",     "*.LD",  "*.vbo", "*.telemetry",
        "*.TELEMETRY", "*.VBO",  "*.mp4",    "*.MP4", "*.mov", "*.MOV",
        "*.mkv",       "*.MKV",  "*.avi",    "*.AVI", "*.m4v", "*.M4V",
        "*.webm",      "*.WEBM", "TRACK.yml"};
    for (const UsbVolume& volume : volumes) {
        if (ioCancelled(cancel)) break;
        if (skipRoots.contains(volume.rootPath)) continue;
        QSet<QString> sourcePaths;
        QDirIterator it(volume.rootPath, filters, QDir::Files,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) {
            it.next();
            if (it.fileName() == QStringLiteral("TRACK.yml")) continue;
            const QString path = vendorSourcePath(it.filePath());
            if (path.isEmpty()) continue;
            if (isSidecarPath(QDir(volume.rootPath).relativeFilePath(path)))
                continue;
            const QFileInfo resolved(path);
            const QString canonical = resolved.canonicalFilePath().isEmpty()
                                          ? resolved.absoluteFilePath()
                                          : resolved.canonicalFilePath();
            sourcePaths.insert(canonical);
        }
        if (sourcePaths.isEmpty()) continue;
        QVariantMap source = buildFileSource(volume.rootPath, sourcePaths);
        source.insert(QStringLiteral("name"), usbSectionName(volume));
        source.insert(QStringLiteral("transient"), true);
        source.insert(QStringLiteral("usb"), true);
        result->fileSources.append(source);
        result->locationFileCounts.insert(volume.rootPath, sourcePaths.size());
    }
    return result;
}

QVariantMap copyContext(const QString& path, const QString& track,
                        const QString& date, const QString& session,
                        const QString& driver) {
    const QFileInfo info(path);
    return QVariantMap{
        {QStringLiteral("track"),
         track.isEmpty() ? QStringLiteral("unknown") : track},
        {QStringLiteral("date"),
         date.isEmpty() ? QDate::currentDate().toString(Qt::ISODate) : date},
        {QStringLiteral("session"),
         session.isEmpty() ? QStringLiteral("session") : session},
        {QStringLiteral("original"), info.fileName()},
        {QStringLiteral("driver"), driver},
        {QStringLiteral("stem"), info.completeBaseName()},
        {QStringLiteral("ext"), info.suffix()},
    };
}

}  // namespace

void TelemetryStore::setupLibraryWatch() {
    libraryWatch_ = new QFileSystemWatcher(this);
    usbPollTimer_ = new QTimer(this);
    usbPollTimer_->setInterval(3000);
    usbDebounce_ = new QTimer(this);
    usbDebounce_->setSingleShot(true);
    usbDebounce_->setInterval(400);
    connect(usbDebounce_, &QTimer::timeout, this, [this]() {
        if (usbRescanOnly_)
            startUsbScan();
        else
            scan();
        usbRescanOnly_ = false;
    });
    connect(libraryWatch_, &QFileSystemWatcher::directoryChanged, this,
            [this](const QString& path) {
                const QString slash = QDir::fromNativeSeparators(path);
                usbRescanOnly_ =
                    slash.startsWith(QStringLiteral("/run/media/")) ||
                    slash.startsWith(QStringLiteral("/media/"));
                usbDebounce_->start();
            });
    connect(usbPollTimer_, &QTimer::timeout, this, [this]() {
        const auto volumes = mountedUsbVolumes();
        QStringList roots;
        for (const UsbVolume& volume : volumes) roots.append(volume.rootPath);
        if (roots == usbRoots_) return;
        usbRoots_ = roots;
        usbRescanOnly_ = true;
        usbDebounce_->start();
    });
    rebuildLibraryWatch();
    usbPollTimer_->start();
    startUsbScan();
}

void TelemetryStore::rebuildLibraryWatch() {
    if (!libraryWatch_) return;
    const QStringList current = libraryWatch_->directories();
    if (!current.isEmpty()) libraryWatch_->removePaths(current);
    QStringList paths;
    for (const LibraryLocation& location : prefs_->locations()) {
        if (!location.enabled || location.type != LocationType::Folder)
            continue;
        if (QFileInfo(location.target).isDir()) paths.append(location.target);
    }
    paths += usbWatchRoots();
    paths += usbRoots_;
    paths.removeDuplicates();
    if (!paths.isEmpty()) libraryWatch_->addPaths(paths);
}

void TelemetryStore::startUsbScan() {
    const QVector<UsbVolume> volumes = mountedUsbVolumes();
    const QSet<QString> skip = configuredFolderTargets(prefs_->locations());
    usbScanJob_.start(
        [volumes, skip](IoCancel cancel) {
            return scanUsbVolumes(volumes, skip, cancel);
        },
        [this](std::shared_ptr<SessionScanResult> result) {
            finishUsbScan(std::move(result));
        });
}

void TelemetryStore::finishUsbScan(std::shared_ptr<SessionScanResult> result) {
    usbFileSources_.clear();
    usbPresent_ = false;
    usbLabel_.clear();
    if (result) {
        usbFileSources_ = result->fileSources;
        usbPresent_ = !usbFileSources_.isEmpty();
        if (usbPresent_)
            usbLabel_ = usbFileSources_.constFirst()
                            .toMap()
                            .value(QStringLiteral("name"))
                            .toString();
    }
    rebuildLibraryWatch();
    refreshLibraryModel();
    emit usbChanged();
}

bool TelemetryStore::eventMode() const { return prefs_->eventMode(); }
QString TelemetryStore::eventTrack() const { return prefs_->eventTrack(); }
QString TelemetryStore::eventSession() const { return prefs_->eventSession(); }
QString TelemetryStore::eventDate() const { return prefs_->eventDate(); }

void TelemetryStore::setEventMode(bool enabled) {
    if (prefs_->eventMode() == enabled) return;
    prefs_->setEventMode(enabled);
    if (enabled) {
        if (prefs_->eventDate().isEmpty())
            prefs_->setEventDate(QDate::currentDate().toString(Qt::ISODate));
        if (prefs_->eventTrack().isEmpty() && primarySession_) {
            const QString track = displayTrack(primarySession_);
            if (!track.isEmpty()) prefs_->setEventTrack(track);
        }
    }
    schedulePreferencesSave();
    emit eventChanged();
}

void TelemetryStore::setEventTrack(const QString& track) {
    if (prefs_->eventTrack() == track) return;
    prefs_->setEventTrack(track);
    schedulePreferencesSave();
    emit eventChanged();
}

void TelemetryStore::setEventSession(const QString& session) {
    if (prefs_->eventSession() == session) return;
    prefs_->setEventSession(session.trimmed());
    schedulePreferencesSave();
    emit eventChanged();
}

void TelemetryStore::setEventDate(const QString& date) {
    if (prefs_->eventDate() == date) return;
    prefs_->setEventDate(date);
    schedulePreferencesSave();
    emit eventChanged();
}

QString TelemetryStore::usbDest() const { return prefs_->usbDest(); }
QString TelemetryStore::usbFormat() const { return prefs_->usbFormat(); }
QString TelemetryStore::usbRenameScript() const {
    return prefs_->usbRenameScript();
}

void TelemetryStore::setUsbDest(const QString& dest) {
    if (prefs_->usbDest() == dest) return;
    prefs_->setUsbDest(dest);
    schedulePreferencesSave();
    emit usbChanged();
}

void TelemetryStore::setUsbFormat(const QString& format) {
    if (prefs_->usbFormat() == format) return;
    prefs_->setUsbFormat(format);
    schedulePreferencesSave();
    emit usbChanged();
}

void TelemetryStore::setUsbRenameScript(const QString& script) {
    if (prefs_->usbRenameScript() == script) return;
    prefs_->setUsbRenameScript(script);
    schedulePreferencesSave();
    emit usbChanged();
}

QString TelemetryStore::luaRenameExample() const {
    return exampleLuaRenameScript();
}

void TelemetryStore::showUsbCopy() {
    if (!usbPresent_) return;
    usbCopyVisible_ = true;
    emit usbChanged();
}

void TelemetryStore::hideUsbCopy() {
    if (!usbCopyVisible_) return;
    usbCopyVisible_ = false;
    emit usbChanged();
}

void TelemetryStore::copyUsbFiles() {
    if (usbCopyJob_.running() || usbFileSources_.isEmpty()) return;
    const QString dest = prefs_->usbDest().trimmed().isEmpty()
                             ? defaultTelemetryDirectory()
                             : prefs_->usbDest().trimmed();
    const QString format = prefs_->usbFormat().trimmed().isEmpty()
                               ? defaultCopyFormat()
                               : prefs_->usbFormat();
    const QString script = prefs_->usbRenameScript();
    const QString track = prefs_->eventTrack();
    const QString session = prefs_->eventSession();
    const QString date = prefs_->eventDate().isEmpty()
                             ? QDate::currentDate().toString(Qt::ISODate)
                             : prefs_->eventDate();
    QStringList files;
    const auto collect = [&](auto&& self, const QVariantList& nodes) -> void {
        for (const QVariant& node : nodes) {
            const QVariantMap map = node.toMap();
            if (map.value(QStringLiteral("role")).toString() ==
                QStringLiteral("file")) {
                const QString path =
                    map.value(QStringLiteral("path")).toString();
                if (!path.isEmpty()) files.append(path);
            }
            self(self, map.value(QStringLiteral("children")).toList());
        }
    };
    collect(collect, usbFileSources_);
    usbCopyStatus_ = QStringLiteral("Copying…");
    usbCopyProgress_ = 0.0;
    emit usbChanged();
    usbCopyJob_.start(
        [dest, format, script, track, session, date, files](IoCancel cancel) {
            QString error;
            int copied = 0;
            for (int i = 0; i < files.size(); ++i) {
                if (ioCancelled(cancel)) {
                    error = QStringLiteral("Cancelled");
                    break;
                }
                const QString source = files.at(i);
                QVariantMap ctx = copyContext(source, track, date, session, {});
                QString relative = expandCopyFormat(format, ctx);
                if (!script.trimmed().isEmpty()) {
                    const LuaRenameResult lua = runLuaRename(script, ctx);
                    if (!lua.ok) {
                        error = lua.error;
                        break;
                    }
                    if (!lua.relativePath.trimmed().isEmpty())
                        relative = lua.relativePath;
                }
                const PathJailResult jailed = jailRelativePath(dest, relative);
                if (!jailed.ok) {
                    error = jailed.error;
                    break;
                }
                QDir().mkpath(QFileInfo(jailed.absolutePath).absolutePath());
                if (QFileInfo::exists(jailed.absolutePath)) {
                    ++copied;
                    continue;
                }
                if (!QFile::copy(source, jailed.absolutePath)) {
                    error = QStringLiteral("Could not copy %1")
                                .arg(QFileInfo(source).fileName());
                    break;
                }
                ++copied;
            }
            return QVariantMap{{QStringLiteral("error"), error},
                               {QStringLiteral("copied"), copied},
                               {QStringLiteral("dest"), dest}};
        },
        [this](QVariantMap result) {
            const QString error =
                result.value(QStringLiteral("error")).toString();
            usbCopyProgress_ = 1.0;
            if (!error.isEmpty()) {
                usbCopyStatus_ = error;
                emit operationError(QStringLiteral("USB copy"), error);
            } else {
                usbCopyStatus_ =
                    QStringLiteral("Copied %1 files")
                        .arg(result.value(QStringLiteral("copied")).toInt());
                usbCopyVisible_ = false;
                const QString dest =
                    result.value(QStringLiteral("dest")).toString();
                if (!dest.isEmpty()) appendFolderLocation(dest, false);
                scan();
            }
            emit usbChanged();
        });
}

bool TelemetryStore::swapPrimaryWithReference() {
    if (!omatrack::swapRolesPossible(compareSession_ != nullptr)) return false;
    SessionHandle* primary = primarySession_;
    SessionHandle* compare = compareSession_;
    const int primaryLap = primaryLap_;
    const int compareLap = compareLap_;
    primarySession_ = compare;
    primaryLap_ = compareLap;
    compareSession_ = primary;
    compareLap_ = primaryLap;
    overlays_->setPrimarySession(primarySession_);
    overlays_->setPrimaryLap(primaryLap_);
    overlays_->setCompareSession(compareSession_);
    overlays_->setEventLabel(primaryLabel());
    overlays_->setDriverName(primaryDriverName());
    deltaCacheValid_ = false;
    damperAlignmentValid_ = false;
    invalidateComparisonAlignment();
    rebuildComparisonAlignment();
    prefs_->lastPrimaryKey() =
        primarySession_ ? primarySession_->sessionKey() : QString();
    prefs_->lastPrimaryLap() = primaryLap_;
    prefs_->lastCompareKey() =
        compareSession_ ? compareSession_->sessionKey() : QString();
    prefs_->lastCompareLap() = compareLap_;
    schedulePreferencesSave();
    loadCornersForPrimary();
    overlays_->resampleOverlays();
    emit cornersChanged();
    emit videoTimeChanged();
    emit selectionChanged();
    emit cursorFracChanged();
    refreshLapModels();
    libraryModel_->updateSelection(primarySessionKey(), compareSessionKey());
    return true;
}

QString TelemetryStore::overlayRefColor() const {
    return prefs_->overlayRefColor();
}
QString TelemetryStore::overlayRefStyle() const {
    return prefs_->overlayRefStyle();
}
bool TelemetryStore::overlayRefWhite() const {
    return prefs_->overlayRefWhite();
}

void TelemetryStore::setOverlayRefColor(const QString& color) {
    if (prefs_->overlayRefColor() == color) return;
    prefs_->setOverlayRefColor(color);
    schedulePreferencesSave();
    emit overlayStyleChanged();
}

void TelemetryStore::setOverlayRefStyle(const QString& style) {
    if (prefs_->overlayRefStyle() == style) return;
    prefs_->setOverlayRefStyle(style);
    schedulePreferencesSave();
    emit overlayStyleChanged();
}

void TelemetryStore::setOverlayRefWhite(bool enabled) {
    if (prefs_->overlayRefWhite() == enabled) return;
    prefs_->setOverlayRefWhite(enabled);
    schedulePreferencesSave();
    emit overlayStyleChanged();
}
