#include "TraceTextCache.h"

#include <QFontMetricsF>
#include <QImage>
#include <QPainter>
#include <QQuickWindow>
#include <QSGTexture>

#include <algorithm>
#include <cmath>

TraceTextCache::TraceTextCache(int capacity)
    : capacity_(std::max(16, capacity)) {}

TraceTextCache::~TraceTextCache() { clear(); }

void TraceTextCache::clear() {
    for (const Item& item : std::as_const(items_)) delete item.texture;
    items_.clear();
}

// Drop the least recently used quarter once the cache is full, so a viewport
// full of one-off axis labels cannot evict the labels drawn every frame.
void TraceTextCache::evict() {
    if (items_.size() <= capacity_) return;
    QList<quint64> stamps;
    stamps.reserve(items_.size());
    for (const Item& item : std::as_const(items_)) stamps.append(item.used);
    std::nth_element(stamps.begin(), stamps.begin() + stamps.size() / 4,
                     stamps.end());
    const quint64 threshold = stamps[stamps.size() / 4];
    for (auto it = items_.begin(); it != items_.end();) {
        if (it->used <= threshold) {
            delete it->texture;
            it = items_.erase(it);
        } else {
            ++it;
        }
    }
}

TraceTextCache::Entry TraceTextCache::texture(QQuickWindow* window,
                                              const QString& text,
                                              const QFont& font,
                                              const QColor& color) {
    if (!window || text.isEmpty() || !color.isValid()) return {};

    const qreal ratio = window->effectiveDevicePixelRatio();
    const QString key = text + QLatin1Char('\x1f') + font.key() +
                        QLatin1Char('\x1f') + color.name(QColor::HexArgb) +
                        QLatin1Char('\x1f') + QString::number(ratio, 'f', 2);

    auto existing = items_.find(key);
    if (existing != items_.end()) {
        existing->used = ++tick_;
        return {existing->texture, existing->size};
    }

    const QFontMetricsF metrics(font);
    const QSizeF logical = metrics.size(Qt::TextSingleLine, text) +
                           QSizeF(2.0, 2.0);  // room for antialiased edges
    const QSize pixels(int(std::ceil(logical.width() * ratio)),
                       int(std::ceil(logical.height() * ratio)));
    if (pixels.isEmpty()) return {};

    QImage image(pixels, QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(ratio);
    image.fill(Qt::transparent);
    {
        QPainter painter(&image);
        painter.setRenderHint(QPainter::TextAntialiasing, true);
        painter.setFont(font);
        painter.setPen(color);
        painter.drawText(QRectF(QPointF(1.0, 0.0), logical),
                         Qt::AlignLeft | Qt::AlignVCenter, text);
    }

    Item item;
    item.texture = window->createTextureFromImage(
        image, QQuickWindow::TextureHasAlphaChannel);
    if (!item.texture) return {};
    item.texture->setFiltering(QSGTexture::Linear);
    item.size = logical;
    item.used = ++tick_;
    items_.insert(key, item);
    evict();
    return {item.texture, item.size};
}
