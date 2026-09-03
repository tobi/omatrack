#include "TraceSceneBuilder.h"

#include <QQuickWindow>
#include <QSGGeometryNode>
#include <QSGSimpleTextureNode>
#include <QSGTransformNode>
#include <QSGVertexColorMaterial>

#include <cmath>
#include <cstring>
#include <limits>

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

qreal TraceSceneBuilder::devicePixelRatio() const {
    return window_ ? std::max(1.0, window_->effectiveDevicePixelRatio()) : 1.0;
}

int TraceSceneBuilder::deviceColumns(const QRectF& rect) const {
    return std::max(2, int(std::lround(rect.width() * devicePixelRatio())));
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

void TraceSceneBuilder::dashedHLine(qreal y, qreal left, qreal right,
                                    const QColor& color, qreal dash,
                                    qreal gap) {
    for (qreal x = left; x < right; x += dash + gap)
        hLine(y, x, std::min(x + dash, right), 1.0, color);
}

void TraceSceneBuilder::dashedVLine(qreal x, qreal top, qreal bottom,
                                    const QColor& color, qreal width,
                                    qreal dash, qreal gap) {
    for (qreal y = top; y < bottom; y += dash + gap)
        vLine(x, y, std::min(y + dash, bottom), width, color);
}

void TraceSceneBuilder::outline(const QRectF& rect, const QColor& color) {
    hLine(rect.top(), rect.left(), rect.right(), 1.0, color);
    hLine(rect.bottom(), rect.left(), rect.right(), 1.0, color);
    vLine(rect.left(), rect.top(), rect.bottom(), 1.0, color);
    vLine(rect.right(), rect.top(), rect.bottom(), 1.0, color);
}

void TraceSceneBuilder::envelopePolyline(
    const std::vector<double>& series,
    const std::function<double(double)>& sourceFraction, double xStart,
    double xSpan, const QRectF& rect, double yMin, double ySpan,
    const EnvelopeStyle& style, double clipLow, double clipHigh) {
    if (series.size() < 2 || rect.width() < 2.0 || xSpan <= 0.0 || ySpan <= 0.0)
        return;

    const int valueLast = int(series.size()) - 1;
    const int columns = deviceColumns(rect);
    const auto toY = [&](double value) {
        return rect.top() + (1.0 - (value - yMin) / ySpan) * rect.height();
    };

    const double series0 = sourceFraction(xStart);
    const double series1 = sourceFraction(xStart + xSpan);
    const double seriesSpan = series1 - series0;

    const bool warped = std::fabs(seriesSpan) > 1.0e-12;
    // One tight stroke for every zoom level: each device column contributes
    // the exact min/max of the samples it covers, so extremes survive
    // decimation without ever rendering a filled band or a stretched raster.
    // A column holding a single sample plots that sample's exact position,
    // so a zoomed slope is a line through the data. Gaps (no finite sample,
    // clipped column) break the stroke; nothing is invented across them.
    pointsScratch_.clear();
    auto flush = [&]() {
        if (pointsScratch_.size() >= 2)
            polyline(pointsScratch_.constData(), int(pointsScratch_.size()),
                     style.width, style.color);
        else if (pointsScratch_.size() == 1)
            dot(pointsScratch_.constData()[0], style.width * 0.5, style.color);
        pointsScratch_.clear();
    };
    reserveQuads(columns);

    // Fill chains along the top edge of the stroke, column to column, exactly
    // like the stroke itself: no band, no stretching, just area under the
    // tight line for channels that ask for it.
    bool fillOpen = false;
    double fillX = 0.0;
    double fillTop = 0.0;
    for (int column = 0; column < columns; ++column) {
        const double startFraction =
            xStart + xSpan * double(column) / double(columns);
        const double endFraction =
            xStart + xSpan * double(column + 1) / double(columns);
        const double centre = (startFraction + endFraction) * 0.5;
        if (centre < clipLow || centre > clipHigh) {
            flush();
            fillOpen = false;
            continue;
        }
        double from = sourceFraction(startFraction) * valueLast;
        double to = sourceFraction(endFraction) * valueLast;
        if (to < from) std::swap(from, to);

        double low = std::numeric_limits<double>::infinity();
        double high = -std::numeric_limits<double>::infinity();
        int finite = 0;
        double singleFrac = 0.0;
        const int firstIndex = std::clamp(int(std::floor(from)), 0, valueLast);
        const int lastIndex = std::clamp(int(std::ceil(to)), 0, valueLast);
        if (lastIndex - firstIndex >= 2) {
            for (int i = firstIndex; i <= lastIndex; ++i) {
                const double value = series[size_t(i)];
                if (!std::isfinite(value)) continue;
                if (finite == 0) singleFrac = double(i) / double(valueLast);
                ++finite;
                low = std::min(low, value);
                high = std::max(high, value);
            }
        } else {
            const int lower = firstIndex;
            const int upper = std::min(lower + 1, valueLast);
            const double value =
                series[size_t(lower)] +
                (series[size_t(upper)] - series[size_t(lower)]) *
                    (from - double(lower));
            if (std::isfinite(value)) {
                finite = 1;
                singleFrac = from / double(valueLast);
                low = high = value;
            }
        }
        if (finite == 0) {
            flush();
            fillOpen = false;
            continue;
        }

        double x = rect.left() +
                   rect.width() * (double(column) + 0.5) / double(columns);
        if (finite == 1) {
            // One sample in this column: plot its exact position instead of
            // the column centre. x is the linear inverse of sourceFraction
            // between the viewport endpoints — exact for an unwarped lap,
            // and locally close once GPS alignment is linear.
            const double t = warped ? (singleFrac - series0) / seriesSpan
                                    : (centre - xStart) / xSpan;
            if (t < -0.02 || t > 1.02) {
                flush();
                fillOpen = false;
                continue;
            }
            x = rect.left() + t * rect.width();
        }
        const double top = std::clamp(toY(high), rect.top(), rect.bottom());
        const double bottom = std::clamp(toY(low), rect.top(), rect.bottom());

        if (style.fill) {
            if (fillOpen)
                fillQuad(QPointF(fillX, fillTop), QPointF(x, top),
                         QPointF(x, rect.bottom()),
                         QPointF(fillX, rect.bottom()), style.fillColor);
            fillX = x;
            fillTop = top;
            fillOpen = true;
        }
        pointsScratch_.append(QPointF(x, top));
        if (bottom - top > 1.0e-9) pointsScratch_.append(QPointF(x, bottom));
    }
    flush();
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
            vertexCapacity_ = 0;
            indexCapacity_ = 0;
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
        // Capacity buckets: round the vertex/index counts up to the next
        // 4096-vertex / 16384-index bucket and only reallocate the geometry
        // buffer when the frame grows past it. The bucket is never shrunk per
        // frame, so a cursor moving across a fixed scene reuses the same
        // allocation. The renderer draws `used` vertices, so the unused tail
        // is filled with degenerate (zero-index) triangles.
        const int usedVertices = int(vertices_.size());
        const int usedIndices = int(indices_.size());
        auto bucket = [](int count, int step) {
            return std::max(step, ((count + step - 1) / step) * step);
        };
        const int vertexBucket = bucket(usedVertices, 4096);
        const int indexBucket = bucket(usedIndices, 16384);
        auto* geometry = geometryNode_->geometry();
        if (!geometry || vertexBucket > vertexCapacity_ ||
            indexBucket > indexCapacity_) {
            geometry = new QSGGeometry(
                QSGGeometry::defaultAttributes_ColoredPoint2D(), vertexBucket,
                indexBucket, QSGGeometry::UnsignedIntType);
            geometry->setDrawingMode(QSGGeometry::DrawTriangles);
            geometry->setVertexDataPattern(QSGGeometry::DynamicPattern);
            geometry->setIndexDataPattern(QSGGeometry::DynamicPattern);
            geometryNode_->setGeometry(geometry);
            vertexCapacity_ = vertexBucket;
            indexCapacity_ = indexBucket;
        }
        std::memcpy(geometry->vertexData(), vertices_.constData(),
                    size_t(usedVertices) * sizeof(QSGGeometry::ColoredPoint2D));
        // Pad the unused index tail with degenerate triangles so the GPU
        // never reads past the frame's data: every extra index references
        // vertex 0, producing zero-area triangles.
        std::memcpy(geometry->indexData(), indices_.constData(),
                    size_t(usedIndices) * sizeof(quint32));
        if (usedIndices < indexBucket)
            std::memset(
                static_cast<quint32*>(geometry->indexData()) + usedIndices, 0,
                size_t(indexBucket - usedIndices) * sizeof(quint32));
        geometry->setIndexCount(usedIndices);
        geometry->setVertexCount(usedVertices);
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
        // Only mark the texture node dirty when the texture or rect actually
        // changed. Lane labels, units and axis ticks are immutable across
        // frames, so a cursor move no longer re-uploads them.
        if (node.initialised && node.lastTexture == quad.texture &&
            node.lastRect == quad.rect) {
            // Texture unchanged; still may need a transform update below.
        } else {
            node.texture->setTexture(quad.texture);
            node.texture->setRect(quad.rect);
            node.texture->markDirty(QSGNode::DirtyMaterial);
            node.lastTexture = quad.texture;
            node.lastRect = quad.rect;
            node.initialised = true;
        }
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
    vertexCapacity_ = 0;
    indexCapacity_ = 0;
}
