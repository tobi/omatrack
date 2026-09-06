#include "PreferencesStore.h"
#include "TraceLaneSizing.h"

#include "RemoteCache.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>

namespace {

constexpr int kMaximumRecentFiles = 6;

QString legacyAppDataPath() {
    return QStandardPaths::writableLocation(
               QStandardPaths::GenericDataLocation) +
           QStringLiteral("/racecraft/racecraft-qt");
}

QString normalizedSidebarPinPath(const QString& path) {
    if (path.trimmed().isEmpty()) return {};
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
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

}  // namespace

using namespace omatrack;

PreferencesStore::PreferencesStore(QObject* parent)
    : QObject(parent), saveJob_(this) {
    // Debounced preference writer: serialise on the GUI thread, write via
    // QtConcurrent, surface commit failures through operationError.
    prefsTimer_ = new QTimer(this);
    prefsTimer_->setSingleShot(true);
    prefsTimer_->setInterval(250);
    connect(prefsTimer_, &QTimer::timeout, this, [this]() {
        if (!YamlConfig::instance().isWritable()) return;
        // Never overlap two writes. Both are QSaveFile renames, so whichever
        // commits last wins — and a slow first write could land *after* the
        // newer document. The document stays dirty, and the completion
        // callback below re-arms the timer once the in-flight write lands.
        if (saveJob_.running()) return;
        const QByteArray bytes = YamlConfig::instance().serializeDocument();
        YamlConfig::instance().clearDirty();
        const QString path = YamlConfig::filePath();
        saveJob_.start(
            [bytes, path](IoCancel) {
                QString error;
                YamlConfig::writeDocumentBytes(path, bytes, &error);
                return error;
            },
            [this](QString error) {
                if (!error.isEmpty()) {
                    qWarning().noquote() << error;
                    YamlConfig::instance().markDirty();
                    emit operationError(
                        QStringLiteral("Preferences"),
                        QStringLiteral("Could not save omatrack.yml: %1")
                            .arg(error));
                }
                // If changes arrived during the write, re-arm the debounce.
                if (YamlConfig::instance().isDirty()) prefsTimer_->start();
            });
    });
}

PreferencesStore::~PreferencesStore() {
    flush();
    // saveJob_'s destructor waits for an in-flight write.
}

void PreferencesStore::loadAll(const QString& defaultTelemetryDir) {
    migrateLegacyConfig();
    loadPreferences();
    loadLocations(defaultTelemetryDir);
    loadChannelsConfig();
    if (YamlConfig::instance().isFresh()) scheduleSave();
}

void PreferencesStore::migrateLegacyConfig() {
    // One-time legacy imports, run before loadPreferences() so the normal load
    // path is straight-line. The pre-YAML QSettings store and per-track corner
    // CSVs are read only and left untouched; racecraft.yml is imported by
    // YamlConfig::load() itself (it must populate the document before reads).
    YamlConfig& config = YamlConfig::instance();
    if (config.isFresh()) {
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

    migrateLegacyCornerCsvs();
}

void PreferencesStore::migrateLegacyCornerCsvs() {
    // Per-track corner CSVs from the pre-omatrack.yml era. Scanned once at
    // startup and folded into omatrack.yml so loadCornersForPrimary() never
    // touches the filesystem. A guard flag keeps the scan from repeating.
    YamlConfig& config = YamlConfig::instance();
    if (config
            .value(
                {QStringLiteral("legacy"), QStringLiteral("corners_migrated")})
            .toBool())
        return;

    const QStringList roots{
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation),
        legacyAppDataPath()};
    bool migrated = false;
    for (const QString& root : roots) {
        const QDir directory(root);
        if (!directory.exists()) continue;
        const QStringList csvs = directory.entryList({QStringLiteral("*.csv")},
                                                     QDir::Files, QDir::Name);
        for (const QString& csv : csvs) {
            QString track = csv;
            track.chop(4);  // strip ".csv"
            track.replace('_', ' ');
            const QStringList path = cornerConfigPath(track);
            if (!config.value(path).toList().isEmpty()) continue;
            QFile legacy(root + QLatin1Char('/') + csv);
            if (!legacy.open(QIODevice::ReadOnly)) continue;
            QVariantList zones;
            const auto lines = QString::fromUtf8(legacy.readAll())
                                   .split('\n', Qt::SkipEmptyParts);
            for (int i = 1; i < lines.size(); ++i) {
                const auto parts = lines[i].split(',');
                if (parts.size() < 3) continue;
                const QString name = parts[0].trimmed();
                if (name.isEmpty()) continue;
                zones.append(
                    QVariantMap{{QStringLiteral("name"), name},
                                {QStringLiteral("start"), parts[1].trimmed()},
                                {QStringLiteral("end"), parts[2].trimmed()}});
            }
            if (zones.isEmpty()) continue;
            config.setValue(path, zones);
            migrated = true;
        }
    }
    config.setValue(
        {QStringLiteral("legacy"), QStringLiteral("corners_migrated")}, true);
    if (migrated) config.save();
}

void PreferencesStore::flush() {
    if (prefsTimer_) prefsTimer_->stop();
    // A write already in flight carries an older document; let it land
    // before the synchronous save so it cannot clobber the newer bytes.
    saveJob_.wait();
    YamlConfig::instance().save();
}

void PreferencesStore::loadPreferences() {
    YamlConfig& config = YamlConfig::instance();
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

    bool prunedRecentFiles = false;
    const QStringList configuredRecentFiles =
        config.value(QStringLiteral("recent_files")).toStringList();
    for (const QString& filePath : configuredRecentFiles) {
        // Recent entries are user-facing open targets, not a second index.
        // Drop deleted recordings while loading preferences so the sidebar
        // does not offer an invalid target to the asynchronous opener.
        if (filePath.isEmpty() || !QFileInfo(filePath).isFile()) {
            prunedRecentFiles = true;
            continue;
        }
        if (recentFiles_.contains(filePath)) continue;
        recentFiles_.append(filePath);
        if (recentFiles_.size() == kMaximumRecentFiles) break;
    }
    if (prunedRecentFiles) {
        config.setValue(QStringLiteral("recent_files"), recentFiles_);
        config.save();
    }
    videoMuted_ = config.value(QStringLiteral("video/muted"), false).toBool();
    imageTelemetryEnabled_ =
        config.value(QStringLiteral("video/image_telemetry"), false).toBool();
    imageTelemetryModel_ =
        config.value(QStringLiteral("video/image_model")).toString();
    imageModelManaged_ =
        config.value(QStringLiteral("video/image_model_managed"), false)
            .toBool();
    imageModelUpdates_ =
        config.value(QStringLiteral("video/image_model_updates"), true)
            .toBool();
    const auto hudPosition =
        config.map({QStringLiteral("video"), QStringLiteral("hud_position")});
    bool xOk = false, yOk = false;
    const double hudX =
        hudPosition.value(QStringLiteral("x"), -1.0).toDouble(&xOk);
    const double hudY =
        hudPosition.value(QStringLiteral("y"), -1.0).toDouble(&yOk);
    if (xOk && yOk && std::isfinite(hudX) && std::isfinite(hudY) && hudX >= 0 &&
        hudX <= 1 && hudY >= 0 && hudY <= 1)
        videoHudPosition_ = QPointF(hudX, hudY);
    const QString configuredSyncStrategy =
        config.value(QStringLiteral("video/reference_sync")).toString();
    if (QStringList{
            QStringLiteral("gps-continuous"), QStringLiteral("pre-corner-gps"),
            QStringLiteral("pre-corner-dampers"),
            QStringLiteral("manual-dampers"), QStringLiteral("lap-percentage")}
            .contains(configuredSyncStrategy))
        requestedComparisonSyncStrategy_ = configuredSyncStrategy;
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

    const QVariantMap event = config.map({QStringLiteral("event")});
    eventMode_ = event.value(QStringLiteral("enabled"), false).toBool();
    eventTrack_ = event.value(QStringLiteral("track")).toString();
    eventSession_ = event.value(QStringLiteral("session")).toString();
    eventDate_ = event.value(QStringLiteral("date")).toString();

    const QVariantMap usb = config.map({QStringLiteral("usb")});
    usbDest_ = usb.value(QStringLiteral("dest")).toString();
    usbFormat_ =
        usb.value(QStringLiteral("format"),
                  QStringLiteral("{track}/{date}/{session}/{original}"))
            .toString();
    usbRenameScript_ = usb.value(QStringLiteral("rename_script")).toString();

    const QVariantMap overlay = config.map({QStringLiteral("overlay")});
    overlayRefColor_ =
        overlay.value(QStringLiteral("ref_color"), QStringLiteral("#e09d7f"))
            .toString();
    overlayRefStyle_ =
        overlay.value(QStringLiteral("ref_style"), QStringLiteral("dashed"))
            .toString();
    overlayRefWhite_ =
        overlay.value(QStringLiteral("ref_white"), false).toBool();

    const QVariantMap plugins = config.map({QStringLiteral("plugins")});
    enabledPlugins_ = plugins.value(QStringLiteral("enabled")).toStringList();
    enabledPlugins_.removeDuplicates();

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

void PreferencesStore::loadLocations(const QString& defaultTelemetryDir) {
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
                                     : QStringList{defaultTelemetryDir};
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
                row.value(QStringLiteral("enabled"), true).toBool();
            location.id = row.value(QStringLiteral("id")).toString().trimmed();
            if (location.id.isEmpty())
                location.id = locationId(location.target, location.username);
            if (locationIndex(location.id) < 0)
                locations_.append(std::move(location));
        }
        config.remove({QStringLiteral("telemetry_dirs")});
        config.remove({QStringLiteral("webdav")});
        scheduleSave();
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
        location.enabled = row.value(QStringLiteral("enabled"), true).toBool();
        location.id = row.value(QStringLiteral("id")).toString().trimmed();
        if (location.id.isEmpty())
            location.id = location.type == LocationType::Folder
                              ? locationId(location.target, QString())
                              : locationId(location.target, location.username);
        if (locationIndex(location.id) < 0)
            locations_.append(std::move(location));
    }
    if (rewrite) scheduleSave();
}

