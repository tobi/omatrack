#include "DamperStripView.h"

#include <QSGNode>

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

    // The shift is a pixel offset: the whole strip slides right by shift_*w.
    // envelopePolyline maps viewport fraction → series fraction (identity
    // here, since the full lap is always shown) and handles both the
    // zoomed-out min/max envelope and the zoomed-in polyline, matching what
    // TraceView does for channel traces.
    const double offset = shift_ * w;

    // The builder has no separate opacity concept, so fold strokeOpacity into
    // the colour alpha.
    QColor stroke = color_;
    stroke.setAlphaF(color_.alphaF() * strokeOpacity_);

    TraceSceneBuilder::EnvelopeStyle style;
    style.width = 1.3;
    style.fill = false;
    style.color = stroke;
    builder_.envelopePolyline(
        *values, [](double f) { return f; }, 0.0, 1.0,
        QRectF(offset, 0.0, w, h), low, span, style);

    builder_.commit(root);
    return root;
}

void DamperStripView::releaseResources() { builder_.releaseResources(); }
