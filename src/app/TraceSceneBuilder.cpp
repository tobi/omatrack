#include "TraceSceneBuilder.h"

#include <QQuickWindow>
#include <QSGGeometryNode>
#include <QSGSimpleTextureNode>
#include <QSGTransformNode>
#include <QSGVertexColorMaterial>

#include <cmath>
#include <cstring>

namespace {

constexpr int kCircleSegments = 10;

// The software adaptation of the scene graph cannot render custom geometry
// nodes — it crashes on them. It is what the offscreen platform falls back to
// when no GL context exists, so guard rather than trust the platform: these
// surfaces need a real GPU context and render empty without one.
bool supportsGeometryNodes(QQuickWindow* window) {
    if (!window) return false;
    QSGRendererInterface* renderer = window->rendererInterface();
    return renderer &&
           renderer->graphicsApi() != QSGRendererInterface::Software;
}

}  // namespace

TraceSceneBuilder::TraceSceneBuilder() = default;

void TraceSceneBuilder::begin(QQuickWindow* window) {
    // A null window is also the benchmark path: geometry is built, text is
    // skipped because textures may only be created with a live scene graph.
    softwareFallback_ = window && !supportsGeometryNodes(window);
    window_ = softwareFallback_ ? nullptr : window;
    vertices_.clear();
    indices_.clear();
    texts_.clear();
}

void TraceSceneBuilder::reserveQuads(int quads) {
    vertices_.reserve(vertices_.size() + quads * 4);
    indices_.reserve(indices_.size() + quads * 6);
}

void TraceSceneBuilder::quad(const QPointF& a, const QPointF& b,
                             const QPointF& c, const QPointF& d,
                             const QColor& color) {
    if (!color.isValid() || color.alpha() == 0) return;
    // Vertex colours are premultiplied: QSGVertexColorMaterial blends with
    // ONE, ONE_MINUS_SRC_ALPHA.
    const float alpha = float(color.alphaF());
    const uchar r = uchar(std::lround(color.redF() * alpha * 255.0));
    const uchar g = uchar(std::lround(color.greenF() * alpha * 255.0));
    const uchar b8 = uchar(std::lround(color.blueF() * alpha * 255.0));
    const uchar a8 = uchar(std::lround(alpha * 255.0));

    const quint32 base = quint32(vertices_.size());
    const QPointF corners[4] = {a, b, c, d};
    for (const QPointF& point : corners) {
        QSGGeometry::ColoredPoint2D vertex{};
        vertex.x = float(point.x());
        vertex.y = float(point.y());
        vertex.r = r;
        vertex.g = g;
        vertex.b = b8;
        vertex.a = a8;
        vertices_.append(vertex);
    }
    for (const quint32 offset : {0u, 1u, 2u, 0u, 2u, 3u})
        indices_.append(base + offset);
}

void TraceSceneBuilder::rect(const QRectF& rect, const QColor& color) {
    if (rect.width() <= 0.0 || rect.height() <= 0.0) return;
    quad(rect.topLeft(), rect.topRight(), rect.bottomRight(), rect.bottomLeft(),
         color);
}

void TraceSceneBuilder::fillQuad(const QPointF& a, const QPointF& b,
                                 const QPointF& c, const QPointF& d,
                                 const QColor& color) {
    quad(a, b, c, d, color);
}

void TraceSceneBuilder::line(const QPointF& from, const QPointF& to,
                             qreal width, const QColor& color) {
    const qreal dx = to.x() - from.x();
    const qreal dy = to.y() - from.y();
    const qreal length = std::hypot(dx, dy);
    if (length < 1.0e-9) {
        rect(QRectF(from.x() - width * 0.5, from.y() - width * 0.5, width,
                    width),
             color);
        return;
    }
    const qreal half = std::max(0.5, width * 0.5);
    const QPointF normal(-dy / length * half, dx / length * half);
    quad(from + normal, to + normal, to - normal, from - normal, color);
}