int PreferencesStore::locationIndex(const QString& id) const {
    for (int i = 0; i < locations_.size(); ++i)
        if (locations_[i].id == id) return i;
    return -1;
}

bool PreferencesStore::appendFolderLocation(const QString& dirPath,
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

int PreferencesStore::sidebarPinIndex(const QString& kind,
                                      const QString& path) const {
    for (int i = 0; i < sidebarPins_.size(); ++i)
        if (sidebarPins_[i].kind == kind && sidebarPins_[i].path == path)
            return i;
    return -1;
}

void PreferencesStore::scheduleSave() {
    // Rebuild the document from in-memory state, mark dirty, and arm the
    // 250 ms debounce. The timer slot serialises on the GUI thread and writes
    // via QtConcurrent; the destructor/aboutToQuit flushes synchronously.
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
    config.setValue(QStringLiteral("video/image_telemetry"),
                    imageTelemetryEnabled_);
    config.setValue(QStringLiteral("video/image_model"), imageTelemetryModel_);
    config.setValue(QStringLiteral("video/image_model_managed"),
                    imageModelManaged_);
    config.setValue(QStringLiteral("video/image_model_updates"),
                    imageModelUpdates_);
    if (videoHudPosition_.x() >= 0 && videoHudPosition_.y() >= 0)
        config.setMap({QStringLiteral("video"), QStringLiteral("hud_position")},
                      {{QStringLiteral("x"), videoHudPosition_.x()},
                       {QStringLiteral("y"), videoHudPosition_.y()}});
    config.setValue(QStringLiteral("video/reference_sync"),
                    requestedComparisonSyncStrategy_);
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
    config.setMap({QStringLiteral("event")},
                  QVariantMap{{QStringLiteral("enabled"), eventMode_},
                              {QStringLiteral("track"), eventTrack_},
                              {QStringLiteral("session"), eventSession_},
                              {QStringLiteral("date"), eventDate_}});
    config.setMap(
        {QStringLiteral("usb")},
        QVariantMap{{QStringLiteral("dest"), usbDest_},
                    {QStringLiteral("format"), usbFormat_},
                    {QStringLiteral("rename_script"), usbRenameScript_}});
    config.setMap({QStringLiteral("overlay")},
                  QVariantMap{{QStringLiteral("ref_color"), overlayRefColor_},
                              {QStringLiteral("ref_style"), overlayRefStyle_},
                              {QStringLiteral("ref_white"), overlayRefWhite_}});
    config.setMap(
        {QStringLiteral("plugins")},
        QVariantMap{{QStringLiteral("enabled"),
                     enabledPlugins_.isEmpty() ? QVariant()
                                               : QVariant(enabledPlugins_)}});

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
    // Include raw-channel weights too. Resize drafts live in TelemetryStore,
    // not here, so another preference save cannot publish an unsaved resize.
    for (auto it = channelWeights_.cbegin(); it != channelWeights_.cend();
         ++it) {
        if (it.key().startsWith(QStringLiteral("sidecar:"))) continue;
        QVariantMap entry = channels.value(it.key()).toMap();
        // Browsing source channels caches hundreds of default weights. Only
        // persist a raw default when it replaces an existing override.
        if (it.key().startsWith(QStringLiteral("raw:")) &&
            qFuzzyCompare(it.value(), 1.0) &&
            !entry.contains(QStringLiteral("weight")))
            continue;
        entry.insert(QStringLiteral("weight"), it.value());
        channels.insert(it.key(), entry);
    }
    for (auto it = channelAppearance_.cbegin(); it != channelAppearance_.cend();
         ++it) {
        if (it.key().startsWith(QStringLiteral("sidecar:"))) continue;
        QVariantMap entry = channels.value(it.key()).toMap();
        it.value().writeTo(entry);
        channels.insert(it.key(), entry);
    }
    config.setMap({QStringLiteral("channels")}, channels);

    if (prefsTimer_ && !saveJob_.running()) prefsTimer_->start();
}

void PreferencesStore::loadChannelsConfig() {
    // The dialog exposes every UnifiedLap channel; extras start hidden so
    // enabling them never changes the default overview.
    static const char* order[] = {"speed",    "throttle", "brake",
                                  "steering", "gear",     "dampers",
                                  "g_long",   "clutch",   "driver_throttle",
                                  "gps_lat",  "gps_lon",  "delta"};
    channelOrder_ =
        QStringList{order, order + sizeof(order) / sizeof(order[0])};
    YamlConfig& config = YamlConfig::instance();
    QVariantMap channels = config.map({QStringLiteral("channels")});
    bool removedSidecarSettings = false;
    const QStringList keys = channels.keys();
    for (const QString& key : keys) {
        if (!key.startsWith(QStringLiteral("sidecar:"))) continue;
        channels.remove(key);
        removedSidecarSettings = true;
    }
    if (removedSidecarSettings) {
        config.setMap({QStringLiteral("channels")}, channels);
        config.save();
    }
    channelAppearance_.clear();
    for (auto it = channels.cbegin(); it != channels.cend(); ++it)
        channelAppearance_.insert(
            it.key(), ChannelAppearance::fromMap(it.key(), it.value().toMap()));
    for (const QString& k : channelOrder_) {
        const bool defaultVisible = k != "delta" && k != "clutch" &&
                                    k != "driver_throttle" && k != "gps_lat" &&
                                    k != "gps_lon";
        const QVariantMap entry = channels.value(k).toMap();
        channelVisible_[k] =
            entry.value(QStringLiteral("visible"), defaultVisible).toBool();
        const QColor color(
            entry
                .value(QStringLiteral("color"),
                       defaultChannelColor(k).name(QColor::HexRgb))
                .toString());
        channelColors_[k] = color.isValid() ? color : defaultChannelColor(k);
        channelWeights_[k] = trace::validLaneWeight(
            entry.value(QStringLiteral("weight"), 1.0).toDouble());
    }
}

QStringList PreferencesStore::cornerConfigPath(const QString& track) {
    QString key = track.toLower();
    key.replace(' ', '_').replace('-', '_');
    key.remove(QRegularExpression(QStringLiteral("[^a-z0-9_]")));
    if (key.isEmpty()) key = QStringLiteral("unknown");
    return {QStringLiteral("tracks"), key, QStringLiteral("corners")};
}
