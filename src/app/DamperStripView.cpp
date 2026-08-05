#include "DamperStripView.h"

#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>
#include <limits>

#include "TelemetryStore.h"

DamperStripView::DamperStripView(QQuickItem* parent)
    : QQuickPaintedItem(parent) {
    setAntialiasing(true);
}

void DamperStripView::setStore(TelemetryStore* store) {
    if (store_ == store) return;
    if (store_) disconnect(store_, nullptr, this, nullptr);
    store_ = store;
    if (store_) {
        connect(store_, &TelemetryStore::selectionChanged, this,
                &QQuickItem::update);
        connect(store_, &TelemetryStore::referenceAlignmentChanged, this,
                &QQuickItem::update);
        connect(store_, &TelemetryStore::cornersChanged, this,
                &QQuickItem::update);
    }
    emit storeChanged();
    update();
}

void DamperStripView::setSource(Source source) {
    if (source_ == source) return;
    source_ = source;
    emit sourceChanged();
    update();
}

void DamperStripView::setSeries(Series series) {
    if (series_ == series) return;
    series_ = series;
    emit seriesChanged();
    update();
}

void DamperStripView::setCornerIndex(int index) {
    if (cornerIndex_ == index) return;
    cornerIndex_ = index;
    emit cornerIndexChanged();
    update();
}

void DamperStripView::setColor(const QColor& color) {
    if (color_ == color) return;
    color_ = color;
    emit colorChanged();
    update();
}

void DamperStripView::setShift(qreal shift) {
    if (qFuzzyCompare(shift_ + 1.0, shift + 1.0)) return;
    shift_ = shift;
    emit shiftChanged();
    update();
}

void DamperStripView::setStrokeOpacity(qreal opacity) {
    if (qFuzzyCompare(strokeOpacity_ + 1.0, opacity + 1.0)) return;
    strokeOpacity_ = opacity;
    emit strokeOpacityChanged();
    update();
}

void DamperStripView::paint(QPainter* painter) {
    if (!store_) return;
    const std::vector<double>* values = nullptr;
    double low = 0.0;
    double span = 1.0;
    double offsetFraction = 0.0;

    if (source_ == LapAlignment) {
        const DamperAlignment& alignment = store_->damperAlignment();
        if (!alignment.valid()) return;
        values = series_ == Primary ? &alignment.primary : &alignment.compare;
        low = alignment.minimum;
        span = alignment.span();
        offsetFraction = shift_;
    } else {
        const CornerGraph* graph = store_->cornerGraph(cornerIndex_);
        if (!graph || !graph->damper.valid()) return;
        const CornerDamperWindow& damper = graph->damper;
        values = series_ == Primary ? &damper.primary : &damper.compare;
        // Both traces share one scale so a shift reads as a shift.
        low = std::numeric_limits<double>::infinity();
        double high = -std::numeric_limits<double>::infinity();
        for (const std::vector<double>* set : {&damper.primary, &damper.compare})
            for (double value : *set) {
                low = std::min(low, value);
                high = std::max(high, value);
            }
        if (!std::isfinite(low) || !std::isfinite(high) || high <= low) {
            low = 0.0;
            high = 1.0;
        }
        span = std::max(1e-6, high - low);
        offsetFraction = shift_ / std::max(1.0, damper.windowMeters);
    }
    if (!values || values->size() < 2) return;

    const double w = width();
    const double h = height();
    if (w <= 1.0 || h <= 1.0) return;

    // One point per pixel column: a lap of damper travel is tens of thousands
    // of samples and the strip is a few hundred pixels wide, so submitting
    // every sample would cost the same image for far more work.
    const int columns = std::max(2, int(w));
    const double last = double(values->size() - 1);
    const double offset = offsetFraction * w;

    QPainterPath path;
    for (int column = 0; column < columns; ++column) {
        const double position = double(column) / double(columns - 1) * last;
        const size_t index = size_t(position);
        const size_t next = std::min(index + 1, values->size() - 1);
        const double fraction = position - double(index);
        const double value =
            (*values)[index] + ((*values)[next] - (*values)[index]) * fraction;
        const double x = double(column) / double(columns - 1) * w + offset;
        const double y = h - (value - low) / span * h;
        if (column == 0)
            path.moveTo(x, y);
        else
            path.lineTo(x, y);
    }

    painter->setOpacity(strokeOpacity_);
    painter->setBrush(Qt::NoBrush);
    painter->setPen(QPen(color_, 1.3));
    painter->drawPath(path);
    painter->setOpacity(1.0);
}