void TraceSceneBuilder::vLine(qreal x, qreal top, qreal bottom, qreal width,
                              const QColor& color) {
    const qreal half = std::max(0.5, width * 0.5);
    rect(QRectF(x - half, std::min(top, bottom), half * 2.0,
                std::max(1.0, std::fabs(bottom - top))),
         color);
}

void TraceSceneBuilder::hLine(qreal y, qreal left, qreal right, qreal width,
                              const QColor& color) {
    const qreal half = std::max(0.5, width * 0.5);
    rect(QRectF(std::min(left, right), y - half,
                std::max(1.0, std::fabs(right - left)), half * 2.0),
         color);
}

void TraceSceneBuilder::polyline(const QPointF* points, int count, qreal width,
                                 const QColor& color) {
    if (!points || count < 2) return;
    reserveQuads(count);
    const qreal half = std::max(0.5, width * 0.5);
    for (int i = 1; i < count; ++i) {
        const QPointF& from = points[i - 1];
        const QPointF& to = points[i];
        const qreal dx = to.x() - from.x();
        const qreal dy = to.y() - from.y();
        const qreal length = std::hypot(dx, dy);
        if (length < 1.0e-9) continue;
        const QPointF normal(-dy / length * half, dx / length * half);
        quad(from + normal, to + normal, to - normal, from - normal, color);
        // Square joint: without it, direction changes leave a notch.
        if (i + 1 < count)
            rect(QRectF(to.x() - half, to.y() - half, half * 2.0, half * 2.0),
                 color);
    }
}

void TraceSceneBuilder::dot(const QPointF& center, qreal radius,
                            const QColor& color) {
    if (radius <= 0.0) return;
    QPointF previous(center.x() + radius, center.y());
    for (int i = 1; i <= kCircleSegments; ++i) {
        const double angle = 2.0 * M_PI * double(i) / double(kCircleSegments);
        const QPointF point(center.x() + std::cos(angle) * radius,
                            center.y() + std::sin(angle) * radius);
        quad(center, previous, point, point, color);
        previous = point;
    }
}

qreal TraceSceneBuilder::text(const QString& string, const QFont& font,
                              const QColor& color, const QRectF& box,
                              Qt::Alignment alignment) {
    const TraceTextCache::Entry entry =
        cache_.texture(window_, string, font, color);
    if (!entry.valid()) return 0.0;

    qreal x = box.left();
    if (alignment & Qt::AlignRight)
        x = box.right() - entry.size.width();
    else if (alignment & Qt::AlignHCenter)
        x = box.center().x() - entry.size.width() * 0.5;
    qreal y = box.top();
    if (alignment & Qt::AlignVCenter)
        y = box.center().y() - entry.size.height() * 0.5;
    else if (alignment & Qt::AlignBottom)
        y = box.bottom() - entry.size.height();

    texts_.append({entry.texture,
                   QRectF(std::round(x), std::round(y), entry.size.width(),
                          entry.size.height()),
                   0.0});
    return entry.size.width();
}

// Rotation lives in a transform node above the text quad: the texture itself
// stays axis-aligned and shared with the unrotated cache entry.
qreal TraceSceneBuilder::rotatedText(const QString& string, const QFont& font,
                                     const QColor& color, const QPointF& pivot,
                                     Qt::Alignment alignment, qreal degrees) {
    const TraceTextCache::Entry entry =
        cache_.texture(window_, string, font, color);
    if (!entry.valid()) return 0.0;

    qreal x = pivot.x();
    if (alignment & Qt::AlignRight)
        x -= entry.size.width();
    else if (alignment & Qt::AlignHCenter)
        x -= entry.size.width() * 0.5;
    const qreal y = pivot.y() - entry.size.height() * 0.5;
    texts_.append({entry.texture,
                   QRectF(x, y, entry.size.width(), entry.size.height()),
                   degrees});
    return entry.size.width();
}

