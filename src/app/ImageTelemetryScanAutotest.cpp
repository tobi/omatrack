// origin: PUBLIC — real progressive scan/cache/scene-graph acceptance.
// Inputs arrive only through the test environment. No fabricated observations.
#include "ImageTelemetryScanAutotest.h"
#include "AsyncJob.h"
#include "ImageTelemetryCache.h"
#include "ImageTelemetryController.h"
#include "ImageTelemetryTraces.h"
#include "MpvVideoItem.h"
#include "TelemetryStore.h"
#include "core/TelemetryEngine.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QImage>
#include <QKeySequence>
#include <QMouseEvent>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QTimer>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

namespace {
using omatrack::inference::ImageTelemetryChannelNames;
using omatrack::inference::ImageTelemetryPeriodNs;
using omatrack::inference::ImageTelemetrySeries;
using omatrack::inference::ImageTelemetrySnapshot;
constexpr int SweepFrames = 120;

enum class Phase {
    Startup,
    Watching,
    SeekWatch,
    AheadPartial,
    PartialIdle,
    BlankForPartial,
    ReopenPartial,
    ResumeSeek,
    AheadFull,
    VerifyFull,
    BlankForComplete,
    ReopenComplete,
    CacheHitHold,
    RenderPrepare,
    RenderSweep,
    RenderViewport,
    RenderClick,
    NativeHold,
    VetoHold,
    BlankScan,
    VerifyNegative,
    Finish
};
struct IoResult {
    bool ok = false;
    QString why;
    int channels = 0;
    double ms = 0;
};

bool retained(const ImageTelemetrySeries& before,
              const ImageTelemetrySeries& after) {
    if (before.durationNs != after.durationNs ||
        before.cells.size() != after.cells.size())
        return false;
    for (std::size_t i = 0; i < before.cells.size(); ++i) {
        const auto& a = before.cells[i];
        const auto& b = after.cells[i];
        if (!a.visited) continue;
        if (!b.visited || (a.layoutSupported && !b.layoutSupported) ||
            (a.presentationPtsNs &&
             a.presentationPtsNs != b.presentationPtsNs) ||
            (a.sourcePtsNs && a.sourcePtsNs != b.sourcePtsNs))
            return false;
        for (std::size_t field = 0; field < 4; ++field)
            if (a.values[field] && a.values[field] != b.values[field])
                return false;
    }
    return true;
}
double coverageSeconds(const ImageTelemetrySeries& series) {
    std::int64_t covered = 0;
    for (std::size_t i = 0; i < series.cells.size(); ++i)
        if (series.cells[i].visited)
            covered += std::min(
                series.durationNs - std::int64_t(i) * ImageTelemetryPeriodNs,
                ImageTelemetryPeriodNs);
    return double(covered) / 1e9;
}
IoResult roundTrip(const QString& file, ImageTelemetrySnapshot expected,
                   omatrack::IoCancel cancel) {
    QElapsedTimer time;
    time.start();
    IoResult result;
    const auto bad = [&](const char* why) {
        result.why = QString::fromLatin1(why);
        return result;
    };
    if (!expected || !file.endsWith(QStringLiteral(".telemetry")) ||
        cancel->load())
        return bad("missing native cache snapshot");
    // Independent application parser, not merely reading the worker's draft.
    auto source = omatrack::TelemetrySource::open(file.toStdString());
    if (!source || source->isExtension() || !source->sourceLaps().empty())
        return bad(
            "standard recording reader failed or fabricated source laps");
    std::array<std::string, 11> names;
    for (std::size_t i = 0; i < 4; ++i) {
        names[i] = ImageTelemetryChannelNames[i];
        names[i + 4] = names[i] + "_known";
    }
    names[8] = "image_derived_visited";
    names[9] = "image_derived_presentation_pts_ns";
    names[10] = "image_layout_supported";
    if (source->channels().size() != names.size())
        return bad("wrong standard channel count");
    for (std::size_t channel = 0; channel < names.size(); ++channel) {
        if (cancel->load()) return bad("cancelled");
        const auto& channels = source->channels();
        const auto found = std::find_if(
            channels.begin(), channels.end(),
            [&](const auto& c) { return c.name == names[channel]; });
        if (found == channels.end() ||
            found->samples.size() != expected->cells.size() ||
            found->startNs != 0 || std::abs(found->frequencyHz - 5) > 1e-9)
            return bad("standard channel clock or sample count disagrees");
        for (std::size_t i = 0; i < expected->cells.size(); ++i) {
            const auto& point = expected->cells[i];
            std::optional<double> value;
            if (channel < 4)
                value = point.values[channel];
            else if (channel < 8)
                value = point.values[channel - 4].has_value() ? 1.0 : 0.0;
            else if (channel == 8)
                value = point.visited ? 1.0 : 0.0;
            else if (channel == 9) {
                if (point.presentationPtsNs)
                    value = double(*point.presentationPtsNs);
            } else
                value = point.layoutSupported ? 1.0 : 0.0;
            if (value ? found->samples[i] != *value
                      : !std::isnan(found->samples[i]))
                return bad(
                    "standard reader changed values, masks or actual PTS");
            double sampled = std::numeric_limits<double>::quiet_NaN();
            const bool present = source->sampleAtNs(
                std::size_t(found - channels.begin()),
                std::uint64_t(i) * ImageTelemetryPeriodNs, &sampled, false);
            if (value ? (!present || sampled != *value)
                      : (present && !std::isnan(sampled)))
                return bad(
                    "standard reader source-clock lookup disagrees with cache "
                    "lattice");
        }
    }
    omatrack::ImageTelemetryCache cache;
    const auto restored = cache.load(*expected, cancel);
    if (!restored.ok() || !restored.series ||
        !retained(*expected, *restored.series) ||
        !retained(*restored.series, *expected))
        return bad("cache reload changed immutable evidence");
    result.ok = true;
    result.channels = int(names.size());
    result.ms = time.nsecsElapsed() / 1e6;
    return result;
}

class ScanCheck final : public QObject {
public:
    ScanCheck(QQmlApplicationEngine& engine, TelemetryStore& store,
              QString mode)
        : QObject(&engine),
          engine_(engine),
          store_(store),
          mode_(std::move(mode)),
          io_(this) {
        source_ = QUrl::fromLocalFile(qEnvironmentVariable("OMATRACK_VIDEO"));
        blank_ = QUrl::fromLocalFile(
            qEnvironmentVariable("OMATRACK_AUTOTEST_IMAGE_BLANK"));
        shot_ = qEnvironmentVariable("OMATRACK_AUTOTEST");
        cursorCosts_.reserve(SweepFrames);
        total_.start();
        phaseTime_.start();
        timer_.setInterval(50);
        connect(&timer_, &QTimer::timeout, this, [this]() { tick(); });
        timer_.start();
    }

private:
    void enter(Phase phase) {
        phase_ = phase;
        phaseTime_.restart();
    }
    bool require(bool condition, const char* why) {
        if (!condition) fail(QString::fromLatin1(why));
        return condition;
    }
    void fail(const QString& why) {
        if (finished_) return;
        finished_ = true;
        timer_.stop();
        io_.reset();
        qWarning() << "AUTOTEST image scan FAIL" << mode_ << "phase"
                   << int(phase_) << why;
        if (reader_)
            qWarning() << "AUTOTEST image scan state: visited"
                       << reader_->scannedSamples() << "known"
                       << reader_->knownSamples() << "runs"
                       << reader_->inferenceRuns() << "complete"
                       << reader_->complete() << "persisted"
                       << reader_->cacheComplete() << "status"
                       << reader_->status() << "cache path"
                       << reader_->cachePath();
        if (window_ && !shot_.isEmpty())
            window_->grabWindow().save(shot_ + QStringLiteral(".failed.png"));
        QCoreApplication::exit(1);
    }
    bool unknown() const {
        return !reader_->valid() && std::isnan(reader_->gear()) &&
               std::isnan(reader_->stintLap()) &&
               std::isnan(reader_->brakeFillPct()) &&
               std::isnan(reader_->throttleFillPct());
    }
    bool loaded(const QUrl& url) const {
        return player_->loaded() && player_->source() == url &&
               !store_.loading() && !store_.lapLoading();
    }
    bool shortcut(const char* key) {
        QObject* found = nullptr;
        for (QObject* object : window_->findChildren<QObject*>()) {
            if (object->metaObject()->indexOfSignal("activated()") < 0 ||
                !object->property("enabled").toBool() ||
                object->property("sequence").value<QKeySequence>() !=
                    QKeySequence(QLatin1String(key)))
                continue;
            if (!require(!found, "ambiguous enabled shortcut")) return false;
            found = object;
        }
        return require(found && QMetaObject::invokeMethod(found, "activated"),
                       "real shortcut handler unavailable");
    }
    bool dock() {
        return !window_->property("videoFullscreen").toBool() ||
               shortcut("Escape");
    }
    void timeline() {
        if (finished_) return;
        const auto snapshot = reader_->series();
        if (mode_ != QStringLiteral("supported") ||
            player_->source() == blank_) {
            if (!require(reader_->knownSamples() == 0 &&
                             reader_->inferenceRuns() == 0 && unknown(),
                         "negative source published inferred values or ran the "
                         "model"))
                return;
        }
        if (mode_ != QStringLiteral("supported") ||
            player_->source() != source_ || !snapshot)
            return;
        if (lastOriginal_ &&
            snapshot->identity.source.canonicalPath ==
                lastOriginal_->identity.source.canonicalPath &&
            !retained(*lastOriginal_, *snapshot)) {
            int missingCells = 0, lostValues = 0, changedValues = 0,
                changedPts = 0;
            for (std::size_t i = 0; i < std::min(lastOriginal_->cells.size(),
                                                 snapshot->cells.size());
                 ++i) {
                const auto& a = lastOriginal_->cells[i];
                const auto& b = snapshot->cells[i];
                if (a.visited && !b.visited) ++missingCells;
                if (a.presentationPtsNs && b.presentationPtsNs &&
                    a.presentationPtsNs != b.presentationPtsNs)
                    ++changedPts;
                for (std::size_t field = 0; field < 4; ++field) {
                    if (a.values[field] && !b.values[field])
                        ++lostValues;
                    else if (a.values[field] &&
                             a.values[field] != b.values[field])
                        ++changedValues;
                }
            }
            qWarning() << "AUTOTEST image scan retention diagnostics: "
                          "before/after visited"
                       << lastOriginal_->visitedCount()
                       << snapshot->visitedCount() << "missing cells"
                       << missingCells << "lost known fields" << lostValues
                       << "changed known fields" << changedValues
                       << "changed actual PTS" << changedPts;
            fail(QStringLiteral(
                "progressive coverage or known evidence regressed"));
            return;
        }
        lastOriginal_ = snapshot;
        if (priorityGuard_ && snapshot->cells[priorityCell_].visited) {
            priorityGuard_ = false;
            qWarning() << "AUTOTEST image scan: cursor-priority cell collected";
        } else if (priorityGuard_ &&
                   reader_->scannedSamples() > priorityBefore_) {
            fail(QStringLiteral(
                "other new coverage published before requested cursor cell"));
        }
    }
    bool seek(double seconds, bool retainHistory) {
        const auto before = reader_->series();
        player_->seek(seconds);
        if (!require(unknown(),
                     "seek did not immediately invalidate current reading"))
            return false;
        if (retainHistory && !require(before && reader_->series() &&
                                          retained(*before, *reader_->series()),
                                      "seek erased collected history"))
            return false;
        return true;
    }
    void startAhead() {
        aheadStarted_ = total_.elapsed();
        aheadBefore_ = coverageSeconds(*reader_->series());
        reader_->setScanAhead(true);
        require(reader_->scanAhead(), "scan-ahead request refused");
    }
    void finishAheadSegment() {
        const double seconds = (total_.elapsed() - aheadStarted_) / 1000.0;
        const double media = coverageSeconds(*reader_->series()) - aheadBefore_;
        aheadWall_ += seconds;
        aheadMedia_ += media;
        qWarning() << "AUTOTEST image scan segment: coverage seconds" << media
                   << "wall seconds" << seconds
                   << "coverage-seconds per wall-second"
                   << (seconds > 0 ? media / seconds : 0);
    }
    void open(const QUrl& url) { store_.openFile(url.toLocalFile()); }
    void verifyCache(Phase waiting) {
        enter(waiting);
        const auto snapshot = reader_->series();
        const QString path = reader_->cachePath();
        io_.start(
            [snapshot, path](omatrack::IoCancel cancel) {
                return roundTrip(path, snapshot, cancel);
            },
            [this, waiting](IoResult result) {
                if (finished_ || phase_ != waiting) return;
                if (!result.ok) {
                    fail(result.why);
                    return;
                }
                qWarning()
                    << "AUTOTEST image scan: standard .telemetry roundtrip"
                    << result.channels
                    << "channels; exact values/masks/PTS; no source laps; "
                       "worker ms"
                    << result.ms;
                if (waiting == Phase::VerifyFull) {
                    open(blank_);
                    enter(Phase::BlankForComplete);
                } else
                    enter(Phase::Finish);
            });
    }
    void verifyNoCache() {
        enter(Phase::VerifyNegative);
        const QString root = qEnvironmentVariable("XDG_CACHE_HOME");
        io_.start(
            [root](omatrack::IoCancel cancel) {
                IoResult result;
                QDirIterator files(root, {QStringLiteral("*.telemetry")},
                                   QDir::Files, QDirIterator::Subdirectories);
                result.ok = !cancel->load() && !files.hasNext();
                result.why = QStringLiteral(
                    "native/metadata-veto source wrote an inferred cache");
                return result;
            },
            [this](IoResult result) {
                if (!result.ok) {
                    fail(result.why);
                    return;
                }
                qWarning() << "AUTOTEST image scan: native/metadata veto left "
                              "no inferred .telemetry files";
                enter(Phase::Finish);
            });
    }
    void frameEnded() {
        ++frames_;
        if (finished_ || phase_ != Phase::RenderSweep ||
            traces_->cursorBuildCount() <= lastCursorBuild_)
            return;
        if (!require(traces_->staticBuildCount() == frozenStatic_,
                     "cursor movement rebuilt static geometry"))
            return;
        lastCursorBuild_ = traces_->cursorBuildCount();
        cursorCosts_.push_back(traces_->lastCursorBuildMs());
        if (cursorCosts_.size() == SweepFrames) {
            std::sort(cursorCosts_.begin(), cursorCosts_.end());
            qWarning() << "AUTOTEST image traces: real frames"
                       << cursorCosts_.size()
                       << "cursor geometry ms p50/p95/max" << cursorCosts_[59]
                       << cursorCosts_[113] << cursorCosts_.back()
                       << "static builds unchanged" << frozenStatic_
                       << "sweep wall ms" << phaseTime_.elapsed();
            traces_->setView(0, traces_->duration() / 2);
            enter(Phase::RenderViewport);
        } else {
            traces_->setPosition(traces_->duration() *
                                 double(cursorCosts_.size() + 1) /
                                 (SweepFrames + 1));
        }
    }
    void tick() {
        if (finished_) return;
        if (total_.elapsed() > 180000 || phaseTime_.elapsed() > 60000) {
            fail(QStringLiteral("finite scan acceptance deadline"));
            return;
        }
        if (!window_) {
            if (!require(
                    !shot_.isEmpty() && source_.isLocalFile() &&
                        !source_.toLocalFile().isEmpty() &&
                        !qEnvironmentVariable("XDG_CONFIG_HOME").isEmpty() &&
                        !qEnvironmentVariable("XDG_CACHE_HOME").isEmpty(),
                    "screenshot, source and isolated XDG config/cache are "
                    "required"))
                return;
            if (!require(mode_ == QStringLiteral("supported") ||
                             mode_ == QStringLiteral("native") ||
                             mode_ == QStringLiteral("blank") ||
                             mode_ == QStringLiteral("metadata-veto"),
                         "unsupported scan mode"))
                return;
            if (engine_.rootObjects().isEmpty()) return;
            window_ =
                qobject_cast<QQuickWindow*>(engine_.rootObjects().first());
            if (!require(window_, "no Quick window")) return;
            player_ = window_->findChild<MpvVideoItem*>(
                QStringLiteral("videoPlayer"));
            reader_ = window_->findChild<ImageTelemetryController*>(
                QStringLiteral("imageTelemetryController"));
            traces_ = window_->findChild<ImageTelemetryTraces*>(
                QStringLiteral("imageTelemetryTraces"));
            if (!require(player_ && reader_ && traces_,
                         "progressive player/controller/traces missing"))
                return;
            connect(reader_, &ImageTelemetryController::timelineChanged, this,
                    [this]() { timeline(); });
            connect(reader_, &ImageTelemetryController::sampleChanged, this,
                    [this]() {
                        if (player_->source() == blank_ && !unknown())
                            fail(QStringLiteral(
                                "old readings reached blank source"));
                    });
            connect(
                window_, &QQuickWindow::afterFrameEnd, this,
                [this]() { frameEnded(); }, Qt::QueuedConnection);
            connect(traces_, &ImageTelemetryTraces::seekRequested, this,
                    [this](double seconds) {
                        clickTarget_ = seconds;
                        ++clickCount_;
                    });
            timeline();
        }
        if (finished_) return;
        switch (phase_) {
            case Phase::Startup:
                if (!store_.ready() || !loaded(source_) || !player_->ready())
                    return;
                if (!require(reader_->available() && reader_->enabled() &&
                                 window_->rendererInterface()->graphicsApi() ==
                                     QSGRendererInterface::OpenGL,
                             "native GL/real image runtime/enabled setting "
                             "required"))
                    return;
                if (mode_ == QStringLiteral("native")) {
                    player_->setPaused(false);
                    enter(Phase::NativeHold);
                    return;
                }
                if (!require(
                        !store_.primaryUnified() &&
                            window_->property("standaloneVideoActive").toBool(),
                        "source did not take standalone path"))
                    return;
                if (mode_ == QStringLiteral("metadata-veto")) {
                    player_->setPaused(false);
                    enter(Phase::VetoHold);
                    return;
                }
                if (mode_ == QStringLiteral("blank")) {
                    reader_->setScanAhead(true);
                    enter(Phase::BlankScan);
                    return;
                }
                if (!require(!blank_.toLocalFile().isEmpty() &&
                                 blank_ != source_ &&
                                 player_->duration() >= 30 &&
                                 player_->duration() <= 180,
                             "supported scan needs distinct blank and a "
                             "30–180s source"))
                    return;
                if (!dock()) return;
                reader_->setScanAhead(false);
                player_->setPaused(false);
                enter(Phase::Watching);
                return;
            case Phase::Watching:
                if (reader_->scannedSamples() < 5 ||
                    reader_->knownSamples() < 3 ||
                    reader_->inferenceRuns() == 0 || player_->position() < 0.8)
                    return;
                if (!require(traces_->isVisible() &&
                                 !window_->property("videoFullscreen").toBool(),
                             "time traces are not in docked workspace"))
                    return;
                watched_ = reader_->series();
                if (!require(watched_ && reader_->progress() < 0.25,
                             "cold watch unexpectedly prefilled recording"))
                    return;
                player_->setPaused(true);
                priorityCell_ = watched_->cells.size() / 2;
                priorityBefore_ = reader_->scannedSamples();
                priorityGuard_ = true;
                if (!seek(double(priorityCell_ * ImageTelemetryPeriodNs) / 1e9 +
                              0.03,
                          true))
                    return;
                qWarning() << "AUTOTEST image scan: watched coverage retained "
                              "across mid-recording seek";
                enter(Phase::SeekWatch);
                return;
            case Phase::SeekWatch:
                if (player_->seeking() || priorityGuard_) return;
                if (!require(retained(*watched_, *reader_->series()),
                             "watch history lost after seek"))
                    return;
                startAhead();
                enter(Phase::AheadPartial);
                return;
            case Phase::AheadPartial:
                if (reader_->progress() < 1.0 / 3.0) return;
                reader_->setScanAhead(false);
                enter(Phase::PartialIdle);
                return;
            case Phase::PartialIdle:
                if (phaseTime_.elapsed() < 200 || reader_->running()) return;
                if (!require(
                        reader_->series() && !reader_->complete() &&
                            reader_->progress() < 0.75,
                        "fixture finished before partial-cache interruption"))
                    return;
                partial_ = reader_->series();
                partialCount_ = reader_->scannedSamples();
                qWarning()
                    << "AUTOTEST image scan before partial switch: visited"
                    << partialCount_ << "cacheComplete"
                    << reader_->cacheComplete() << "status" << reader_->status()
                    << "cache path" << reader_->cachePath();
                finishAheadSegment();
                if (!require(window_->grabWindow().save(
                                 shot_ + QStringLiteral(".partial.png")),
                             "could not capture actual partial time traces"))
                    return;
                open(blank_);
                enter(Phase::BlankForPartial);
                return;
            case Phase::BlankForPartial:
                if (!loaded(blank_) || phaseTime_.elapsed() < 200) return;
                player_->setPaused(true);
                reopenAt_ = total_.elapsed();
                open(source_);
                enter(Phase::ReopenPartial);
                return;
            case Phase::ReopenPartial:
                if (!loaded(source_)) return;
                player_->setPaused(true);
                if (!reader_->series() ||
                    reader_->scannedSamples() < partialCount_)
                    return;
                if (!require(reader_->inferenceRuns() == 0 &&
                                 retained(*partial_, *reader_->series()) &&
                                 !reader_->complete(),
                             "partial reopen did not reuse saved monotonic "
                             "coverage"))
                    return;
                qWarning() << "AUTOTEST image scan: partial cache reused with "
                              "zero model calls; reopen ms"
                           << total_.elapsed() - reopenAt_;
                priorityCell_ = reader_->series()->cells.size() - 2;
                while (priorityCell_ >
                           reader_->series()->cells.size() * 3 / 4 &&
                       reader_->series()->cells[priorityCell_].visited)
                    --priorityCell_;
                if (!require(!reader_->series()->cells[priorityCell_].visited,
                             "no late gap left to prove wrap/backfill"))
                    return;
                priorityBefore_ = reader_->scannedSamples();
                priorityGuard_ = true;
                if (!seek(double(priorityCell_ * ImageTelemetryPeriodNs) / 1e9 +
                              0.03,
                          true))
                    return;
                enter(Phase::ResumeSeek);
                return;
            case Phase::ResumeSeek:
                if (player_->seeking() || priorityGuard_) return;
                startAhead();
                enter(Phase::AheadFull);
                return;
            case Phase::AheadFull:
                if (!reader_->complete() || !reader_->cacheComplete()) return;
                full_ = reader_->series();
                if (!require(full_ && full_->complete() &&
                                 retained(*partial_, *full_) &&
                                 reader_->knownSamples() > 0,
                             "full coverage/backfill did not preserve actual "
                             "observations"))
                    return;
                finishAheadSegment();
                qWarning() << "AUTOTEST image scan: end wrap/backfill complete "
                              "and persisted; cumulative ahead wall seconds"
                           << aheadWall_ << "coverage seconds" << aheadMedia_
                           << "coverage throughput" << aheadMedia_ / aheadWall_;
                verifyCache(Phase::VerifyFull);
                return;
            case Phase::VerifyFull:
            case Phase::VerifyNegative: return;
            case Phase::BlankForComplete:
                if (!loaded(blank_) || phaseTime_.elapsed() < 200) return;
                player_->setPaused(true);
                reopenAt_ = total_.elapsed();
                open(source_);
                enter(Phase::ReopenComplete);
                return;
            case Phase::ReopenComplete:
                if (!loaded(source_) || !reader_->cacheComplete()) return;
                if (!require(
                        reader_->complete() && reader_->inferenceRuns() == 0 &&
                            reader_->series() &&
                            retained(*full_, *reader_->series()),
                        "complete reopen re-extracted or lost cached evidence"))
                    return;
                qWarning()
                    << "AUTOTEST image scan: complete cache hit reopen ms"
                    << total_.elapsed() - reopenAt_ << "model calls 0";
                playStart_ = player_->position();
                player_->setPaused(false);
                enter(Phase::CacheHitHold);
                return;
            case Phase::CacheHitHold:
                if (!require(reader_->inferenceRuns() == 0 &&
                                 reader_->cacheComplete(),
                             "cached playback re-extracted"))
                    return;
                if (phaseTime_.elapsed() < 1200 ||
                    player_->position() < playStart_ + 0.8)
                    return;
                player_->setPaused(true);
                if (!dock()) return;
                frozen_ = reader_->series();
                traces_->setController(nullptr);
                traces_->setSeries(frozen_);
                traces_->resetView();
                traces_->setPosition(0);
                prepareFrame_ = frames_;
                enter(Phase::RenderPrepare);
                return;
            case Phase::RenderPrepare:
                if (frames_ <= prepareFrame_ || phaseTime_.elapsed() < 200 ||
                    traces_->staticBuildCount() == 0)
                    return;
                if (!require(traces_->isVisible(),
                             "docked trace view disappeared"))
                    return;
                frozenStatic_ = traces_->staticBuildCount();
                lastCursorBuild_ = traces_->cursorBuildCount();
                qWarning() << "AUTOTEST image traces: static geometry ms"
                           << traces_->lastStaticBuildMs();
                enter(Phase::RenderSweep);
                traces_->setPosition(traces_->duration() / (SweepFrames + 1));
                return;
            case Phase::RenderSweep:
                return;  // Driven solely by completed real frames.
            case Phase::RenderViewport:
                if (traces_->staticBuildCount() <= frozenStatic_) return;
                if (!require(traces_->staticBuildCount() == frozenStatic_ + 1,
                             "viewport changed static geometry more than once"))
                    return;
                traces_->resetView();
                traces_->setController(reader_);
                traces_->setPosition(player_->position());
                {
                    const QPointF point(traces_->width() * 0.75,
                                        traces_->height() * 0.5);
                    QMouseEvent press(QEvent::MouseButtonPress, point, point,
                                      point, Qt::LeftButton, Qt::LeftButton,
                                      Qt::NoModifier);
                    QMouseEvent release(QEvent::MouseButtonRelease, point,
                                        point, point, Qt::LeftButton,
                                        Qt::NoButton, Qt::NoModifier);
                    QCoreApplication::sendEvent(traces_, &press);
                    QCoreApplication::sendEvent(traces_, &release);
                }
                if (!require(clickCount_ == 1 && clickTarget_ > 0 &&
                                 clickTarget_ < player_->duration() &&
                                 std::abs(player_->targetPosition() -
                                          clickTarget_) < 1e-5,
                             "trace click did not seek real video time"))
                    return;
                enter(Phase::RenderClick);
                return;
            case Phase::RenderClick:
                if (player_->seeking() ||
                    std::abs(player_->position() - clickTarget_) > 0.15 ||
                    phaseTime_.elapsed() < 300)
                    return;
                if (!require(reader_->inferenceRuns() == 0 &&
                                 reader_->cacheComplete(),
                             "trace click re-extracted complete cache"))
                    return;
                qWarning() << "AUTOTEST image traces: real click-to-seek "
                              "verified; cache retained";
                enter(Phase::Finish);
                return;
            case Phase::NativeHold:
                if (!require(store_.primaryUnified() && !reader_->eligible() &&
                                 reader_->knownSamples() == 0 &&
                                 reader_->inferenceRuns() == 0 && unknown(),
                             "native precedence violated"))
                    return;
                if (phaseTime_.elapsed() >= 2500) verifyNoCache();
                return;
            case Phase::VetoHold:
                if (!require(reader_->knownSamples() == 0 &&
                                 reader_->inferenceRuns() == 0 && unknown(),
                             "metadata veto produced readings"))
                    return;
                if (phaseTime_.elapsed() >= 2500 &&
                    reader_->status().contains(
                        QStringLiteral("Metadata track present")))
                    verifyNoCache();
                return;
            case Phase::BlankScan:
                if (!require(reader_->knownSamples() == 0 &&
                                 reader_->inferenceRuns() == 0 && unknown(),
                             "no-HUD source ran model or became known"))
                    return;
                if (!reader_->cacheComplete()) return;
                if (!require(reader_->complete() && reader_->series() &&
                                 reader_->series()->complete(),
                             "blank completion coverage missing"))
                    return;
                qWarning() << "AUTOTEST image scan: blank cached as visited "
                              "unknown, model calls 0";
                verifyCache(Phase::VerifyNegative);
                return;
            case Phase::Finish:
                if (phaseTime_.elapsed() < 200) return;
                {
                    const QImage image = window_->grabWindow();
                    if (!require(!image.isNull(), "empty native GL screenshot"))
                        return;
                    if (mode_ != QStringLiteral("blank")) {
                        const QRectF video = player_->mapRectToScene(
                            QRectF(0, 0, player_->width(), player_->height()));
                        const qreal scale =
                            image.width() / qreal(window_->width());
                        int lit = 0;
                        for (int y = 0; y < 20; ++y)
                            for (int x = 0; x < 30; ++x) {
                                const int px =
                                    std::clamp(int((video.left() +
                                                    video.width() *
                                                        (0.1 + 0.55 * x / 30)) *
                                                   scale),
                                               0, image.width() - 1);
                                const int py =
                                    std::clamp(int((video.top() +
                                                    video.height() *
                                                        (0.2 + 0.5 * y / 20)) *
                                                   scale),
                                               0, image.height() - 1);
                                if (qGray(image.pixel(px, py)) > 24) ++lit;
                            }
                        if (!require(lit > 40,
                                     "loaded video is black in native GL"))
                            return;
                    }
                    if (mode_ == QStringLiteral("supported")) {
                        const QRectF area = traces_->mapRectToScene(
                            QRectF(0, 0, traces_->width(), traces_->height()));
                        int chromatic = 0;
                        const qreal scale =
                            image.width() / qreal(window_->width());
                        const int left = std::clamp(
                            int((area.left() + area.width() * 0.2) * scale), 0,
                            image.width() - 1);
                        const int right =
                            std::clamp(int((area.right() - 8) * scale), left,
                                       image.width());
                        const int top =
                            std::clamp(int((area.top() + 22) * scale), 0,
                                       image.height() - 1);
                        const int bottom =
                            std::clamp(int((area.bottom() - 24) * scale), top,
                                       image.height());
                        for (int py = top; py < bottom; py += 2)
                            for (int px = left; px < right; px += 2) {
                                const QColor c = image.pixelColor(px, py);
                                if (std::max({c.red(), c.green(), c.blue()}) -
                                        std::min(
                                            {c.red(), c.green(), c.blue()}) >
                                    35)
                                    ++chromatic;
                            }
                        if (!require(
                                chromatic > 40,
                                "time traces have no visible colored geometry"))
                            return;
                    }
                    if (!require(image.save(shot_),
                                 "could not save scan acceptance image"))
                        return;
                }
                finished_ = true;
                timer_.stop();
                qWarning() << "AUTOTEST image scan PASS" << mode_
                           << "elapsed ms" << total_.elapsed();
                QCoreApplication::exit(0);
                return;
        }
    }
    QQmlApplicationEngine& engine_;
    TelemetryStore& store_;
    QString mode_, shot_;
    QUrl source_, blank_;
    QQuickWindow* window_ = nullptr;
    MpvVideoItem* player_ = nullptr;
    ImageTelemetryController* reader_ = nullptr;
    ImageTelemetryTraces* traces_ = nullptr;
    QTimer timer_;
    QElapsedTimer total_, phaseTime_;
    AsyncJob<IoResult> io_;
    Phase phase_ = Phase::Startup;
    bool finished_ = false, priorityGuard_ = false;
    ImageTelemetrySnapshot watched_, partial_, full_, frozen_, lastOriginal_;
    std::size_t priorityCell_ = 0;
    int priorityBefore_ = 0, partialCount_ = 0;
    qint64 reopenAt_ = 0, aheadStarted_ = 0;
    double playStart_ = 0, aheadBefore_ = 0, aheadWall_ = 0, aheadMedia_ = 0;
    quint64 frozenStatic_ = 0, lastCursorBuild_ = 0, frames_ = 0,
            prepareFrame_ = 0;
    std::vector<double> cursorCosts_;
    int clickCount_ = 0;
    double clickTarget_ = -1;
};
}  // namespace

bool omatrack::autotest::installImageTelemetryScan(
    QQmlApplicationEngine& engine, TelemetryStore& store) {
    QString mode = qEnvironmentVariable("OMATRACK_AUTOTEST_IMAGE_SCAN");
    if (mode.isEmpty()) return false;
    if (mode == QStringLiteral("1")) mode = QStringLiteral("supported");
    new ScanCheck(engine, store, mode);
    return true;
}
