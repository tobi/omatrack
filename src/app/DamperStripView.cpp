#include "DamperStripView.h"

#include <QSGNode>

#include <algorithm>
#include <cmath>
#include <limits>

#include "TelemetryStore.h"
#include "core/TelemetryEngine.h"

DamperStripView::DamperStripView(QQuickItem* parent) : QQuickItem(parent) {
    setFlag(QQuickItem::ItemHasContents, true);
    connect(this, &DamperStripView::styleChanged, this, &QQuickItem::update);
}

void DamperStripView::setStore(TelemetryStore* store) {
    if (store_ == store) return;
    if (store_) disconnect(store_, nullptr, this, nullptr);
    store_ = store;
    if (store_) {
        for (auto signal :
             {&TelemetryStore::selectionChanged, &TelemetryStore::peekChanged,
              &TelemetryStore::referenceAlignmentChanged,
              &TelemetryStore::cursorFracChanged,
              &TelemetryStore::comparisonSyncStrategyChanged})
            connect(store_, signal, this, &QQuickItem::update);
        // The fraction a pixel stands for depends on the lap length.
        connect(store_, &TelemetryStore::selectionChanged, this,
                &DamperStripView::windowSecondsChanged);
    }
    emit storeChanged();
    emit windowSecondsChanged();
    update();
}

double DamperStripView::lapSeconds() const {
    const omatrack::UnifiedLap* primary =
        store_ ? store_->primaryUnified() : nullptr;
    if (!primary || primary->time.size() < 2) return 0.0;
    return primary->time.back() - primary->time.front();
}

void DamperStripView::setWindowSeconds(double seconds) {
    const double lap = lapSeconds();
    seconds = std::max(kMinimumWindowSeconds,
                       lap > 0.0 ? std::min(seconds, lap) : seconds);
    if (!std::isfinite(seconds) ||
        qFuzzyCompare(windowSeconds_ + 1.0, seconds + 1.0))
        return;
    windowSeconds_ = seconds;
    emit windowSecondsChanged();
    update();
}

double DamperStripView::windowFraction() const {
    const double lap = lapSeconds();
    return lap > 0.0 ? std::min(1.0, windowSeconds_ / lap) : 1.0;
}

void DamperStripView::zoom(double factor) {
    if (std::isfinite(factor) && factor > 0.0)
        setWindowSeconds(windowSeconds_ * factor);
}

QSGNode* DamperStripView::updatePaintNode(QSGNode* oldNode,
                                          UpdatePaintNodeData*) {
    QSGNode* root = oldNode ? oldNode : new QSGNode;
    builder_.begin(window());
    const double w = width();
    const double h = height();
    const DamperAlignment* alignment =
        store_ ? &store_->damperAlignment() : nullptr;
    if (!alignment || !alignment->valid() || w <= 1.0 || h <= 1.0) {
        builder_.commit(root);
        return root;
    }

    const double span = windowFraction();
    const double start = store_->cursorFrac() - 0.5 * span;
    const auto referenceFraction = [this](double primaryFraction) {
        return store_->compareFractionForPrimaryFraction(
            std::clamp(primaryFraction, 0.0, 1.0));
    };

    // Scale each trace to what the window shows, independently: static
    // ride height and fuel load differ between laps, and a shared or
    // whole-lap scale flattens the bumps the eye has to line up.
    const auto range = [](const std::vector<double>& values, double from,
                          double to, double* low, double* span) {
        double lo = std::numeric_limits<double>::infinity();
        double hi = -lo;
        if (values.size() >= 2) {
            const double last = double(values.size() - 1);
            const size_t a = size_t(std::clamp(from, 0.0, 1.0) * last);
            const size_t b = size_t(std::ceil(std::clamp(to, 0.0, 1.0) * last));
            for (size_t i = a; i <= b && i < values.size(); ++i)
                if (std::isfinite(values[i])) {
                    lo = std::min(lo, values[i]);
                    hi = std::max(hi, values[i]);
                }
        }
        if (!(hi > lo)) {
            *low = std::isfinite(lo) ? lo - 0.5 : 0.0;
            *span = 1.0;
            return;
        }
        const double pad = 0.08 * (hi - lo);
        *low = lo - pad;
        *span = hi - lo + 2.0 * pad;
    };
    double primaryLow = 0.0, primarySpan = 1.0;
    double referenceLow = 0.0, referenceSpan = 1.0;
    range(alignment->primary, start, start + span, &primaryLow, &primarySpan);
    range(alignment->compare, referenceFraction(start),
          referenceFraction(start + span), &referenceLow, &referenceSpan);

    const QRectF rect(0.0, 0.0, w, h);
    TraceSceneBuilder::EnvelopeStyle style;
    style.width = 1.4;
    style.fill = false;
    style.color = referenceColor_;
    builder_.envelopePolyline(alignment->compare, referenceFraction, start,
                              span, rect, referenceLow, referenceSpan, style);
    style.color = primaryColor_;
    builder_.envelopePolyline(
        alignment->primary, [](double f) { return f; }, start, span, rect,
        primaryLow, primarySpan, style);
    builder_.vLine(0.5 * w, 0.0, h, 1.0, cursorColor_);

    builder_.commit(root);
    return root;
}

void DamperStripView::releaseResources() { builder_.releaseResources(); }
