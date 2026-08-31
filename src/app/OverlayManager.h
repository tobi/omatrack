// Owns MTX sidecar overlay groups, the overlay channel cache, the
// attach / resample / sidecar-library / discovery background jobs, and the
// host-window computations that place sidecars on the active session's
// timeline. Emits overlaysChanged / sidecarLibraryChanged /
// channelConfigChanged; the store forwards those to QML.
#pragma once

#include "AsyncJob.h"
#include "TelemetryStore.h"  // OverlayGroup, OverlayChannel, SessionHandle, VideoHudSeries, LapEntry

#include "LibraryLocation.h"

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QVariantList>
#include <QVector>

#include <memory>
#include <vector>

struct SidecarLoadResult {
    QString path;
    QString error;
    bool notExtension = false;
    OverlayGroup group;
    QHash<QString, std::shared_ptr<std::vector<double>>> samples;
};

class OverlayManager : public QObject {
    Q_OBJECT
public:
    explicit OverlayManager(QObject* parent = nullptr);
    ~OverlayManager() override = default;

    // ── selection state (set by the store) ───────────────────────────
    void setPrimarySession(SessionHandle* session) {
        primarySession_ = session;
    }
    void setCompareSession(SessionHandle* session) {
        compareSession_ = session;
    }
    void setPrimaryLap(int lapId) { primaryLap_ = lapId; }
    void setLocations(const QVector<omatrack::LibraryLocation>* locations) {
        locations_ = locations;
    }
    void setEventLabel(const QString& label) { eventLabel_ = label; }
    void setDriverName(const QString& name) { driverName_ = name; }

    // ── host window ──────────────────────────────────────────────────
    bool hostWindowNs(qint64* startNs, qint64* endNs, qint64* utcNs) const;
    bool videoClipWindowNs(qint64* startNs, qint64* endNs) const;

    // ── overlay management ───────────────────────────────────────────
    void attachSidecar(const QString& filePath, bool fromOpen, bool silent);
    /// Adopt a plugin's group (channels carry explicit times and values).
    /// Replaces a previous group of the same plugin; resamples onto the lap.
    void attachPluginGroup(OverlayGroup group);
    void removePluginGroup(const QString& pluginId);
    void discoverSidecarSiblings();
    void resampleOverlays();
    void removeOverlay(const QString& id);
    void setOverlayExpanded(const QString& id, bool expanded);
    bool overlayExpanded(const QString& id) const;

    const QVector<OverlayGroup>& overlayGroups() const { return overlays_; }
    const std::vector<double>* overlayChannelData(const QString& key) const;
    QVariantList sidecarLibrary() const { return sidecarLibrary_; }
    void refreshSidecarLibraryAttachment(const QString& path, bool attached);

    static bool isMtxSidecarPath(const QString& filePath);
    void clear();

signals:
    void overlaysChanged();
    void sidecarLibraryChanged();
    void channelConfigChanged();
    void openAsFile(const QString& path);
    void operationError(const QString& title, const QString& message);

private:
    bool adoptOverlay(
        OverlayGroup group,
        QHash<QString, std::shared_ptr<std::vector<double>>> samples,
        QString* error);

    SessionHandle* primarySession_ = nullptr;
    SessionHandle* compareSession_ = nullptr;
    const QVector<omatrack::LibraryLocation>* locations_ = nullptr;
    int primaryLap_ = -1;
    QString eventLabel_;
    QString driverName_;

    QVector<OverlayGroup> overlays_;
    QHash<QString, std::shared_ptr<std::vector<double>>> overlayChannelCache_;
    QSet<QString> overlayLoading_;
    QVariantList sidecarLibrary_;
    QSet<QString> sidecarLibraryLoading_;

    AsyncJob<QHash<QString, std::shared_ptr<std::vector<double>>>>
        overlayResampleJob_;
    SerialJobQueue<SidecarLoadResult> overlayAttachQueue_;
    SerialJobQueue<SidecarLoadResult> sidecarLibraryQueue_;
    AsyncJob<QStringList> sidecarDiscoveryJob_;
};
