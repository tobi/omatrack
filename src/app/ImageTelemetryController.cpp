#include "ImageTelemetryController.h"

#include "GaugeReader.h"
#include "ImageTelemetryCache.h"
#include "VideoFrameDecoder.h"
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

ImageTelemetryController::ImageTelemetryController(QObject* parent)
    : QObject(parent), job_(this), disposeJob_(this) {
    workerPool_.setMaxThreadCount(1);
    workerPool_.setExpiryTimeout(-1);
    clock_.start();
    std::fill(std::begin(values_), std::end(values_),
              std::numeric_limits<double>::quiet_NaN());
    timer_.setInterval(200);
    connect(&timer_, &QTimer::timeout, this, &ImageTelemetryController::sample);
    connect(&job_, &AsyncJobBase::runningChanged, this,
            &ImageTelemetryController::scanStateChanged);
    timer_.start();
    reset();
}
ImageTelemetryController::~ImageTelemetryController() {
    timer_.stop();
    job_.reset();
    retireWorker();
    job_.wait();
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
    job_.reset();
    reanchor_ = true;
    if (!enabled_) {
        scanAhead_ = false;
        invalidate();
    }
    blocked_ = false;
    nextAttemptMs_ = 0;
    emit enabledChanged();
    emit scanStateChanged();
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
void ImageTelemetryController::setScanAhead(bool enabled) {
    if (scanAhead_ == enabled ||
        (enabled && (!enabled_ || blocked_ || complete_)))
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
void ImageTelemetryController::reset() {
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
    setStatus(eligible_
                  ? QStringLiteral("Checking image telemetry cache…")
                  : QStringLiteral("Native telemetry / no standalone video"));
}
void ImageTelemetryController::resetForSeek() {
    // A seek invalidates current readings, NOT already collected source data.
    job_.reset();
    reanchor_ = true;
    nextAttemptMs_ = 0;
    invalidate();
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
    if (!eligible_ || !player_ || !player_->loaded() || player_->seeking() ||
        awaitingSeek_)
        return;
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
        modelPath_.isEmpty()
            ? QDir(QCoreApplication::applicationDirPath())
                  .filePath(QStringLiteral("models/gauge-reader.onnx"))
            : modelPath_;
    const bool reanchor = reanchor_;
    reanchor_ = false;
    const auto state = worker_;
    const bool ahead = scanAhead_, enabled = enabled_;
    job_.start(
        [state, source, model, durationNs, current, reanchor, ahead, enabled,
         seenRevision](omatrack::IoCancel cancel) {
            auto result = std::make_shared<ImageTelemetryResult>();
            QElapsedTimer batch;
            batch.start();
            bool publish = false;
            try {
                if (!state->initialized) {
                    auto expected = state->cache.prepare(
                        source, model, durationNs, 0, false, cancel);
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
                            const auto reading = state->reader->read(pixels);
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
