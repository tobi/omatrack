// Cached text textures for the scene-graph trace renderers.
//
// The trace surfaces draw their geometry as QSGGeometryNodes, so the only
// thing left that a rasteriser has to produce is text. Each distinct
// (string, font, colour) is rasterised once into a small QImage, uploaded as a
// QSGTexture, and then composited by the GPU as a textured quad. Lane labels,
// units, axis ticks and corner names never change between frames; cursor
// readouts cycle through a small set of short numeric strings, so after a
// second of hovering the cache hits on nearly every frame.
//
// Ownership: the cache owns every texture it hands out — never delete an
// Entry's texture. Every call must happen on the scene-graph render thread,
// i.e. from updatePaintNode(), and clear() must run before the scene graph is
// invalidated (QQuickItem::releaseResources() or
// QQuickWindow::sceneGraphInvalidated).

#pragma once

#include <QColor>
#include <QFont>
#include <QHash>
#include <QSizeF>
#include <QString>

class QQuickWindow;
class QSGTexture;

class TraceTextCache {
public:
    struct Entry {
        QSGTexture* texture = nullptr;
        QSizeF size;  // device-independent pixels

        bool valid() const { return texture != nullptr; }
    };

    explicit TraceTextCache(int capacity = 384);
    ~TraceTextCache();

    TraceTextCache(const TraceTextCache&) = delete;
    TraceTextCache& operator=(const TraceTextCache&) = delete;

    Entry texture(QQuickWindow* window, const QString& text, const QFont& font,
                  const QColor& color);
    void clear();

private:
    struct Item {
        QSGTexture* texture = nullptr;
        QSizeF size;
        quint64 used = 0;
    };

    void evict();

    QHash<QString, Item> items_;
    quint64 tick_ = 0;
    int capacity_;
};
