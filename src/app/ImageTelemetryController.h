// Progressive image-derived telemetry ingestion, independent of native
// sessions.
#pragma once

#include "AsyncJob.h"
#include "MpvVideoItem.h"
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

class ImageTelemetryController : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(MpvVideoItem* player READ player WRITE setPlayer NOTIFY
                   playerChanged FINAL)
    Q_PROPERTY(
        bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged FINAL)
    Q_PROPERTY(bool eligible READ eligible WRITE setEligible NOTIFY
                   eligibleChanged FINAL)
    Q_PROPERTY(QString modelPath READ modelPath WRITE setModelPath NOTIFY
                   modelPathChanged FINAL)
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
    explicit ImageTelemetryController(QObject* parent = nullptr);
    ~ImageTelemetryController() override;
    MpvVideoItem* player() const { return player_; }
    void setPlayer(MpvVideoItem* player);
    bool enabled() const { return enabled_; }
    void setEnabled(bool enabled);
    bool eligible() const { return eligible_; }
    void setEligible(bool eligible);
    const QString& modelPath() const { return modelPath_; }
    void setModelPath(const QString& path);
    const QString& status() const { return status_; }
    bool available() const;
    bool scanAhead() const { return scanAhead_; }
    void setScanAhead(bool enabled);
    bool running() const { return job_.running(); }
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
    void playerChanged();
    void enabledChanged();
    void eligibleChanged();
    void modelPathChanged();
    void statusChanged();
    void sampleChanged();
    void scanStateChanged();
    void timelineChanged();

private:
    void reset();
    void resetForSeek();
    void retireWorker();
    void sample();
    void setStatus(const QString& message);
    void invalidate();
    void refreshCurrent();
    void apply(const std::shared_ptr<ImageTelemetryResult>& result);

    QPointer<MpvVideoItem> player_;
    QTimer timer_;
    QThreadPool workerPool_;
    AsyncJob<std::shared_ptr<ImageTelemetryResult>> job_;
    AsyncJob<int> disposeJob_;
    std::shared_ptr<ImageTelemetryWorker> worker_;
    omatrack::inference::ImageTelemetrySnapshot series_;
    QElapsedTimer clock_;
    QString modelPath_, status_, cachePath_;
    bool enabled_ = true, eligible_ = false, scanAhead_ = false;
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
