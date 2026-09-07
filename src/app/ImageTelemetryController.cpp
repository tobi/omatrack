#include "ImageTelemetryController.h"

#include "GaugeReader.h"
#include "GaugeDetectorArtifact.h"
#include "GaugeModelPaths.h"
#include "ImageTelemetryCache.h"
#include "VideoFrameDecoder.h"
#include "TelemetryStore.h"
#include "inference/ImageScanScheduler.h"

#include <QCoreApplication>
#include <QDir>
#include <algorithm>
#include <cmath>
#include <exception>
#include <iterator>
#include <limits>

using namespace omatrack::inference;
using Cache = omatrack::ImageTelemetryCache;
namespace {
constexpr int BatchBudgetMs = 120;
constexpr int BatchLimit = 12;
constexpr int CacheIntervalMs = 1500;
std::size_t slotAt(double seconds, std::size_t count) {
    if (!count || !std::isfinite(seconds) || seconds <= 0) return 0;
    return std::min(count - 1,
                    std::size_t(seconds * 1e9 / ImageTelemetryPeriodNs));
}
bool anyKnown(const ImageTelemetrySlot& slot) {
    return std::any_of(slot.values.begin(), slot.values.end(),
                       [](const auto& value) { return value.has_value(); });
}
}  // namespace

struct ImageTelemetryWorker {
    // Mutated only on the serial pool, never by the GUI or renderer.
    VideoFrameDecoder decoder;
    std::unique_ptr<GaugeReader> reader;
    std::unique_ptr<omatrack::GaugeDetectorRouter> detectorRouter;
    Cache cache;
    std::shared_ptr<ImageTelemetrySeries> draft;
    ImageScanScheduler scheduler;
    QElapsedTimer sinceSave;
    QString cachePath, lastCacheError;
    bool initialized = false, opened = false, dirty = false,
         savedComplete = false;
    std::size_t watchStart = 0, lastCursor = 0;
    int runs = 0, errors = 0;
    ImageTelemetryWorker() { sinceSave.start(); }
};
struct ImageTelemetryResult {
    ImageTelemetrySnapshot series;
    QString message, cachePath, cacheError;
    bool fatal = false, cancelled = false, complete = false,
         cacheComplete = false, dirty = false, watchPending = false;
    int visited = 0, known = 0, runs = 0;
    double inferenceMs = 0, totalMs = 0;
};

struct GaugeDiscoveryResult {
    QVector<omatrack::GaugeRegion> regions;
    QSize size;
    qint64 ptsNs = -1;
    QString error, detectorIdentity, detectorNotice, candidateId;
    double inferenceMs = 0, loadMs = 0;
    bool fatal = false;
};

