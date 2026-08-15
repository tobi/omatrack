// Shared scene-graph builder for the telemetry surfaces.
//
// Every trace surface (lanes, cursor overlay, damper strip, video HUD) draws
// the same two things: flat-coloured quads and short strings. This builder
// batches all quads of a frame into one QSGGeometryNode with vertex colours —
// one draw call for the whole surface — and composites text as cached
// textures. Nothing goes through QPainter on the frame path, so the renderer
// is GPU-bound instead of rasterisation-bound.
//
// Lines are expanded to quads on purpose: OpenGL core profile clamps
// glLineWidth to 1, so a 2 px trace has to be geometry. Edge antialiasing
// comes from the window's multisample format, set in main().
//
// Usage, from updatePaintNode() only (render thread):
//     builder.begin(window());
//     builder.rect(...); builder.polyline(...); builder.text(...);
//     builder.commit(root);
// and call releaseResources() from QQuickItem::releaseResources().

#pragma once

#include <QColor>
#include <QFont>
#include <QPointF>
#include <QRectF>
#include <QSGGeometry>
#include <QVector>

#include <algorithm>

#include "TraceTextCache.h"

class QQuickWindow;
class QSGNode;
class QSGGeometryNode;
class QSGSimpleTextureNode;
class QSGTransformNode;

class TraceSceneBuilder {
public:
    TraceSceneBuilder();

    /// Starts a frame. Clears the accumulated geometry, keeps the text cache.
    void begin(QQuickWindow* window);

    void rect(const QRectF& rect, const QColor& color);
    void line(const QPointF& from, const QPointF& to, qreal width,
              const QColor& color);
    /// Vertical/horizontal helpers: pixel-aligned so 1 px rules stay crisp.
    void vLine(qreal x, qreal top, qreal bottom, qreal width,
               const QColor& color);
    void hLine(qreal y, qreal left, qreal right, qreal width,
               const QColor& color);
    void polyline(const QPointF* points, int count, qreal width,
                  const QColor& color);
    void fillQuad(const QPointF& a, const QPointF& b, const QPointF& c,
                  const QPointF& d, const QColor& color);
    void dot(const QPointF& center, qreal radius, const QColor& color);
    /// Text is laid out inside `box` with `alignment`; the returned width is
    /// the measured text width, 0 when nothing was emitted.
    qreal text(const QString& text, const QFont& font, const QColor& color,
               const QRectF& box, Qt::Alignment alignment);
    /// Text rotated by `degrees` around `pivot`; the alignment applies to the
    /// unrotated string, so AlignHCenter centres it on the pivot.
    qreal rotatedText(const QString& text, const QFont& font,
                      const QColor& color, const QPointF& pivot,
                      Qt::Alignment alignment, qreal degrees);
    void reserveQuads(int quads);

    /// Writes the frame into `root`, reusing its child nodes.
    void commit(QSGNode* root);
    void releaseResources();

    int quadCount() const { return vertices_.size() / 6; }

private:
    struct TextQuad {
        QSGTexture* texture = nullptr;
        QRectF rect;
        qreal rotation = 0.0;
    };

    struct TextNode {
        QSGTransformNode* transform = nullptr;
        QSGSimpleTextureNode* texture = nullptr;
    };

    void quad(const QPointF& a, const QPointF& b, const QPointF& c,
              const QPointF& d, const QColor& color);

    QVector<QSGGeometry::ColoredPoint2D> vertices_;
    QVector<quint32> indices_;
    QVector<TextQuad> texts_;
    QVector<TextNode> textNodes_;
    bool softwareFallback_ = false;
    QSGGeometryNode* geometryNode_ = nullptr;
    QQuickWindow* window_ = nullptr;
    TraceTextCache cache_;
};
