#include "GaugeDiscoveryAutotest.h"
#include "ImageTelemetryController.h"
#include "TelemetryStore.h"
#include "inference/GaugeReader.h"
#include "inference/VideoFrameDecoder.h"
#include <QDir>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QTimer>
#include <algorithm>
#include <cmath>

namespace {
bool matchesIncumbent(const QString& source, const QString& model,
                      omatrack::inference::ImageTelemetrySnapshot series,
                      omatrack::IoCancel cancel) {
    using namespace omatrack::inference;
    VideoFrameDecoder decoder;
    GaugeReader reader(model.toStdString());
    if (!series || !reader.ready() ||
        !decoder.open(source.toStdString(), cancel))
        return false;
    int compared = 0;
    for (const auto& cell : series->cells) {
        if (!cell.presentationPtsNs ||
            !std::all_of(cell.values.begin(), cell.values.end(),
                         [](const auto& v) { return v.has_value(); }))
            continue;
        DecodedRgbFrame frame;
        if (!decoder.frameAtOrAfter(*cell.presentationPtsNs, frame, cancel) ||
            frame.presentationPtsNs != *cell.presentationPtsNs)
            return false;
        const auto expected =
            reader.read({frame.pixels.data(), frame.pixels.size(), frame.width,
                         frame.height, frame.stride});
        const std::array<std::optional<double>, 4> values{
            expected.gear.value, expected.stintLap.value,
            expected.brakeFillPct.value, expected.throttleFillPct.value};
        if (expected.error != GaugeError::None || values != cell.values)
            return false;
        if (++compared == 3) return true;
    }
    return false;
}
class GaugeDiscoveryCheck : public QObject {
public:
    GaugeDiscoveryCheck(QQmlApplicationEngine& engine, TelemetryStore& store)
        : QObject(&engine),
          engine_(engine),
          store_(store),
          imageJob_(this),
          oracleJob_(this) {
        total_.start();
        phaseClock_.start();
        timer_.setInterval(100);
        connect(&timer_, &QTimer::timeout, this, [this] { tick(); });
        timer_.start();
    }

private:
    bool require(bool okay, const char* message) {
        if (okay) return true;
        qCritical("GAUGE DISCOVERY FAIL: %s", message);
        timer_.stop();
        QCoreApplication::exit(1);
        return false;
    }
    void enter(int phase) {
        phase_ = phase;
        phaseClock_.restart();
        qInfo("GAUGE DISCOVERY phase %d", phase);
    }
    bool projectionMatchesPlayer() {
        auto* overlay =
            window_->findChild<QQuickItem*>("gaugeDiscoveryOverlay");
        if (!require(overlay, "missing full-video overlay")) return false;
        const auto viewport = player_->mapRectToItem(
            overlay, QRectF(0, 0, player_->width(), player_->height()));
        const double width = std::min(
            viewport.width(), viewport.height() * player_->videoAspectRatio());
        const double height = width / player_->videoAspectRatio();
        return require(
            std::abs(overlay->property("imageX").toDouble() -
                     (viewport.x() + (viewport.width() - width) / 2)) < .01 &&
                std::abs(overlay->property("imageY").toDouble() -
                         (viewport.y() + (viewport.height() - height) / 2)) <
                    .01 &&
                std::abs(overlay->property("imageWidth").toDouble() - width) <
                    .01 &&
                std::abs(overlay->property("imageHeight").toDouble() - height) <
                    .01,
            "source boxes projected against stage rather than actual "
            "letterboxed player");
    }
    void screenshot(const QString& suffix) {
        const auto image = window_->grabWindow();
        const auto path = qEnvironmentVariable("OMATRACK_AUTOTEST") + suffix +
                          QStringLiteral(".png");
        imageJob_.start(
            [image, path](omatrack::IoCancel) { return image.save(path); },
            [this](bool ok) { require(ok, "screenshot save failed"); });
    }
    void tick() {
        if (total_.elapsed() >= 90000) {
            qCritical() << "GAUGE DISCOVERY timeout phase" << phase_ << "status"
                        << (controller_ ? controller_->status() : QString())
                        << "samples"
                        << (controller_ ? controller_->discoverySamples() : 0)
                        << "identity"
                        << (controller_ ? controller_->setupIdentity()
                                        : QString());
            require(false, "deadline exceeded");
            return;
        }
        if (!window_) {
            if (engine_.rootObjects().isEmpty()) return;
            window_ =
                qobject_cast<QQuickWindow*>(engine_.rootObjects().first());
            if (!require(window_, "no window")) return;
            player_ = window_->findChild<MpvVideoItem*>("videoPlayer");
            controller_ = window_->findChild<ImageTelemetryController*>(
                "imageTelemetryController");
            connect(window_, &QQuickWindow::frameSwapped, this, [this] {
                if (phase_ != 2 || !player_ || player_->paused()) {
                    lastFrameMs_ = -1;
                    return;
                }
                const auto now = total_.elapsed();
                if (lastFrameMs_ >= 0)
                    frameIntervals_.push_back(now - lastFrameMs_);
                lastFrameMs_ = now;
            });
            if (!require(player_ && controller_, "missing player/controller"))
                return;
        }
        if (controller_->discoverySamples() > reportedSamples_) {
            reportedSamples_ = controller_->discoverySamples();
            qInfo() << "GAUGE DISCOVERY backend"
                    << controller_->discoveryBackend() << "inference ms"
                    << controller_->discoveryMs() << "load/hash ms"
                    << controller_->discoveryLoadMs();
            if (!require(
                    controller_->discoveryMs() < 3000,
                    "discovery inference exceeded three-second cadence budget"))
                return;
        }
        if (phase_ == 0) {
            if (!store_.ready() || !player_->loaded() ||
                player_->duration() <= 0)
                return;
            if (!require(window_->rendererInterface()->graphicsApi() ==
                             QSGRendererInterface::OpenGL,
                         "real GL context required"))
                return;
            store_.setImageTelemetryEnabled(true);
            const auto mode =
                qEnvironmentVariable("OMATRACK_AUTOTEST_GAUGE_DISCOVERY");
            store_.setGaugeDetectorModel(
                mode == "detector-small" ? QStringLiteral("small")
                : mode.startsWith("detector")
                    ? qEnvironmentVariable(
                          "OMATRACK_AUTOTEST_GAUGE_DETECTOR_MODEL")
                    : QStringLiteral("heuristic"));
            const auto* remember =
                window_->findChild<QObject*>("gaugeRememberExtension");
            if (!require(remember && remember->property("checked").toBool(),
                         "remember extension proposal must default ON"))
                return;
            persistedIdentity_ =
                store_.gaugeProposal(player_->source().toLocalFile())
                    .readingFingerprint();
            persistedSetupIdentity_ =
                store_.gaugeProposal(player_->source().toLocalFile())
                    .fingerprint();
            controller_->retry();
            player_->setPaused(true);
            player_->seek(mode == "detector-restart" ? 9 : 0);
            enter(1);
        } else if (phase_ == 1) {
            if (qEnvironmentVariable("OMATRACK_AUTOTEST_GAUGE_DISCOVERY") ==
                "detector-cancel") {
                if (phaseClock_.elapsed() < 500 || !controller_->running())
                    return;
                const auto blank =
                    qEnvironmentVariable("OMATRACK_AUTOTEST_IMAGE_BLANK");
                if (!require(controller_->discoverySamples() == 0 &&
                                 !blank.isEmpty(),
                             "large source cancellation was not exercised "
                             "during initial work"))
                    return;
                player_->openMedia(QUrl::fromLocalFile(blank));
                enter(15);
                return;
            }
            if (phaseClock_.elapsed() < 4500) return;
            if (!require(controller_->discoverySamples() == 1 &&
                             controller_->inferenceRuns() == 0 &&
                             controller_->scannedSamples() == 0 &&
                             !controller_->canConfirm(),
                         "paused frame inflated evidence or extraction ran "
                         "before confirmation"))
                return;
            if (auto* overlay =
                    window_->findChild<QObject*>("gaugeDiscoveryOverlay"))
                overlay->setProperty("selectedKey",
                                     controller_->gauges()->row(0).key);
            player_->setPaused(false);
            enter(2);
        } else if (phase_ == 2) {
            if (qEnvironmentVariable("OMATRACK_AUTOTEST_GAUGE_DISCOVERY") ==
                "detector-only") {
                if (controller_->discoverySamples() < 3) return;
                if (!require(controller_->status().contains("EXPERIMENTAL") &&
                                 controller_->discoveryBackend() == "tiny-v2" &&
                                 controller_->gauges()->count() > 0 &&
                                 controller_->inferenceRuns() == 0 &&
                                 !controller_->canConfirm() &&
                                 !controller_->canExtract(),
                             "experimental detector missing/mislabeled or "
                             "auto-selected/read"))
                    return;
                for (int i = 0; i < controller_->gauges()->count(); ++i)
                    if (!require(!controller_->gauges()->row(i).selected,
                                 "experimental proposal selected without user "
                                 "action"))
                        return;
                if (!require(controller_->experimentalDetector(),
                             "persistent experimental warning missing"))
                    return;
                screenshot(QStringLiteral("-experimental"));
                enter(8);
                return;
            }
            if (!controller_->canConfirm()) return;
            if (qEnvironmentVariable("OMATRACK_AUTOTEST_GAUGE_DISCOVERY")
                    .startsWith("detector")) {
                int profiles = 0, inventory = 0;
                for (int i = 0; i < controller_->gauges()->count(); ++i) {
                    const auto row = controller_->gauges()->row(i);
                    if (row.origin == "AiM profile") {
                        if (!require(row.selected && row.readable &&
                                         row.evidence >= 3,
                                     "reviewed profile missing fresh "
                                     "evidence/selection/compatibility"))
                            return;
                        ++profiles;
                    } else {
                        if (!require(!row.selected,
                                     "experimental inventory auto-selected"))
                            return;
                        ++inventory;
                    }
                }
                if (!require(
                        controller_->experimentalDetector() && profiles == 4 &&
                            inventory > 0 &&
                            controller_->discoveryBackend() ==
                                (qEnvironmentVariable(
                                     "OMATRACK_AUTOTEST_GAUGE_DISCOVERY") ==
                                         "detector-small"
                                     ? QStringLiteral("tiny-v2")
                                     : QStringLiteral("aim-large-v1")),
                        "default staged V2 did not retain independent "
                        "reviewed profile and inventory"))
                    return;
            }
            if (!require(controller_->discoverySamples() >= 3 &&
                             controller_->inferenceRuns() == 0 &&
                             player_->position() > 4 && !player_->paused(),
                         "discovery blocked playback or read early"))
                return;
            if (!projectionMatchesPlayer()) return;
            screenshot(QStringLiteral("-discovery"));
            if (qEnvironmentVariable("OMATRACK_AUTOTEST_GAUGE_DISCOVERY") ==
                "detector-restart") {
                // An unselected inventory edit must persist without
                // invalidating an otherwise identical confirmed reading's
                // complete cache.
                for (int i = 0; i < controller_->gauges()->count(); ++i) {
                    const auto row = controller_->gauges()->row(i);
                    if (row.selected) continue;
                    auto box = row.box;
                    box.moveLeft(
                        std::clamp(box.x() + .001, 0.0, 1.0 - box.width()));
                    controller_->editGauge(row.key, row.semantic, false, box);
                    break;
                }
            }
            controller_->confirmSetup();
            identity_ = controller_->setupIdentity();
            confirmedBackend_ = controller_->discoveryBackend();
            confirmedSamples_ = controller_->discoverySamples();
            const auto path = player_->source().toLocalFile();
            if (!require(store_.gaugeProposal(path).readingFingerprint() ==
                                 identity_ &&
                             store_.gaugeProposal(path + ".new." +
                                                  path.section('.', -1))
                                     .readingFingerprint() == identity_,
                         "confirmation did not remember per-file and extension "
                         "proposals"))
                return;
            if (qEnvironmentVariable("OMATRACK_AUTOTEST_GAUGE_DISCOVERY") ==
                    "detector-restart" &&
                !require(store_.gaugeProposal(path).fingerprint() !=
                             persistedSetupIdentity_,
                         "cold restart did not exercise changed unselected "
                         "inventory"))
                return;
            if (!require(controller_->phase() ==
                                 ImageTelemetryController::Confirmed &&
                             controller_->inferenceRuns() == 0,
                         "confirmation started extraction"))
                return;
            enter(3);
        } else if (phase_ == 3) {
            if (phaseClock_.elapsed() < 2200 || imageJob_.running()) return;
            if (!require(
                    controller_->inferenceRuns() == 0 &&
                        controller_->setupIdentity() == identity_ &&
                        controller_->discoveryBackend() == confirmedBackend_ &&
                        controller_->discoverySamples() == confirmedSamples_,
                    "confirmed state read or changed routing identity without "
                    "explicit action"))
                return;
            if (qEnvironmentVariable("OMATRACK_AUTOTEST_GAUGE_DISCOVERY")
                    .endsWith("native")) {
                if (!require(
                        !controller_->eligible() && !controller_->canExtract(),
                        "native priority lost"))
                    return;
                finish();
                return;
            }
            if (!require(controller_->canExtract(),
                         "reviewed offline reader unavailable"))
                return;
            controller_->startExtraction();
            if (qEnvironmentVariable("OMATRACK_AUTOTEST_GAUGE_DISCOVERY")
                    .endsWith("restart")) {
                if (!require(identity_ == persistedIdentity_,
                             "inventory churn changed identical confirmed "
                             "reading identity"))
                    return;
                enter(9);
            } else
                enter(4);
        } else if (phase_ == 4) {
            if (controller_->knownSamples() < 3) return;
            if (!require(controller_->inferenceRuns() > 0 &&
                             controller_->phase() ==
                                 ImageTelemetryController::Extracting,
                         "explicit action did not extract"))
                return;
            if (qEnvironmentVariable("OMATRACK_AUTOTEST_GAUGE_DISCOVERY")
                    .startsWith("detector")) {
                const auto source = player_->source().toLocalFile();
                const auto model =
                    controller_->modelPath().isEmpty()
                        ? QDir(QCoreApplication::applicationDirPath())
                              .filePath("models/gauge-reader.onnx")
                        : controller_->modelPath();
                oracleJob_.start(
                    [source, model, series = controller_->series()](
                        omatrack::IoCancel cancel) {
                        return matchesIncumbent(source, model, series, cancel);
                    },
                    [this](bool ok) { oraclePassed_ = ok; });
                enter(12);
                return;
            }
            enter(13);
        } else if (phase_ == 13) {
            const auto gear = controller_->gauges()->row(0);
            controller_->editGauge(gear.key, gear.semantic, false, gear.box);
            if (!require(controller_->phase() ==
                                 ImageTelemetryController::Discovery &&
                             controller_->knownSamples() == 0 &&
                             controller_->setupIdentity() != identity_,
                         "edit kept setup-dependent old data"))
                return;
            controller_->confirmSetup(false);
            if (!require(controller_->canExtract(),
                         "selected subset cannot extract"))
                return;
            controller_->startExtraction();
            enter(5);
        } else if (phase_ == 5) {
            if (controller_->knownSamples() < 3) return;
            const auto series = controller_->series();
            for (const auto& cell : series->cells)
                if (!require(!cell.values[0],
                             "disabled gear leaked numeric values"))
                    return;
            if (!require(QString::fromStdString(series->identity.setupSha256) ==
                             controller_->setupIdentity(),
                         "cache does not carry confirmed setup identity"))
                return;
            screenshot(QStringLiteral("-extraction"));
            player_->seek(18);
            if (!require(!controller_->valid(),
                         "seek did not invalidate current reading"))
                return;
            enter(6);
        } else if (phase_ == 6) {
            if (phaseClock_.elapsed() < 2000 || imageJob_.running()) return;
            const auto blank =
                qEnvironmentVariable("OMATRACK_AUTOTEST_IMAGE_BLANK");
            if (!require(!blank.isEmpty(), "blank source required")) return;
            player_->openMedia(QUrl::fromLocalFile(blank));
            enter(7);
        } else if (phase_ == 7) {
            if (phaseClock_.elapsed() < 4000 || !player_->loaded()) return;
            if (!require(controller_->phase() ==
                                 ImageTelemetryController::Discovery &&
                             controller_->inferenceRuns() == 0 &&
                             controller_->knownSamples() == 0 &&
                             !controller_->valid() &&
                             !controller_->canConfirm(),
                         "source switch/default proposal trusted stale "
                         "readings or confirmation"))
                return;
            // Visual confirmation of an extension proposal is not a substitute
            // for current-image structural admission on an unknown layout.
            for (int i = 0; i < controller_->gauges()->count(); ++i)
                controller_->confirmGauge(controller_->gauges()->row(i).key);
            controller_->confirmSetup(false);
            controller_->startExtraction();
            if (!require(!controller_->canExtract() &&
                             controller_->inferenceRuns() == 0 &&
                             !controller_->series(),
                         "unknown layout accepted a visually confirmed saved "
                         "profile"))
                return;
            finish();
        } else if (phase_ == 8) {
            if (imageJob_.running()) return;
            const auto proposal = controller_->gauges()->row(0);
            controller_->editGauge(proposal.key, proposal.semantic, true,
                                   proposal.box);
            controller_->confirmGauge(proposal.key);
            controller_->confirmSetup(false);
            enter(10);
        } else if (phase_ == 9) {
            if (!controller_->cacheComplete()) return;
            if (!require(controller_->inferenceRuns() == 0 &&
                             controller_->complete() && controller_->series() &&
                             QString::fromStdString(
                                 controller_->series()->identity.setupSha256) ==
                                 identity_,
                         "cold restart did not revalidate/preserve confirmed "
                         "setup and reuse complete cache without inference"))
                return;
            qInfo(
                "GAUGE DISCOVERY RESTART PASS: persisted proposal revalidated; "
                "confirmed cache reused; reader calls 0");
            finish();
        } else if (phase_ == 10) {
            const auto* warning =
                window_->findChild<QQuickItem*>("gaugeExperimentalWarning");
            if (!require(controller_->phase() ==
                                 ImageTelemetryController::Confirmed &&
                             controller_->experimentalDetector() && warning &&
                             warning->isVisible() &&
                             !controller_->canExtract() &&
                             controller_->inferenceRuns() == 0,
                         "confirmation hid experimental warning or admitted "
                         "detected crop"))
                return;
            qInfo(
                "GAUGE DISCOVERY EXPERIMENTAL PASS: confirmed proposal keeps "
                "warning; detected crop unreadable");
            screenshot(QStringLiteral("-experimental-confirmed"));
            enter(11);
        } else if (phase_ == 11) {
            if (!imageJob_.running()) finish();
        } else if (phase_ == 12) {
            if (oracleJob_.running()) return;
            if (!require(oraclePassed_,
                         "staged-V2 profile extraction disagrees with "
                         "incumbent reader at actual decoded PTS"))
                return;
            qInfo(
                "GAUGE DISCOVERY PROFILE PASS: routed detector plus "
                "independent AiM "
                "profile; 12/12 values exactly match incumbent; no Preferences "
                "switch");
            if (qEnvironmentVariable("OMATRACK_AUTOTEST_GAUGE_DISCOVERY") ==
                "detector-seed") {
                controller_->setScanAhead(true);
                enter(14);
            } else
                enter(13);
        } else if (phase_ == 14) {
            if (!controller_->cacheComplete()) return;
            qInfo()
                << "GAUGE DISCOVERY SEED PASS: complete selected-profile cache"
                << controller_->setupIdentity();
            screenshot(QStringLiteral("-profile-complete"));
            enter(11);
        } else if (phase_ == 15) {
            if (phaseClock_.elapsed() < 4000 || !player_->loaded() ||
                controller_->running())
                return;
            if (!require(controller_->phase() ==
                                 ImageTelemetryController::Discovery &&
                             controller_->discoveryBackend() == "tiny-v2" &&
                             controller_->inferenceRuns() == 0 &&
                             !controller_->series() &&
                             !controller_->canConfirm() &&
                             !controller_->canExtract(),
                         "cancelled large/source work leaked backend, evidence "
                         "or readings"))
                return;
            qInfo(
                "GAUGE DISCOVERY CANCEL PASS: in-flight large initialization "
                "discarded; new unknown source uses tiny; no reader calls");
            screenshot(QStringLiteral("-cancelled-source"));
            enter(11);
        }
    }
    void finish() {
        if (!frameIntervals_.empty()) {
            std::sort(frameIntervals_.begin(), frameIntervals_.end());
            const auto p95 = frameIntervals_[std::min(
                frameIntervals_.size() - 1,
                std::size_t(std::ceil(frameIntervals_.size() * .95)) - 1)];
            qInfo() << "GAUGE DISCOVERY responsiveness: frames"
                    << frameIntervals_.size() << "interval ms p50/p95/max"
                    << frameIntervals_[frameIntervals_.size() / 2] << p95
                    << frameIntervals_.back();
            if (!require(
                    frameIntervals_.size() >= 30 && p95 < 100 &&
                        frameIntervals_.back() < 500,
                    "playback callbacks stalled during background discovery"))
                return;
        }
        qInfo(
            "GAUGE DISCOVERY PASS: fresh PTS, confirmation/action gate, "
            "offline model, masks, setup identity, source/seek cancellation");
        timer_.stop();
        QCoreApplication::exit(0);
    }
    QQmlApplicationEngine& engine_;
    TelemetryStore& store_;
    QQuickWindow* window_ = nullptr;
    MpvVideoItem* player_ = nullptr;
    ImageTelemetryController* controller_ = nullptr;
    QTimer timer_;
    QElapsedTimer total_, phaseClock_;
    AsyncJob<bool> imageJob_, oracleJob_;
    QString identity_, persistedIdentity_, persistedSetupIdentity_,
        confirmedBackend_;
    bool oraclePassed_ = false;
    int phase_ = 0, confirmedSamples_ = 0, reportedSamples_ = 0;
    qint64 lastFrameMs_ = -1;
    std::vector<qint64> frameIntervals_;
};
}  // namespace
bool omatrack::autotest::installGaugeDiscovery(QQmlApplicationEngine& engine,
                                               TelemetryStore& store) {
    if (!qEnvironmentVariableIsSet("OMATRACK_AUTOTEST_GAUGE_DISCOVERY"))
        return false;
    new GaugeDiscoveryCheck(engine, store);
    return true;
}