ImageTelemetryController::ImageTelemetryController(QObject* parent)
    : QObject(parent),
      gauges_(this),
      discoveryJob_(this),
      job_(this),
      disposeJob_(this) {
    workerPool_.setMaxThreadCount(1);
    workerPool_.setExpiryTimeout(-1);
    clock_.start();
    std::fill(std::begin(values_), std::end(values_),
              std::numeric_limits<double>::quiet_NaN());
    timer_.setInterval(200);
    connect(&timer_, &QTimer::timeout, this, &ImageTelemetryController::sample);
    connect(&job_, &AsyncJobBase::runningChanged, this,
            &ImageTelemetryController::scanStateChanged);
    connect(&discoveryJob_, &AsyncJobBase::runningChanged, this,
            &ImageTelemetryController::scanStateChanged);
    timer_.start();
    reset();
}
ImageTelemetryController::~ImageTelemetryController() {
    timer_.stop();
    discoveryJob_.reset();
    job_.reset();
    retireWorker();
    job_.wait();
    discoveryJob_.wait();
    disposeJob_.wait();
    workerPool_.waitForDone();
}
bool ImageTelemetryController::available() const {
    return GaugeReader::runtimeAvailable() && VideoFrameDecoder::available();
}
double ImageTelemetryController::duration() const {
    return series_ ? double(series_->durationNs) / 1e9 : 0;
}
double ImageTelemetryController::progress() const {
    return series_ && !series_->cells.empty()
               ? double(scanned_) / series_->cells.size()
               : 0;
}
void ImageTelemetryController::setPlayer(MpvVideoItem* player) {
    if (player_ == player) return;
    if (player_) disconnect(player_, nullptr, this, nullptr);
    player_ = player;
    if (player_) {
        connect(player_, &MpvVideoItem::sourceChanged, this,
                &ImageTelemetryController::reset);
        connect(player_, &MpvVideoItem::loadedChanged, this,
                &ImageTelemetryController::reset);
        connect(player_, &MpvVideoItem::seekRequested, this, [this]() {
            resetForSeek();
            awaitingSeek_ = true;
        });
        connect(player_, &MpvVideoItem::seekingChanged, this, [this]() {
            if (player_ && player_->seeking()) {
                resetForSeek();
                awaitingSeek_ = true;
            } else
                awaitingSeek_ = false;
        });
        connect(player_, &MpvVideoItem::videoAspectRatioChanged, this,
                &ImageTelemetryController::refreshGeometry);
        connect(player_, &MpvVideoItem::durationChanged, this, [this]() {
            if (series_ && player_ &&
                std::abs(player_->duration() - duration()) > 0.000001)
                reset();
        });
    }
    reset();
    emit playerChanged();
}
void ImageTelemetryController::setEnabled(bool enabled) {
    if (enabled_ == enabled) return;
    enabled_ = enabled;
    reset();
    emit enabledChanged();
    emit scanStateChanged();
}
void ImageTelemetryController::setDiscovering(bool discovering) {
    if (discovering && !enabled_) return;
    if (discovering_ == discovering) return;
    discovering_ = discovering;
    if (discovering) {
        // Fresh cadence so the first frame is sampled now, not up to three
        // seconds from now. Evidence collected before a pause is kept.
        lastDiscoveryTarget_ = -10;
        nextDiscoveryMs_ = 0;
        if (phase_ == Discovery && eligible_)
            setStatus(QStringLiteral(
                "Discover gauges · play video to gather independent frames"));
        emit discoveringChanged();
        sample();
        return;
    }
    discoveryJob_.reset();
    emit discoveringChanged();
    if (phase_ == Discovery) setStatus(idleStatus());
}
QString ImageTelemetryController::idleStatus() const {
    if (!enabled_) return QStringLiteral("Image telemetry off");
    if (!eligible_)
        return QStringLiteral("Native telemetry / no standalone video");
    return QStringLiteral(
        "Gauge discovery off · Discover gauges to scan this video");
}
void ImageTelemetryController::setEligible(bool eligible) {
    if (eligible_ == eligible) return;
    eligible_ = eligible;
    reset();
    emit eligibleChanged();
}
void ImageTelemetryController::setModelPath(const QString& path) {
    if (modelPath_ == path) return;
    modelPath_ = path;
    reset();
    emit modelPathChanged();
}
void ImageTelemetryController::setDetectorPath(const QString& path) {
    if (detectorPath_ == path) return;
    detectorPath_ = path;
    reset();
    emit detectorPathChanged();
}
void ImageTelemetryController::setScanAhead(bool enabled) {
    if (scanAhead_ == enabled ||
        (enabled &&
         (!enabled_ || phase_ != Extracting || blocked_ || complete_)))
        return;
    scanAhead_ = enabled;
    job_.reset();
    reanchor_ = true;  // Also publish/flush work completed just before a pause.
    emit scanStateChanged();
    sample();
}
void ImageTelemetryController::setStatus(const QString& message) {
    if (status_ == message) return;
    status_ = message;
    emit statusChanged();
}
void ImageTelemetryController::invalidate() {
    valid_ = false;
    sampleTime_ = -1;
    sourcePtsNs_ = 0;
    std::fill(std::begin(values_), std::end(values_),
              std::numeric_limits<double>::quiet_NaN());
    emit sampleChanged();
}
void ImageTelemetryController::retireWorker() {
    if (!worker_) return;
    disposeJob_.start(
        [retired = std::move(worker_)](omatrack::IoCancel) mutable {
            // Preserve legitimately collected partial coverage on close/source
            // change. Same serial pool orders this after cancelled inference;
            // identities are revalidated by the atomic cache writer. No source
            // file is written.
            if (retired->draft && retired->dirty &&
                retired->draft->identity.nativeTelemetryAbsent)
                retired->cache.save(*retired->draft);
            retired.reset();
            return 0;
        },
        [](int) {}, &workerPool_);
}
void ImageTelemetryController::clearReading() {
    job_.reset();
    retireWorker();
    worker_ = std::make_shared<ImageTelemetryWorker>();
    series_.reset();
    blocked_ = awaitingSeek_ = complete_ = cacheComplete_ = pendingSave_ =
        pendingWatch_ = scanAhead_ = false;
    scanned_ = known_ = inferenceRuns_ = 0;
    cachePath_.clear();
    reanchor_ = false;
    nextAttemptMs_ = 0;
    invalidate();
    emit timelineChanged();
    emit scanStateChanged();
}
void ImageTelemetryController::reset() {
    discoveryJob_.reset();
    clearReading();
    evidence_.clear();
    discoveryBackend_.clear();
    discoveryMs_ = discoveryLoadMs_ = 0;
    phase_ = Discovery;
    proposalLoaded_ = false;
    geometryCompatible_ = false;
    lastDiscoveryTarget_ = -10;
    nextDiscoveryMs_ = 0;
    // Discovery is a per-video decision: a new source, a reopen or a settings
    // change never carries the opt-in over.
    if (discovering_) {
        discovering_ = false;
        emit discoveringChanged();
    }
    refreshGauges();
    setStatus(idleStatus());
}
void ImageTelemetryController::resetForSeek() {
    // A seek invalidates current readings, NOT already collected source data.
    discoveryJob_.reset();
    lastDiscoveryTarget_ = -10;
    nextDiscoveryMs_ = 0;
    job_.reset();
    reanchor_ = true;
    nextAttemptMs_ = 0;
    invalidate();
}
TelemetryStore* ImageTelemetryController::store() const { return store_; }
void ImageTelemetryController::setStore(TelemetryStore* store) {
    if (store_ == store) return;
    store_ = store;
    reset();
    emit storeChanged();
}
bool ImageTelemetryController::canConfirm() const {
    if (!enabled_ || !geometryCompatible_ || phase_ != Discovery ||
        !evidence_.setup.valid())
        return false;
    bool selected = false;
    for (const auto& r : evidence_.setup.regions) {
        if (!r.enabled || !r.visible()) continue;
        selected = true;
        if (r.hits < omatrack::GaugeSetup::VotesToConfirm && !r.confirmed)
            return false;
    }
    return selected;
}
bool ImageTelemetryController::canExtract() const {
    if (!enabled_ || !eligible_ || !geometryCompatible_ ||
        phase_ != Confirmed || !available() ||
        !evidence_.reviewedLayoutVerified())
        return false;
    const auto fields = evidence_.setup.readableFields();
    return std::any_of(fields.begin(), fields.end(),
                       [](bool value) { return value; });
}
void ImageTelemetryController::refreshGeometry() {
    const auto size = evidence_.setup.sourceSize;
    const double aspect = player_ ? player_->videoAspectRatio() : 0;
    const bool compatible =
        size.height() > 0 && aspect > 0 &&
        std::abs(double(size.width()) / size.height() - aspect) < .02;
    if (geometryCompatible_ == compatible) return;
    geometryCompatible_ = compatible;
    if (!compatible && phase_ == Extracting) {
        job_.reset();
        invalidate();
        setStatus(QStringLiteral(
            "Source/display aspect mismatch · extraction withheld"));
    }
    emit setupChanged();
    if (compatible &&
        status_.startsWith(QStringLiteral("Source/display aspect mismatch")))
        setStatus(QStringLiteral(
            "Source geometry revalidated · review and confirm setup"));
}
void ImageTelemetryController::refreshGauges() {
    QVector<GaugeRegionRow> rows;
    for (const auto& r : evidence_.setup.regions) {
        // Hidden candidates keep voting in the evidence but never reach the
        // panel or the video until they have earned their place.
        if (!r.visible()) continue;
        GaugeRegionRow row;
        row.key = r.id;
        row.origin = !r.profileKey.isEmpty() ? QStringLiteral("AiM profile")
                     : r.detectorIdentity.contains("aim-large-v1")
                         ? QStringLiteral("AiM detector")
                     : r.detectorIdentity.contains("tiny-v2")
                         ? QStringLiteral("Tiny detector")
                     : experimentalDetector() ? QStringLiteral("Experimental")
                                              : QStringLiteral("Proposal");
        row.semantic = r.semantic;
        row.representation = r.representation;
        row.direction = r.direction;
        row.box = r.box;
        row.selected = r.enabled;
        row.confirmed = r.confirmed;
        row.evidence = r.hits;
        row.readable = omatrack::GaugeSetup::readerField(
                           r, evidence_.setup.sourceSize) >= 0;
        row.support =
            row.readable
                ? QStringLiteral("Reviewed crop · visible values only")
                : QStringLiteral(
                      "Unsupported reader crop/type · remains unknown");
        if (!r.profileKey.isEmpty())
            row.support.prepend(
                QStringLiteral("Reviewed AiM profile (image structure, not "
                               "learned detection) · "));
        if (r.misses >= 2)
            row.support +=
                QStringLiteral(" · stale: missed on %1 independent frames")
                    .arg(r.misses);
        rows.append(row);
    }
    gauges_.refresh(rows);
    emit setupChanged();
}
void ImageTelemetryController::confirmGauge(const QString& key) {
    if (!enabled_ || phase_ != Discovery || evidence_.sampleCount() == 0)
        return;
    for (auto& r : evidence_.setup.regions)
        if (r.id == key) {
            r.confirmed = true;  // explicit visual user confirmation, not model
                                 // confidence
            r.proposal = true;
        }
    refreshGauges();
}
void ImageTelemetryController::confirmSetup(bool extensionDefault) {
    if (!canConfirm()) return;
    discoveryJob_.reset();
    // Only tracks that earned visibility are confirmed and saved; one-frame
    // candidates and suppressed losers are noise, not setup.
    evidence_.setup.pruneInvisible();
    for (auto& r : evidence_.setup.regions) {
        r.confirmed = r.enabled;
        r.proposal = true;
    }
    phase_ = Confirmed;
    if (store_ && player_ && player_->source().isLocalFile())
        store_->saveGaugeSetup(player_->source().toLocalFile(), evidence_.setup,
                               extensionDefault);
    refreshGauges();
    setStatus(
        !eligible_ ? QStringLiteral("Setup saved · native telemetry takes "
                                    "priority; image extraction withheld")
        : canExtract()
            ? QStringLiteral("Setup confirmed · Start extraction when ready")
        : !evidence_.reviewedLayoutVerified()
            ? QStringLiteral("Setup saved · reviewed AiM layout is not "
                             "verified in fresh source frames")
            : QStringLiteral("Setup saved · selected gauges are not supported "
                             "by this reader"));
}
void ImageTelemetryController::startExtraction() {
    if (!canExtract()) return;
    discoveryJob_.reset();
    clearReading();
    phase_ = Extracting;
    emit setupChanged();
    setStatus(QStringLiteral("Loading confirmed-setup cache…"));
    sample();
}
void ImageTelemetryController::reviewSetup() {
    discoveryJob_.reset();
    clearReading();
    phase_ = Discovery;
    for (auto& r : evidence_.setup.regions) r.confirmed = false;
    refreshGauges();
    setStatus(
        QStringLiteral("Review gauges · confirm again before extraction"));
}
void ImageTelemetryController::editGauge(const QString& key,
                                         const QString& semantic, bool selected,
                                         const QRectF& box,
                                         const QString& representation,
                                         const QString& direction) {
    if (!enabled_ || !evidence_.setup.valid()) return;
    auto candidate = evidence_.setup;
    auto it = std::find_if(candidate.regions.begin(), candidate.regions.end(),
                           [&key](const auto& r) { return r.id == key; });
    if (it == candidate.regions.end() || semantic.isEmpty() ||
        semantic.size() > 64)
        return;
    const bool changed =
        it->semantic != semantic || it->box != box ||
        (!representation.isEmpty() && representation != it->representation) ||
        (!direction.isEmpty() && direction != it->direction);
    it->semantic = semantic;
    if (!representation.isEmpty()) it->representation = representation;
    if (!direction.isEmpty()) it->direction = direction;
    it->enabled = selected;
    it->box = box;
    it->edited = true;
    if (changed) it->hits = 0;
    for (auto& r : candidate.regions) r.confirmed = false;
    if (!candidate.valid()) return;
    discoveryJob_.reset();
    clearReading();
    evidence_.setup = std::move(candidate);
    phase_ = Discovery;
    nextDiscoveryMs_ = 0;
    refreshGauges();
    setStatus(
        QStringLiteral("Setup edited · revalidate and confirm; unsupported "
                       "crops stay unknown"));
}
void ImageTelemetryController::discover(double seconds) {
    if (blocked_ || discoveryJob_.running() ||
        clock_.elapsed() < nextDiscoveryMs_ || !std::isfinite(seconds) ||
        seconds < 0 || std::abs(seconds - lastDiscoveryTarget_) < 2.0)
        return;
    if (!player_->source().isLocalFile()) {
        blocked_ = true;
        setStatus(
            QStringLiteral("Discovery requires a local/downloaded video"));
        return;
    }
    const auto source = player_->source().toLocalFile();
    if (!proposalLoaded_) {
        if (store_) evidence_.propose(store_->gaugeProposal(source));
        evidence_.setup.detectorIdentity =
            QStringLiteral("orange-structure-heuristic-v1");
        proposalLoaded_ = true;
        refreshGauges();
    }
    const auto state = worker_;
    const auto detectorPath = detectorPath_;
    const auto modelDirectory = omatrack::gaugeModelRoot();
    lastDiscoveryTarget_ = seconds;
    nextDiscoveryMs_ = clock_.elapsed() + 3000;
    discoveryJob_.start(
        [state, source, seconds, detectorPath,
         modelDirectory](omatrack::IoCancel cancel) {
            auto result = std::make_shared<GaugeDiscoveryResult>();
            try {
                if (!state->opened) {
                    if (!state->decoder.open(source.toStdString(), cancel)) {
                        result->error =
                            QString::fromStdString(state->decoder.error());
                        return result;
                    }
                    state->opened = true;
                }
                DecodedRgbFrame frame;
                if (!state->decoder.frameAtOrAfter(std::llround(seconds * 1e9),
                                                   frame, cancel)) {
                    result->error =
                        QString::fromStdString(state->decoder.error());
                    return result;
                }
                if (cancel->load()) return result;
                result->size = QSize(frame.width, frame.height);
                result->ptsNs = frame.presentationPtsNs;
                const GaugeRgb24Frame pixels{frame.pixels.data(),
                                             frame.pixels.size(), frame.width,
                                             frame.height, frame.stride};
                if (!state->detectorRouter)
                    state->detectorRouter =
                        std::make_unique<omatrack::GaugeDetectorRouter>(
                            detectorPath, modelDirectory);
                const auto detected =
                    state->detectorRouter->detect(pixels, cancel);
                if (detected.cancelled || cancel->load()) return result;
                result->detectorIdentity = detected.identity;
                result->detectorNotice = detected.notice;
                result->candidateId = detected.candidateId;
                result->inferenceMs = detected.result.latencyMs;
                result->loadMs = detected.loadMs;
                for (const auto& d : detected.result.detections) {
                    omatrack::GaugeRegion region;
                    region.box = {d.bbox[0], d.bbox[1], d.bbox[2] - d.bbox[0],
                                  d.bbox[3] - d.bbox[1]};
                    region.semantic = QString::fromStdString(d.semantic);
                    region.representation =
                        QString::fromStdString(d.representation);
                    region.detectorIdentity = detected.identity;
                    region.score = d.score;
                    region.enabled = false;
                    result->regions.append(region);
                }
                // This independent image check remains available even when
                // learned detection succeeds. Its exact crops are separate
                // profile anchors, not claimed as detector localizations.
                if (detected.reviewedAim) {
                    result->regions = omatrack::GaugeSetup::reviewedRegions() +
                                      result->regions;
                    result->detectorNotice += QStringLiteral(
                        " · image-verified reviewed AiM profile");
                }
            } catch (const std::exception&) {
                result->error =
                    QStringLiteral("Discovery failed; video remains available");
            }
            return result;
        },
        [this](const std::shared_ptr<GaugeDiscoveryResult>& result) {
            if (!result->error.isEmpty()) {
                geometryCompatible_ = false;
                emit setupChanged();
                blocked_ = result->fatal;
                setStatus(result->error);
                return;
            }
            evidence_.setup.detectorIdentity = result->detectorIdentity;
            discoveryBackend_ = result->candidateId;
            discoveryMs_ = result->inferenceMs;
            discoveryLoadMs_ = result->loadMs;
            if (!evidence_.observe(result->ptsNs, result->size,
                                   result->regions))
                return;
            refreshGeometry();
            refreshGauges();
            if (!geometryCompatible_) {
                setStatus(QStringLiteral(
                    "Source/display aspect mismatch · boxes and extraction "
                    "withheld; revalidate source geometry"));
                return;
            }
            setStatus(
                QStringLiteral("%1 · %2 independent frames · %3")
                    .arg(result->detectorNotice)
                    .arg(evidence_.sampleCount())
                    .arg(canConfirm()
                             ? QStringLiteral("review and confirm gauges")
                             : QStringLiteral(
                                   "keep playing to validate proposals")));
        },
        &workerPool_);
}
void ImageTelemetryController::retry() { reset(); }
void ImageTelemetryController::refreshCurrent() {
    if (!player_ || !series_ || series_->cells.empty() || awaitingSeek_ ||
        player_->seeking())
        return;
    const auto& point =
        series_->cells[slotAt(player_->position(), series_->cells.size())];
    const double stamp =
        point.presentationPtsNs ? double(*point.presentationPtsNs) / 1e9 : -1;
    const bool valid =
        point.visited && anyKnown(point) && stamp <= player_->position() + 0.06;
    std::array<double, 4> values;
    for (std::size_t i = 0; i < values.size(); ++i)
        values[i] = valid ? point.values[i].value_or(
                                std::numeric_limits<double>::quiet_NaN())
                          : std::numeric_limits<double>::quiet_NaN();
    bool changed = valid_ != valid || sampleTime_ != stamp;
    for (std::size_t i = 0; i < values.size(); ++i)
        changed = changed ||
                  (std::isfinite(values[i]) != std::isfinite(values_[i])) ||
                  (std::isfinite(values[i]) && values[i] != values_[i]);
    valid_ = valid;
    sampleTime_ = stamp;
    sourcePtsNs_ = point.sourcePtsNs.value_or(0);
    std::copy(values.begin(), values.end(), values_);
    if (changed) emit sampleChanged();
}