void TraceSceneBuilder::commit(QSGNode* root) {
    if (!root) return;
    if (softwareFallback_) {
        vertices_.clear();
        indices_.clear();
        texts_.clear();
    }

    // 32-bit indices are load-bearing, not a micro-optimisation. The scene
    // graph's batch renderer merges compatible geometry into 16-bit indexed
    // batches; a zoomed workspace passes 65535 vertices and the lanes past
    // that point silently disappear from the frame. Declaring the index type
    // as UnsignedInt keeps this node out of the merge path and lifts the
    // ceiling. Indexing also stores four vertices per quad instead of six.
    if (vertices_.isEmpty()) {
        if (geometryNode_) {
            root->removeChildNode(geometryNode_);
            delete geometryNode_;
            geometryNode_ = nullptr;
        }
    } else {
        if (!geometryNode_) {
            geometryNode_ = new QSGGeometryNode;
            auto* material = new QSGVertexColorMaterial;
            material->setFlag(QSGMaterial::Blending, true);
            geometryNode_->setMaterial(material);
            geometryNode_->setFlag(QSGNode::OwnsMaterial, true);
            geometryNode_->setFlag(QSGNode::OwnsGeometry, true);
            // Geometry stays behind the text nodes appended later.
            root->prependChildNode(geometryNode_);
        }
        auto* geometry = geometryNode_->geometry();
        if (!geometry || geometry->vertexCount() != vertices_.size() ||
            geometry->indexCount() != indices_.size()) {
            geometry =
                new QSGGeometry(QSGGeometry::defaultAttributes_ColoredPoint2D(),
                                int(vertices_.size()), int(indices_.size()),
                                QSGGeometry::UnsignedIntType);
            geometry->setDrawingMode(QSGGeometry::DrawTriangles);
            geometry->setVertexDataPattern(QSGGeometry::DynamicPattern);
            geometry->setIndexDataPattern(QSGGeometry::DynamicPattern);
            geometryNode_->setGeometry(geometry);
        }
        std::memcpy(
            geometry->vertexData(), vertices_.constData(),
            size_t(vertices_.size()) * sizeof(QSGGeometry::ColoredPoint2D));
        std::memcpy(geometry->indexData(), indices_.constData(),
                    size_t(indices_.size()) * sizeof(quint32));
        geometryNode_->markDirty(QSGNode::DirtyGeometry);
    }

    // Text quads sit above the geometry; the node pool is reused so a moving
    // cursor readout does not allocate scene-graph nodes every frame.
    while (textNodes_.size() < texts_.size()) {
        TextNode node;
        node.transform = new QSGTransformNode;
        node.texture = new QSGSimpleTextureNode;
        node.texture->setOwnsTexture(false);
        node.texture->setFiltering(QSGTexture::Linear);
        node.transform->appendChildNode(node.texture);
        root->appendChildNode(node.transform);
        textNodes_.append(node);
    }
    while (textNodes_.size() > texts_.size()) {
        TextNode node = textNodes_.takeLast();
        root->removeChildNode(node.transform);
        delete node.transform;  // owns the texture node as its child
    }
    for (int i = 0; i < texts_.size(); ++i) {
        const TextQuad& quad = texts_[i];
        TextNode& node = textNodes_[i];
        node.texture->setTexture(quad.texture);
        node.texture->setRect(quad.rect);
        node.texture->markDirty(QSGNode::DirtyMaterial);
        QMatrix4x4 matrix;
        if (!qFuzzyIsNull(quad.rotation)) {
            const QPointF center = quad.rect.center();
            matrix.translate(float(center.x()), float(center.y()));
            matrix.rotate(float(quad.rotation), 0.0f, 0.0f, 1.0f);
            matrix.translate(float(-center.x()), float(-center.y()));
        }
        if (node.transform->matrix() != matrix) {
            node.transform->setMatrix(matrix);
            node.transform->markDirty(QSGNode::DirtyMatrix);
        }
    }
}

void TraceSceneBuilder::releaseResources() {
    cache_.clear();
    vertices_.clear();
    indices_.clear();
    texts_.clear();
    textNodes_.clear();
    geometryNode_ = nullptr;
}
