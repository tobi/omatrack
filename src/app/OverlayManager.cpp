#include "OverlayManager.h"

#include "core/TelemetryEngine.h"
#include "RemoteCache.h"
#include "VerboseLog.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QStandardPaths>
#include <QTimeZone>

#include <algorithm>
#include <cmath>
#include <limits>
using namespace omatrack;

namespace {

QString overlayGroupId(const QString& path) {
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath().isEmpty()
                                  ? info.absoluteFilePath()
                                  : info.canonicalFilePath();
    return QString::number(qHash(canonical), 16);
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

OverlayManager::OverlayManager(QObject* parent)
    : QObject(parent),
      overlayResampleJob_(this),
      overlayAttachQueue_(this),
      sidecarLibraryQueue_(this),
      sidecarDiscoveryJob_(this) {}

void OverlayManager::clear() {
    // A host change invalidates pending joins/resamples as well as the
    // visible arrays. Old completions must not attach to the new host.
    overlayResampleJob_.reset();
    overlayAttachQueue_.clear();
    sidecarLibraryQueue_.clear();
    sidecarDiscoveryJob_.reset();
    overlays_.clear();
    overlayChannelCache_.clear();
    overlayLoading_.clear();
    sidecarLibrary_.clear();
    sidecarLibraryLoading_.clear();
    emit overlaysChanged();
    emit sidecarLibraryChanged();
    emit channelConfigChanged();
}

bool OverlayManager::isMtxSidecarPath(const QString& filePath) {
    return omatrack::isJsonlExtPath(filePath.toStdString()) ||
           omatrack::isJsonlPath(filePath.toStdString());
}

bool OverlayManager::hostWindowNs(qint64* startNs, qint64* endNs,
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
    const VideoHudSeries* hud = primarySession_
                                    ? (primarySession_->videoHud().empty()
                                           ? nullptr
                                           : &primarySession_->videoHud())
                                    : nullptr;
    if (hud && !hud->empty()) {
        *endNs = std::max(*endNs, qint64(std::llround(hud->duration * 1e9)));
        if (*endNs > *startNs) return true;
    }
    return false;
}

bool OverlayManager::videoClipWindowNs(qint64* startNs, qint64* endNs) const {
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

bool OverlayManager::adoptOverlay(
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
            QString event = eventLabel_;
            if (primarySession_) {
                if (!primarySession_->date().isEmpty()) {
                    if (!event.isEmpty()) event += QStringLiteral(" · ");
                    event += primarySession_->date();
                }
                const QString driver = driverName_;
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

void OverlayManager::attachSidecar(const QString& filePath, bool fromOpen,
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
    overlayAttachQueue_.enqueue(
        path,
        [path, hostUtc](IoCancel) {
            return loadSidecarOverlay(path, hostUtc, nullptr, nullptr);
        },
        [this, path, fromOpen, silent](SidecarLoadResult result) {
            overlayLoading_.remove(path);
            if (result.notExtension) {
                if (fromOpen)
                    emit openAsFile(path);
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
            refreshSidecarLibraryAttachment(result.group.path, true);
            resampleOverlays();
        });
}

void OverlayManager::discoverSidecarSiblings() {
    sidecarLibrary_.clear();
    emit sidecarLibraryChanged();
    if (!primarySession_) return;

    qint64 hostStart = 0;
    qint64 hostEnd = 0;
    qint64 hostUtc = -1;
    if (!hostWindowNs(&hostStart, &hostEnd, &hostUtc)) return;

    // Gather the directories to scan on the GUI thread (cheap: no I/O), then
    // walk them off the GUI thread. The recursive folder scan is the part
    // that must not block a QML click.
    const QString primaryDir =
        QFileInfo(primarySession_->path()).absolutePath();
    const QString telemetryDir =
        primarySession_->telemetryPath() != primarySession_->path()
            ? QFileInfo(primarySession_->telemetryPath()).absolutePath()
            : QString();
    const QString documentsDir =
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    QStringList folderTargets;
    if (locations_) {
        for (const omatrack::LibraryLocation& location : *locations_) {
            if (!location.enabled || location.type != LocationType::Folder)
                continue;
            folderTargets.append(location.target);
        }
    }
    const QString hostPath = primarySession_->path();
    sidecarLibraryQueue_.clear();  // drop stale loads from a previous session
    sidecarLibraryLoading_.clear();
    sidecarDiscoveryJob_.start(
        [primaryDir, telemetryDir, documentsDir, folderTargets](IoCancel) {
            QSet<QString> candidates;
            const auto addCandidates = [&candidates](const QStringList& paths) {
                for (const QString& candidate : paths) {
                    const QString canonical =
                        QFileInfo(candidate).canonicalFilePath();
                    const QString path =
                        canonical.isEmpty()
                            ? QFileInfo(candidate).absoluteFilePath()
                            : canonical;
                    if (omatrack::isJsonlExtPath(path.toStdString()) ||
                        omatrack::isJsonlPath(path.toStdString()))
                        candidates.insert(path);
                }
            };
            addCandidates(listMtxFiles(primaryDir, false));
            if (!telemetryDir.isEmpty())
                addCandidates(listMtxFiles(telemetryDir, false));
            addCandidates(listMtxFiles(documentsDir, false));
            for (const QString& target : folderTargets)
                addCandidates(listMtxFiles(target, true));
            return candidates.values();
        },
        [this, hostPath, hostUtc](QStringList candidates) {
            // Stale check: the primary selection moved on while we walked.
            if (!primarySession_ || primarySession_->path() != hostPath) return;
            for (const QString& path : std::as_const(candidates)) {
                if (sidecarLibraryLoading_.contains(path)) continue;
                sidecarLibraryLoading_.insert(path);
                sidecarLibraryQueue_.enqueue(
                    path,
                    [path, hostUtc](IoCancel) {
                        return loadSidecarOverlay(path, hostUtc, nullptr,
                                                  nullptr);
                    },
                    [this, path, hostPath](SidecarLoadResult result) {
                        sidecarLibraryLoading_.remove(path);
                        if (!primarySession_ ||
                            primarySession_->path() != hostPath ||
                            !result.error.isEmpty() || result.notExtension)
                            return;

                        qint64 hostStart = 0;
                        qint64 hostEnd = 0;
                        qint64 hostUtc = -1;
                        if (!hostWindowNs(&hostStart, &hostEnd, &hostUtc) ||
                            !omatrack::nsRangesOverlap(
                                hostStart, hostEnd, result.group.shiftNs,
                                result.group.shiftNs + result.group.durationNs))
                            return;

                        bool attached = false;
                        for (const OverlayGroup& group : overlays_)
                            if (group.path == result.group.path) {
                                attached = true;
                                break;
                            }
                        sidecarLibrary_.append(QVariantMap{
                            {QStringLiteral("path"), result.group.path},
                            {QStringLiteral("name"), result.group.name},
                            {QStringLiteral("timezone"), result.group.timezone},
                            {QStringLiteral("window"),
                             formatWallWindow(result.group.utcStartNs, 0,
                                              result.group.durationNs,
                                              result.group.timezone)},
                            {QStringLiteral("channelCount"),
                             result.group.channels.size()},
                            {QStringLiteral("spanCount"),
                             result.group.spans.size()},
                            {QStringLiteral("attached"), attached}});
                        std::sort(
                            sidecarLibrary_.begin(), sidecarLibrary_.end(),
                            [](const QVariant& left, const QVariant& right) {
                                const QVariantMap a = left.toMap();
                                const QVariantMap b = right.toMap();
                                const int byName =
                                    a.value(QStringLiteral("name"))
                                        .toString()
                                        .compare(b.value(QStringLiteral("name"))
                                                     .toString(),
                                                 Qt::CaseInsensitive);
                                if (byName != 0) return byName < 0;
                                return a.value(QStringLiteral("path"))
                                           .toString()
                                           .compare(
                                               b.value(QStringLiteral("path"))
                                                   .toString(),
                                               Qt::CaseInsensitive) < 0;
                            });
                        emit sidecarLibraryChanged();
                    });
            }
        });
}

void OverlayManager::refreshSidecarLibraryAttachment(const QString& path,
                                                     bool attached) {
    for (QVariant& value : sidecarLibrary_) {
        QVariantMap row = value.toMap();
        if (row.value(QStringLiteral("path")).toString() != path) continue;
        row.insert(QStringLiteral("attached"), attached);
        value = row;
    }
    emit sidecarLibraryChanged();
}

void OverlayManager::resampleOverlays() {
    if (overlays_.isEmpty()) return;
    const LapEntry* lap = lapEntryFor(primarySession_, primaryLap_);
    const auto unifiedPtr = primarySession_ && primaryLap_ >= 0
                                ? primarySession_->unifiedLap(primaryLap_)
                                : nullptr;
    const UnifiedLap* unified = unifiedPtr.get();
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
    overlayResampleJob_.start(
        [jobs, startTime, times, clipStartNs, clipEndNs](IoCancel) {
            QHash<QString, std::shared_ptr<std::vector<double>>> samples;
            LapEntry lap;
            lap.startTime = startTime;
            UnifiedLap unified;
            unified.time = times;
            for (const Job& job : jobs) {
                std::string error;
                auto source =
                    TelemetrySource::open(job.path.toStdString(), &error);
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
                                   resampleSidecarOntoLap(
                                       *source, channelIndex, lap, unified,
                                       job.shiftNs, clipStartNs, clipEndNs));
                }
            }
            return samples;
        },
        [this](QHash<QString, std::shared_ptr<std::vector<double>>> samples) {
            overlayChannelCache_ = std::move(samples);
            emit channelConfigChanged();
        });
}

void OverlayManager::removeOverlay(const QString& id) {
    for (int index = 0; index < overlays_.size(); ++index) {
        if (overlays_[index].id != id) continue;
        const QString path = overlays_[index].path;
        for (const OverlayChannel& channel : overlays_[index].channels)
            overlayChannelCache_.remove(channel.key);
        overlays_.removeAt(index);
        emit overlaysChanged();
        emit channelConfigChanged();
        refreshSidecarLibraryAttachment(path, false);
        return;
    }
}

void OverlayManager::setOverlayExpanded(const QString& id, bool expanded) {
    for (OverlayGroup& group : overlays_) {
        if (group.id != id) continue;
        if (group.expanded == expanded) return;
        group.expanded = expanded;
        emit overlaysChanged();
        emit channelConfigChanged();
        return;
    }
}

bool OverlayManager::overlayExpanded(const QString& id) const {
    for (const OverlayGroup& group : overlays_) {
        if (group.id == id) return group.expanded;
    }
    return true;
}

const std::vector<double>* OverlayManager::overlayChannelData(
    const QString& key) const {
    auto cached = overlayChannelCache_.constFind(key);
    if (cached == overlayChannelCache_.cend()) return nullptr;
    return cached.value().get();
}