void ImageTelemetryController::sample() {
    if (!enabled_ || !player_ || !player_->loaded() || player_->seeking() ||
        awaitingSeek_)
        return;
    if (phase_ != Extracting) {
        if (phase_ == Discovery && discovering_) discover(player_->position());
        return;
    }
    if (!eligible_ || !geometryCompatible_)
        return;  // No native replacement or misprojected reads.
    refreshCurrent();
    if (blocked_ || job_.running() || clock_.elapsed() < nextAttemptMs_ ||
        cacheComplete_)
        return;
    if (!player_->source().isLocalFile()) {
        blocked_ = true;
        setStatus(QStringLiteral(
            "Image extraction requires a local/downloaded video"));
        return;
    }
    const double seconds = player_->position(), length = player_->duration();
    if (!std::isfinite(seconds) || !std::isfinite(length) || seconds < 0 ||
        length <= 0)
        return;
    if (length > 86400) {
        blocked_ = true;
        scanAhead_ = false;
        setStatus(QStringLiteral(
            "Image telemetry is limited to recordings up to 24 hours"));
        emit scanStateChanged();
        return;
    }
    const auto durationNs = std::int64_t(std::llround(length * 1e9));
    const auto current =
        slotAt(seconds, ImageTelemetrySeries::slotCount(durationNs));
    if (series_ && !scanAhead_ && !pendingSave_ && !pendingWatch_ &&
        !reanchor_ && (!enabled_ || series_->cells[current].visited))
        return;
    const auto seenRevision =
        series_ ? series_->revision : std::numeric_limits<std::uint64_t>::max();
    const auto source = player_->source().toLocalFile();
    const auto model =
        modelPath_.isEmpty() ? omatrack::gaugeReaderModelPath() : modelPath_;
    const bool reanchor = reanchor_;
    reanchor_ = false;
    const auto state = worker_;
    const bool ahead = scanAhead_, enabled = enabled_;
    const auto setupHash = evidence_.setup.readingFingerprint();
    const auto selectedFields = evidence_.setup.readableFields();
    const auto readConfiguration = evidence_.setup.readerConfiguration();
    job_.start(
        [state, source, model, durationNs, current, reanchor, ahead, enabled,
         seenRevision, setupHash, selectedFields,
         readConfiguration](omatrack::IoCancel cancel) {
            auto result = std::make_shared<ImageTelemetryResult>();
            QElapsedTimer batch;
            batch.start();
            bool publish = false;
            try {
                if (!state->initialized) {
                    auto expected = state->cache.prepare(
                        source, model, durationNs, 0, false, cancel, setupHash);
                    if (!expected.ok()) {
                        result->message = expected.error;
                        result->fatal = true;
                        return result;
                    }
                    auto hit = state->cache.load(*expected.series, cancel);
                    state->draft = std::make_shared<ImageTelemetrySeries>(
                        hit.ok() ? *hit.series : *expected.series);
                    state->cachePath =
                        hit.ok() ? hit.path
                                 : state->cache.pathFor(*state->draft);
                    state->savedComplete =
                        hit.status == Cache::Status::Complete;
                    state->watchStart = state->lastCursor = current;
                    state->scheduler.fromCursor(current);
                    state->initialized = true;
                    publish = true;
                }
                auto& draft = *state->draft;
                if (reanchor) {
                    state->watchStart = current;
                    state->scheduler.fromCursor(current);
                }
                if (current < state->lastCursor) state->watchStart = current;
                state->lastCursor = current;
                int processed = 0;
                while (enabled && !state->savedComplete &&
                       processed < BatchLimit &&
                       batch.elapsed() < BatchBudgetMs && !cancel->load()) {
                    std::optional<std::size_t> selected;
                    if (ahead) {
                        selected = state->scheduler.next(
                            draft.cells.size(), current, true,
                            [&draft](std::size_t i) {
                                return draft.cells[i].visited;
                            });
                    } else {
                        if (!draft.cells[current].visited)
                            selected = current;
                        else {
                            while (state->watchStart <= current &&
                                   draft.cells[state->watchStart].visited)
                                ++state->watchStart;
                            if (state->watchStart <= current)
                                selected = state->watchStart;
                        }
                    }
                    if (!selected) break;
                    if (!VideoFrameDecoder::available() ||
                        !GaugeReader::runtimeAvailable()) {
                        result->message = QStringLiteral(
                            "Cached data available; image runtime required to "
                            "fill remaining coverage");
                        result->fatal = true;
                        break;
                    }
                    if (!state->opened) {
                        if (!state->decoder.open(source.toStdString(),
                                                 cancel)) {
                            result->message =
                                QString::fromStdString(state->decoder.error());
                            result->fatal = true;
                            break;
                        }
                        state->opened = true;
                        if (state->decoder.hasMetadataTrack()) {
                            result->message = QStringLiteral(
                                "Metadata track present; image fallback "
                                "withheld");
                            result->fatal = true;
                            break;
                        }
                        const auto origin = state->decoder.timelineOriginNs();
                        if (draft.visitedCount() &&
                            draft.timelineOriginNs != origin) {
                            result->message = QStringLiteral(
                                "Cached video clock does not match the "
                                "decoder");
                            result->fatal = true;
                            break;
                        }
                        draft.timelineOriginNs = origin;
                        draft.identity.nativeTelemetryAbsent = true;
                    }
                    const auto index = *selected;
                    const auto target =
                        std::int64_t(index) * ImageTelemetryPeriodNs;
                    const auto end =
                        std::min(durationNs, target + ImageTelemetryPeriodNs);
                    DecodedRgbFrame frame;
                    ImageTelemetrySlot point;
                    QElapsedTimer work;
                    work.start();
                    const bool decoded =
                        state->decoder.frameAtOrAfter(target, frame, cancel);
                    if (cancel->load()) break;
                    if (!decoded && !state->decoder.atEnd()) {
                        result->message =
                            QString::fromStdString(state->decoder.error());
                        result->fatal = ++state->errors >= 3;
                        break;  // No permanent coverage claim for a decode
                                // failure.
                    }
                    point.visited = true;
                    if (decoded && frame.presentationPtsNs >= target &&
                        frame.presentationPtsNs < end) {
                        point.presentationPtsNs = frame.presentationPtsNs;
                        point.sourcePtsNs = frame.sourcePtsNs;
                        const GaugeRgb24Frame pixels{
                            frame.pixels.data(), frame.pixels.size(),
                            frame.width, frame.height, frame.stride};
                        point.layoutSupported =
                            GaugeReader::inspectLayout(pixels).admission ==
                            GaugeAdmission::Supported;
                        if (point.layoutSupported) {
                            if (!state->reader)
                                state->reader = std::make_unique<GaugeReader>(
                                    model.toStdString());
                            if (!state->reader->ready()) {
                                result->message = QString::fromStdString(
                                    state->reader->modelError());
                                result->fatal = true;
                                break;
                            }
                            const auto reading = state->reader->readConfigured(
                                pixels, readConfiguration);
                            ++state->runs;
                            if (reading.error != GaugeError::None) {
                                result->message =
                                    QString::fromStdString(reading.detail);
                                result->fatal = true;
                                break;
                            }
                            point.values = {reading.gear.value,
                                            reading.stintLap.value,
                                            reading.brakeFillPct.value,
                                            reading.throttleFillPct.value};
                            for (std::size_t field = 0;
                                 field < selectedFields.size(); ++field)
                                if (!selectedFields[field])
                                    point.values[field].reset();
                            result->inferenceMs = reading.latencyMs;
                        }
                    }
                    if (cancel->load()) break;
                    result->totalMs = double(work.nsecsElapsed()) / 1e6;
                    draft.cells[index] = std::move(point);
                    ++draft.revision;
                    state->dirty = true;
                    state->errors = 0;
                    publish = true;
                    ++processed;
                }
                result->cancelled = cancel->load();
                result->complete = draft.complete();
                if (state->dirty && !result->cancelled &&
                    ((result->complete && processed > 0) ||
                     state->sinceSave.elapsed() >= CacheIntervalMs)) {
                    auto saved = state->cache.save(draft, cancel);
                    state->sinceSave.restart();
                    if (saved.ok()) {
                        if (saved.series) {
                            draft = *saved.series;
                            publish = true;
                            result->complete = draft.complete();
                        }
                        state->dirty = false;
                        state->lastCacheError.clear();
                        state->savedComplete =
                            saved.status == Cache::Status::Complete;
                        state->cachePath = saved.path;
                    } else
                        state->lastCacheError = saved.error;
                }
                // Compare against the GUI's acknowledged revision, not the last
                // worker result: cancellation may discard an already-built
                // result.
                if (publish || draft.revision != seenRevision)
                    result->series =
                        std::make_shared<const ImageTelemetrySeries>(draft);
                while (state->watchStart <= current &&
                       draft.cells[state->watchStart].visited)
                    ++state->watchStart;
                result->watchPending = enabled && state->watchStart <= current;
                result->visited = int(draft.visitedCount());
                result->known = int(std::count_if(draft.cells.begin(),
                                                  draft.cells.end(), anyKnown));
                result->runs = state->runs;
                result->cacheComplete = state->savedComplete;
                result->cachePath = state->cachePath;
                result->dirty = state->dirty;
                result->cacheError = state->lastCacheError;
            } catch (const std::exception&) {
                result->message = QStringLiteral(
                    "Image telemetry collection failed; video remains "
                    "available");
                result->fatal = true;
            }
            return result;
        },
        [this](const std::shared_ptr<ImageTelemetryResult>& result) {
            apply(result);
        },
        &workerPool_);
}

