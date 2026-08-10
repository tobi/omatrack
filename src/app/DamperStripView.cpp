#include "DamperStripView.h"

#include <QSGNode>

#include <algorithm>
#include <cmath>

#include "TelemetryStore.h"

DamperStripView::DamperStripView(QQuickItem* parent) : QQuickItem(parent) {
    setFlag(QQuickItem::ItemHasContents, true);
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

void DamperStripView::setSeries(Series series) {
    if (series_ == series) return;
    series_ = series;
    emit seriesChanged();
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

QSGNode* DamperStripView::updatePaintNode(QSGNode* oldNode,
                                          UpdatePaintNodeData*) {
    QSGNode* root = oldNode ? oldNode : new QSGNode;
    builder_.begin(window());

    if (!store_ || width() <= 0.0 || height() <= 0.0) {
        builder_.commit(root);
        return root;
    }

    const DamperAlignment& alignment = store_->damperAlignment();
    if (!alignment.valid()) {
        builder_.commit(root);
        return root;
    }

    const std::vector<double>* values =
        series_ == Primary ? &alignment.primary : &alignment.compare;
    const double low = alignment.minimum;
    const double span = alignment.span();
    if (values->size() < 2) {
        builder_.commit(root);
        return root;
    }

    const double w = width();
    const double h = height();
    if (w <= 1.0 || h <= 1.0) {
        builder_.commit(root);
        return root;
    }

    // One point per pixel column: a lap of damper travel is tens of thousands
    // of samples and the strip is a few hundred pixels wide, so submitting
    // every sample would cost the same image for far more work.
    const int columns = std::max(2, int(w));
    const double last = double(values->size() - 1);
    const double offset = shift_ * w;

    points_.clear();
    points_.resize(columns);
    for (int column = 0; column < columns; ++column) {
        const double position = double(column) / double(columns - 1) * last;
        const size_t index = size_t(position);
        const size_t next = std::min(index + 1, values->size() - 1);
        const double fraction = position - double(index);
        const double value =
            (*values)[index] + ((*values)[next] - (*values)[index]) * fraction;
        const double x = double(column) / double(columns - 1) * w + offset;
        const double y = h - (value - low) / span * h;
        points_[column] = QPointF(x, y);
    }

    // The builder has no separate opacity concept, so fold strokeOpacity into
    // the colour alpha.
    QColor stroke = color_;
    stroke.setAlphaF(color_.alphaF() * strokeOpacity_);

    builder_.reserveQuads(points_.size());
    builder_.polyline(points_.constData(), points_.size(), 1.3, stroke);

    builder_.commit(root);
    return root;
}

void DamperStripView::releaseResources() { builder_.releaseResources(); }
