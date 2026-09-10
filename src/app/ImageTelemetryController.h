// Progressive image-derived telemetry ingestion, independent of native
// sessions.
#pragma once

#include "AsyncJob.h"
#include "MpvVideoItem.h"
#include "GaugeSetup.h"
#include "StoreModels.h"
#include "inference/ImageTelemetrySeries.h"

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QThreadPool>
#include <QTimer>
#include <QtQml/qqmlregistration.h>
#include <memory>
#include <optional>

struct ImageTelemetryWorker;
struct ImageTelemetryResult;
struct GaugeDiscoveryResult;
class TelemetryStore;

class ImageTelemetryController : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(TelemetryStore* store READ store WRITE setStore NOTIFY
                   storeChanged FINAL)
    Q_PROPERTY(GaugeRegionModel* gauges READ gauges CONSTANT FINAL)
    Q_PROPERTY(Phase phase READ phase NOTIFY setupChanged FINAL)
    Q_PROPERTY(bool canConfirm READ canConfirm NOTIFY setupChanged FINAL)
    Q_PROPERTY(bool canExtract READ canExtract NOTIFY setupChanged FINAL)
    Q_PROPERTY(bool experimentalDetector READ experimentalDetector NOTIFY
                   setupChanged FINAL)
    Q_PROPERTY(QString discoveryBackend READ discoveryBackend NOTIFY
                   setupChanged FINAL)
    Q_PROPERTY(double discoveryMs READ discoveryMs NOTIFY setupChanged FINAL)
    Q_PROPERTY(
        double discoveryLoadMs READ discoveryLoadMs NOTIFY setupChanged FINAL)
    Q_PROPERTY(bool geometryCompatible READ geometryCompatible NOTIFY
                   setupChanged FINAL)
    Q_PROPERTY(
        QString setupIdentity READ setupIdentity NOTIFY setupChanged FINAL)
    Q_PROPERTY(
        int discoverySamples READ discoverySamples NOTIFY setupChanged FINAL)
    Q_PROPERTY(MpvVideoItem* player READ player WRITE setPlayer NOTIFY
                   playerChanged FINAL)
    Q_PROPERTY(
        bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged FINAL)
    // Per-video opt-in. Nothing is decoded or detected until the user asks;
    // a new source, a reopen or a settings change turns it off again.
    Q_PROPERTY(bool discovering READ discovering WRITE setDiscovering NOTIFY
                   discoveringChanged FINAL)
    Q_PROPERTY(bool eligible READ eligible WRITE setEligible NOTIFY
                   eligibleChanged FINAL)
    Q_PROPERTY(QString modelPath READ modelPath WRITE setModelPath NOTIFY
                   modelPathChanged FINAL)
    Q_PROPERTY(QString detectorPath READ detectorPath WRITE setDetectorPath
                   NOTIFY detectorPathChanged FINAL)
    Q_PROPERTY(QString status READ status NOTIFY statusChanged FINAL)
    Q_PROPERTY(bool available READ available CONSTANT FINAL)
    Q_PROPERTY(bool scanAhead READ scanAhead WRITE setScanAhead NOTIFY
                   scanStateChanged FINAL)
    Q_PROPERTY(bool running READ running NOTIFY scanStateChanged FINAL)
    Q_PROPERTY(
        int scannedSamples READ scannedSamples NOTIFY timelineChanged FINAL)
    Q_PROPERTY(int knownSamples READ knownSamples NOTIFY timelineChanged FINAL)
    Q_PROPERTY(
        int inferenceRuns READ inferenceRuns NOTIFY timelineChanged FINAL)
    Q_PROPERTY(double progress READ progress NOTIFY timelineChanged FINAL)
    Q_PROPERTY(double duration READ duration NOTIFY timelineChanged FINAL)
    Q_PROPERTY(bool complete READ complete NOTIFY timelineChanged FINAL)
    Q_PROPERTY(
        bool cacheComplete READ cacheComplete NOTIFY timelineChanged FINAL)
    Q_PROPERTY(QString cachePath READ cachePath NOTIFY timelineChanged FINAL)
    Q_PROPERTY(bool valid READ valid NOTIFY sampleChanged FINAL)
    Q_PROPERTY(double gear READ gear NOTIFY sampleChanged FINAL)
    Q_PROPERTY(double stintLap READ stintLap NOTIFY sampleChanged FINAL)
    Q_PROPERTY(double brakeFillPct READ brakeFillPct NOTIFY sampleChanged FINAL)
    Q_PROPERTY(
        double throttleFillPct READ throttleFillPct NOTIFY sampleChanged FINAL)
    Q_PROPERTY(double sampleTime READ sampleTime NOTIFY sampleChanged FINAL)
    Q_PROPERTY(qint64 sourcePtsNs READ sourcePtsNs NOTIFY sampleChanged FINAL)
    Q_PROPERTY(double inferenceMs READ inferenceMs NOTIFY sampleChanged FINAL)
    Q_PROPERTY(double totalMs READ totalMs NOTIFY sampleChanged FINAL)
    Q_PROPERTY(int observations READ inferenceRuns NOTIFY timelineChanged FINAL)