void ImageTelemetryController::apply(
    const std::shared_ptr<ImageTelemetryResult>& result) {
    if (result->cancelled || !eligible_ || !player_ || player_->seeking() ||
        awaitingSeek_)
        return;
    const bool metadataChanged = scanned_ != result->visited ||
                                 known_ != result->known ||
                                 inferenceRuns_ != result->runs ||
                                 cacheComplete_ != result->cacheComplete ||
                                 cachePath_ != result->cachePath;
    if (result->series) series_ = result->series;
    scanned_ = result->visited;
    known_ = result->known;
    inferenceRuns_ = result->runs;
    complete_ = result->complete;
    cacheComplete_ = result->cacheComplete;
    pendingSave_ = result->dirty;
    pendingWatch_ = result->watchPending;
    cachePath_ = result->cachePath;
    if (result->inferenceMs > 0) inferenceMs_ = result->inferenceMs;
    if (result->totalMs > 0) totalMs_ = result->totalMs;
    if (result->series || metadataChanged) emit timelineChanged();
    refreshCurrent();
    if (!result->message.isEmpty()) {
        blocked_ = result->fatal;
        setStatus(result->message);
        nextAttemptMs_ = clock_.elapsed() + 1500;
    } else if (!result->cacheError.isEmpty()) {
        setStatus(QStringLiteral("Telemetry collected; cache write failed: %1")
                      .arg(result->cacheError));
    } else if (cacheComplete_) {
        setStatus(known_
                      ? QStringLiteral("Cached image telemetry · complete")
                      : QStringLiteral(
                            "Scan complete · no supported readings (cached)"));
    } else if (!enabled_) {
        setStatus(QStringLiteral("Extraction off · cached coverage %1%")
                      .arg(progress() * 100, 0, 'f', 1));
    } else {
        setStatus(QStringLiteral("%1 · %2% scanned")
                      .arg(scanAhead_
                               ? QStringLiteral(
                                     "Scanning from cursor and backfilling")
                               : QStringLiteral("Reading as you watch"))
                      .arg(progress() * 100, 0, 'f', 1));
    }
    if (complete_ || blocked_) {
        if (scanAhead_) {
            scanAhead_ = false;
            emit scanStateChanged();
        }
    } else if (scanAhead_ && result->message.isEmpty()) {
        // Finite batches yield to the UI, but ahead scanning is not paced by
        // playback or a sleep loop. There is still only one job in flight.
        QTimer::singleShot(0, this, [this]() { sample(); });
    }
}
