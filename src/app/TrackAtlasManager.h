// Owns the Track Atlas snapshot, per-layout centerline geometry, spatial
// station mappings, and the background refresh / centerline / cache jobs.
// Emits changed() when the atlas state (tracks, status) changes and
// cornersNeedReload() when a freshly arrived centerline requires the store
// to recompute corner zones.
#pragma once

#include "AsyncJob.h"
#include "TelemetryStore.h"  // CornerZone, SessionHandle

#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QPointF>
#include <QString>
#include <QVector>

#include <memory>

class TrackAtlasManager : public QObject {
    Q_OBJECT
public:
    explicit TrackAtlasManager(QObject* parent = nullptr);
    ~TrackAtlasManager() override = default;

    void startup();

    // ── atlas data ───────────────────────────────────────────────────
    const QHash<QString, QJsonObject>& tracks() const { return atlasTracks_; }
    bool isEmpty() const { return atlasTracks_.isEmpty(); }
    bool contains(const QString& slug) const {
        return atlasTracks_.contains(slug);
    }
    const QJsonObject value(const QString& slug) const {
        return atlasTracks_.value(slug);
    }
    QString status() const { return trackAtlasStatus_; }

    // ── refresh / cache ──────────────────────────────────────────────
    void refreshTrackAtlas();
    void updateTrackAtlas(bool force);
    void loadTrackAtlasCache();
    void clearSpatialMappings() { atlasSpatialMappings_.clear(); }
    bool parseTrackAtlas(const QByteArray& payload);

    QString trackAtlasCachePath() const;
    QString trackAtlasGeometryCachePath(const QString& trackSlug,
                                        const QString& layoutId) const;

    // ── centerline / spatial ─────────────────────────────────────────
    bool ensureAtlasCenterline(const QString& trackSlug,
                               const QJsonObject& layout);
    void requestAtlasCenterline(const QString& trackSlug,
                                const QJsonObject& layout);
    void loadAtlasCenterlineFromCache(const QString& trackSlug,
                                      const QJsonObject& layout);

    // ── corner computation (input side) ──────────────────────────────
    // Computes atlas-derived corner zones for the active lap. `resolvedSlug`
    // is the store's resolved track slug (assigned or detected). Sets the
    // atlas status internally and emits changed() for status updates; does
    // NOT emit cornersNeedReload() (the store drives that).
    QVector<CornerZone> cornersForPrimary(SessionHandle* session, int lapId,
                                          const QString& resolvedSlug);

signals:
    void changed();
    void cornersNeedReload();

private:
    QHash<QString, QJsonObject> atlasTracks_;
    QHash<QString, QVector<QPointF>> atlasCenterlines_;
    QHash<QString, QVector<QPointF>> atlasSpatialMappings_;
    QSet<QString> atlasGeometryRequests_;
    mutable QString atlasCachePath_;
    mutable bool atlasCachePathReady_ = false;
    QString trackAtlasStatus_;
    QTimer* atlasTimer_ = nullptr;

    AsyncJob<QByteArray> atlasRefreshJob_;
    SerialJobQueue<QByteArray> atlasCenterlineQueue_;
    SerialJobQueue<std::pair<QVector<QPointF>, bool>> atlasCacheQueue_;
};