public:
    enum Phase { Discovery, Confirmed, Extracting };
    Q_ENUM(Phase)
    explicit ImageTelemetryController(QObject* parent = nullptr);
    TelemetryStore* store() const;
    void setStore(TelemetryStore* store);
    GaugeRegionModel* gauges() { return &gauges_; }
    Phase phase() const { return phase_; }
    bool canConfirm() const;
    bool canExtract() const;
    QString setupIdentity() const {
        return evidence_.setup.readingFingerprint();
    }
    int discoverySamples() const { return evidence_.sampleCount(); }
    bool geometryCompatible() const { return geometryCompatible_; }
    bool experimentalDetector() const {
        return evidence_.setup.detectorIdentity.startsWith(
            QStringLiteral("experimental-"));
    }
    QString discoveryBackend() const { return discoveryBackend_; }
    double discoveryMs() const { return discoveryMs_; }
    double discoveryLoadMs() const { return discoveryLoadMs_; }
    Q_INVOKABLE virtual void confirmSetup(bool extensionDefault = true) final;
    Q_INVOKABLE virtual void confirmGauge(const QString& key) final;
    Q_INVOKABLE virtual void startExtraction() final;
    Q_INVOKABLE virtual void reviewSetup() final;
    Q_INVOKABLE virtual void editGauge(const QString& key,
                                       const QString& semantic, bool selected,
                                       const QRectF& box,
                                       const QString& representation = {},
                                       const QString& direction = {}) final;

    ~ImageTelemetryController() override;
    MpvVideoItem* player() const { return player_; }
    void setPlayer(MpvVideoItem* player);
    bool enabled() const { return enabled_; }
    void setEnabled(bool enabled);
    bool discovering() const { return discovering_; }
    void setDiscovering(bool discovering);
    bool eligible() const { return eligible_; }
    void setEligible(bool eligible);
    const QString& modelPath() const { return modelPath_; }
    void setModelPath(const QString& path);
    QString detectorPath() const { return detectorPath_; }
    void setDetectorPath(const QString& path);
    const QString& status() const { return status_; }
    bool available() const;
    bool scanAhead() const { return scanAhead_; }
    void setScanAhead(bool enabled);
    bool running() const { return job_.running() || discoveryJob_.running(); }
    int scannedSamples() const { return scanned_; }
    int knownSamples() const { return known_; }
    int inferenceRuns() const { return inferenceRuns_; }
    int observations() const { return inferenceRuns_; }
    double progress() const;
    double duration() const;
    bool complete() const { return complete_; }
    bool cacheComplete() const { return cacheComplete_; }
    QString cachePath() const { return cachePath_; }
    omatrack::inference::ImageTelemetrySnapshot series() const {
        return series_;
    }
    bool valid() const { return valid_; }
    double gear() const { return values_[0]; }
    double stintLap() const { return values_[1]; }
    double brakeFillPct() const { return values_[2]; }
    double throttleFillPct() const { return values_[3]; }
    double sampleTime() const { return sampleTime_; }
    qint64 sourcePtsNs() const { return sourcePtsNs_; }
    double inferenceMs() const { return inferenceMs_; }
    double totalMs() const { return totalMs_; }
    Q_INVOKABLE void retry();

signals:
    void storeChanged();
    void setupChanged();
    void playerChanged();
    void enabledChanged();
    void discoveringChanged();
    void eligibleChanged();
    void modelPathChanged();
    void detectorPathChanged();
    void statusChanged();
    void sampleChanged();
    void scanStateChanged();
    void timelineChanged();

private:
    void discover(double seconds);
    void refreshGauges();
    void refreshGeometry();
    void clearReading();
    void reset();
    void resetForSeek();
    void retireWorker();
    void sample();
    void setStatus(const QString& message);
    QString idleStatus() const;
    void invalidate();
    void refreshCurrent();
    void apply(const std::shared_ptr<ImageTelemetryResult>& result);

    QPointer<MpvVideoItem> player_;
    QPointer<TelemetryStore> store_;
    GaugeRegionModel gauges_;
    omatrack::GaugeEvidence evidence_;
    Phase phase_ = Discovery;
    bool proposalLoaded_ = false, geometryCompatible_ = false;
    double lastDiscoveryTarget_ = -10;
    qint64 nextDiscoveryMs_ = 0;
    AsyncJob<std::shared_ptr<GaugeDiscoveryResult>> discoveryJob_;
    QTimer timer_;
    QThreadPool workerPool_;
    AsyncJob<std::shared_ptr<ImageTelemetryResult>> job_;
    AsyncJob<int> disposeJob_;
    std::shared_ptr<ImageTelemetryWorker> worker_;
    omatrack::inference::ImageTelemetrySnapshot series_;
    QElapsedTimer clock_;
    QString modelPath_, detectorPath_, status_, cachePath_, discoveryBackend_;
    double discoveryMs_ = 0, discoveryLoadMs_ = 0;
    bool enabled_ = true, discovering_ = false, eligible_ = false,
         scanAhead_ = false;
    bool valid_ = false, blocked_ = false, awaitingSeek_ = false;
    bool complete_ = false, cacheComplete_ = false, pendingSave_ = false,
         pendingWatch_ = false;
    bool reanchor_ = false;
    qint64 nextAttemptMs_ = 0;
    double values_[4];
    double sampleTime_ = -1.0, inferenceMs_ = 0.0, totalMs_ = 0.0;
    qint64 sourcePtsNs_ = 0;
    int scanned_ = 0, known_ = 0, inferenceRuns_ = 0;
};
