#include "GaugeDiscoveryAutotest.h"
#include "ImageTelemetryController.h"
#include "TelemetryStore.h"
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
class GaugeDiscoveryCheck : public QObject {
public:
    GaugeDiscoveryCheck(QQmlApplicationEngine& engine, TelemetryStore& store)
        : QObject(&engine), engine_(engine), store_(store), imageJob_(this) {
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
            if (!require(player_ && controller_, "missing player/controller"))
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
                mode == "detector"
                    ? qEnvironmentVariable(
                          "OMATRACK_AUTOTEST_GAUGE_DETECTOR_MODEL")
                    : QStringLiteral("heuristic"));
            const auto* remember =
                window_->findChild<QObject*>("gaugeRememberExtension");
            if (!require(remember && remember->property("checked").toBool(),
                         "remember extension proposal must default ON"))
                return;
            controller_->retry();
            player_->setPaused(true);
            player_->seek(0);
            enter(1);
        } else if (phase_ == 1) {
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
                "detector") {
                if (controller_->discoverySamples() < 3) return;
                if (!require(controller_->status().contains(
                                 "EXPERIMENTAL detector") &&
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
            if (!require(controller_->discoverySamples() >= 3 &&
                             controller_->inferenceRuns() == 0 &&
                             player_->position() > 4 && !player_->paused(),
                         "discovery blocked playback or read early"))
                return;
            if (!projectionMatchesPlayer()) return;
            screenshot(QStringLiteral("-discovery"));
            controller_->confirmSetup();
            identity_ = controller_->setupIdentity();
            const auto path = player_->source().toLocalFile();
            if (!require(
                    store_.gaugeProposal(path).fingerprint() == identity_ &&
                        store_.gaugeProposal(path + ".new." +
                                             path.section('.', -1))
                                .fingerprint() == identity_,
                    "confirmation did not remember per-file and extension "
                    "proposals"))
                return;
            if (!require(controller_->phase() ==
                                 ImageTelemetryController::Confirmed &&
                             controller_->inferenceRuns() == 0,
                         "confirmation started extraction"))
                return;
            enter(3);
        } else if (phase_ == 3) {
            if (phaseClock_.elapsed() < 2200 || imageJob_.running()) return;
            if (!require(controller_->inferenceRuns() == 0,
                         "confirmed state read without explicit action"))
                return;
            if (qEnvironmentVariable("OMATRACK_AUTOTEST_GAUGE_DISCOVERY") ==
                "native") {
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
            enter(qEnvironmentVariable("OMATRACK_AUTOTEST_GAUGE_DISCOVERY") ==
                          "restart"
                      ? 9
                      : 4);
        } else if (phase_ == 4) {
            if (controller_->knownSamples() < 3) return;
            if (!require(controller_->inferenceRuns() > 0 &&
                             controller_->phase() ==
                                 ImageTelemetryController::Extracting,
                         "explicit action did not extract"))
                return;
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
        }
    }
    void finish() {
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
    AsyncJob<bool> imageJob_;
    QString identity_;
    int phase_ = 0;
};
}  // namespace
bool omatrack::autotest::installGaugeDiscovery(QQmlApplicationEngine& engine,
                                               TelemetryStore& store) {
    if (!qEnvironmentVariableIsSet("OMATRACK_AUTOTEST_GAUGE_DISCOVERY"))
        return false;
    new GaugeDiscoveryCheck(engine, store);
    return true;
}
