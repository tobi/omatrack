#include "TraceInteraction.h"

#include "TelemetryStore.h"
#include "TraceLaneLayout.h"
#include "TraceLaneSizing.h"
#include "TraceSnapshot.h"
#include "core/TelemetryEngine.h"

#include <QMouseEvent>
#include <QHoverEvent>
#include <QWheelEvent>
#include <QKeyEvent>

#include <algorithm>
#include <cmath>

namespace {

constexpr double kTopPad = 22.0;
constexpr double kMarkerBand = 26.0;
constexpr int kCursorSamplesPerStep = 1;
constexpr qint64 kHoverFrameMs = 16;

}  // namespace

double TraceInteraction::xForFrac(double frac) const {
    return layout_->labelWidth() + (frac - store_->viewStart()) /
                                       store_->viewSpan() *
                                       (itemWidth_ - layout_->labelWidth());
}

double TraceInteraction::fracForX(double x) const {
    double w = std::max(1.0, itemWidth_ - layout_->labelWidth());
    return store_->viewStart() +
           std::clamp((x - layout_->labelWidth()) / w, 0.0, 1.0) *
               store_->viewSpan();
}

int TraceInteraction::cornerIndexAt(const QPointF& position) const {
    if (!store_ || store_->corners().isEmpty() || position.y() > kTopPad)
        return -1;
    const double fraction = fracForX(position.x());
    for (int i = 0; i < store_->corners().size(); ++i) {
        const CornerZone& corner = store_->corners()[i];
        if (corner.start <= fraction && fraction <= corner.end) return i;
    }
    return -1;
}

int TraceInteraction::channelIndexAt(const QPointF& position) const {
    for (const auto& lane : layout_->layoutLanes())
        if (position.y() >= lane.y && position.y() < lane.y + lane.height)
            return lane.spec;
    return -1;
}

int TraceInteraction::markerIndexAt(const QPointF& position) const {
    if (!store_ || store_->focusedCorner() < 0) return -1;
    if (position.y() < cursorBottom_ - kMarkerBand ||
        position.y() > cursorBottom_)
        return -1;
    const QVector<CornerMarker>& markers = store_->cornerMarkers();
    int best = -1;
    double bestDistance = 9.0;
    for (int i = 0; i < markers.size(); ++i) {
        const double distance =
            std::fabs(position.x() - xForFrac(markers[i].fraction));
        if (distance < bestDistance) {
            bestDistance = distance;
            best = i;
        }
    }
    return best;
}

int TraceInteraction::focusedMarkerIndexValue() const {
    if (!store_ || store_->focusedCorner() < 0) return -1;
    if (hoveredMarker_ >= 0) return hoveredMarker_;
    const QString key = store_->highlightedCornerMarker();
    if (key.isEmpty()) return -1;
    const QVector<CornerMarker>& markers = store_->cornerMarkers();
    for (int i = 0; i < markers.size(); ++i)
        if (markers[i].key == key) return i;
    return -1;
}

int TraceInteraction::focusedZoneHandleAt(const QPointF& position) const {
    if (!store_ || store_->focusedCorner() < 0) return 0;
    const int focused = store_->focusedCorner();
    if (focused >= store_->corners().size()) return 0;
    const CornerZone& corner = store_->corners()[focused];
    const double x = position.x();
    const double x1 = xForFrac(corner.start);
    const double x2 = xForFrac(corner.end);
    constexpr double kEdge = 8.0;
    if (std::fabs(x - x1) <= kEdge) return 1;
    if (std::fabs(x - x2) <= kEdge) return 2;
    if (position.y() <= kTopPad && x >= x1 && x <= x2) return 3;
    return 0;
}

void TraceInteraction::updateZoneHoverCursor(const QPointF& position) {
    if (dragging_ || panning_ || selecting_) return;
    if (!store_ || !store_->editingCorners()) {
        if (onUnsetCursor) onUnsetCursor();
        return;
    }
    switch (focusedZoneHandleAt(position)) {
        case 1:
        case 2:
            if (onSetCursor) onSetCursor(Qt::SizeHorCursor);
            return;
        case 3:
            if (onSetCursor) onSetCursor(Qt::OpenHandCursor);
            return;
        default: break;
    }
    if (onUnsetCursor) onUnsetCursor();
}

void TraceInteraction::updateHoveredMarker(const QPointF& position) {
    const int marker = markerIndexAt(position);
    if (marker == hoveredMarker_) return;
    hoveredMarker_ = marker;
    if (onOverlayChanged) onOverlayChanged();
}

void TraceInteraction::updateHoveredCorner(const QPointF& position) {
    const int index = cornerIndexAt(position);
    if (index == hoveredCorner_) return;
    hoveredCorner_ = index;
    if (onOverlayChanged) onOverlayChanged();
}

void TraceInteraction::updateHoveredSpan(const QPointF& position) {
    spanHoverX_ = position.x();
    spanHoverY_ = position.y();
    int index = -1;
    for (int i = 0; i < spanHits_.size(); ++i) {
        if (spanHits_.at(i).rect.contains(position)) {
            index = i;
            break;
        }
    }
    if (index == hoveredSpan_) {
        if (spanHoverVisible_ && onSpanHoverChanged) onSpanHoverChanged();
        return;
    }
    hoveredSpan_ = index;
    if (index < 0 || index >= spanHits_.size()) {
        spanHoverVisible_ = false;
        spanHoverTitle_.clear();
        spanHoverSubtitle_.clear();
        spanHoverMeta_.clear();
    } else {
        const SpanHit& hit = spanHits_.at(index);
        spanHoverVisible_ = true;
        spanHoverTitle_ = hit.title;
        spanHoverSubtitle_ = hit.subtitle;
        spanHoverColor_ = hit.color;
        spanHoverMeta_ = hit.meta;
    }
    if (onSpanHoverChanged) onSpanHoverChanged();
}

void TraceInteraction::showChannelMenu(const QPointF& position) {
    const int index = channelIndexAt(position);
    const auto& specs = layout_->channelSpecs();
    if (index < 0 || index >= specs.size()) return;
    const auto& spec = specs[index];
    if (onChannelMenuRequested)
        onChannelMenuRequested(spec.key, spec.title,
                               store_->channelWeight(spec.key), position.x(),
                               position.y());
}

int TraceInteraction::groupHeaderAt(const QPointF& position) const {
    const auto& specs = layout_->channelSpecs();
    for (const auto& lane : layout_->layoutLanes()) {
        if (position.y() < lane.y || position.y() >= lane.y + lane.height)
            continue;
        if (lane.spec < 0 || lane.spec >= specs.size()) return -1;
        if (specs[lane.spec].kind ==
            TraceLaneLayout::ChannelSpec::Kind::GroupHeader)
            return lane.spec;
        return -1;
    }
    return -1;
}

void TraceInteraction::cancelLaneResize() {
    resizingBoundary_ = hoveredResizeBoundary_ = -1;
    resizeKeys_.clear();
    resizeHeights_.clear();
    if (onUnsetCursor) onUnsetCursor();
    if (onOverlayChanged) onOverlayChanged();
}

void TraceInteraction::validateLaneResize() {
    if (resizingBoundary_ < 0) return;
    const auto& specs = layout_->channelSpecs();
    qsizetype index = 0;
    for (const auto& lane : layout_->layoutLanes()) {
        if (specs[lane.spec].kind != TraceLaneLayout::ChannelSpec::Kind::Sample)
            continue;
        if (index >= resizeKeys_.size() ||
            resizeKeys_[index++] != specs[lane.spec].key) {
            cancelLaneResize();
            return;
        }
    }
    if (index != resizeKeys_.size()) cancelLaneResize();
}

int TraceInteraction::resizeBoundaryAt(const QPointF& position) const {
    if (!store_ || !store_->resizingTraces() ||
        position.x() < layout_->labelWidth())
        return -1;
    const auto lanes = layout_->layoutLanes();
    const auto& specs = layout_->channelSpecs();
    int count = 0;
    for (const auto& lane : lanes)
        count +=
            specs[lane.spec].kind == TraceLaneLayout::ChannelSpec::Kind::Sample;
    int index = 0, best = -1;
    double distance = 6.0;
    for (const auto& lane : lanes) {
        if (specs[lane.spec].kind != TraceLaneLayout::ChannelSpec::Kind::Sample)
            continue;
        const double d = std::abs(position.y() - lane.y - lane.height);
        if (index < count - 1 && d < distance) {
            distance = d;
            best = index;
        }
        ++index;
    }
    return best;
}

void TraceInteraction::mouseDoubleClickEvent(QMouseEvent* event) {
    if (!store_ || !store_->primaryUnified()) return;
    if (store_->resizingTraces()) {
        event->accept();
        return;
    }
    panning_ = false;
    selecting_ = false;
    selectionStart_ = -1.0;
    selectionEnd_ = -1.0;
    store_->resetView();
    if (onUnsetCursor) onUnsetCursor();
    if (onOverlayChanged) onOverlayChanged();
    event->accept();
}

void TraceInteraction::mousePressEvent(QMouseEvent* event) {
    if (!store_ || !store_->primaryUnified()) return;
    if (store_->resizingTraces()) {
        resizingBoundary_ = event->button() == Qt::LeftButton
                                ? resizeBoundaryAt(event->position())
                                : -1;
        if (resizingBoundary_ >= 0) {
            resizeOriginY_ = event->position().y();
            resizeKeys_.clear();
            resizeHeights_.clear();
            const auto& specs = layout_->channelSpecs();
            for (const auto& lane : layout_->layoutLanes()) {
                if (specs[lane.spec].kind !=
                    TraceLaneLayout::ChannelSpec::Kind::Sample)
                    continue;
                resizeKeys_.append(specs[lane.spec].key);
                resizeHeights_.push_back(lane.height);
            }
            if (onSetCursor) onSetCursor(Qt::SizeVerCursor);
            if (onOverlayChanged) onOverlayChanged();
        }
        event->accept();
        return;
    }
    const double x = event->position().x();
    const double fraction = fracForX(x);
    if (event->button() == Qt::RightButton) {
        if (event->position().y() >= kTopPad)
            showChannelMenu(event->position());
        event->accept();
        return;
    }

    if (event->button() == Qt::MiddleButton) {
        panning_ = true;
        lastPanFrac_ = fraction;
        if (onSetCursor) onSetCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }

    if (event->button() != Qt::LeftButton) return;

    const int header = groupHeaderAt(event->position());
    const auto& specs = layout_->channelSpecs();
    if (header >= 0 && header < specs.size()) {
        const QString groupId = specs[header].groupId;
        store_->setOverlayExpanded(groupId, !store_->overlayExpanded(groupId));
        event->accept();
        return;
    }

    const int focusHandle =
        store_->editingCorners() ? focusedZoneHandleAt(event->position()) : 0;
    if (focusHandle > 0) {
        dragCorner_ = store_->focusedCorner();
        dragCornerMove_ = focusHandle == 3;
        dragStartFrac_ = dragCornerMove_
                             ? fraction - store_->corners()[dragCorner_].start
                             : 0.0;
        dragging_ = true;
        if (onSetCursor)
            onSetCursor(dragCornerMove_ ? Qt::ClosedHandCursor
                                        : Qt::SizeHorCursor);
        event->accept();
        return;
    }

    const int cornerIndex = cornerIndexAt(event->position());
    if (cornerIndex >= 0 && !store_->editingCorners()) {
        store_->focusCorner(cornerIndex);
        event->accept();
        return;
    }

    if (store_->editingCorners() && !store_->corners().isEmpty()) {
        for (int i = 0; i < store_->corners().size(); ++i) {
            const CornerZone& corner = store_->corners()[i];
            const double x1 = xForFrac(corner.start);
            const double x2 = xForFrac(corner.end);
            if (std::fabs(x - x1) < 5 || std::fabs(x - x2) < 5) {
                dragCorner_ = i;
                dragCornerMove_ = false;
                dragging_ = true;
                break;
            }
            if (corner.start <= fraction && fraction <= corner.end) {
                dragCorner_ = i;
                dragCornerMove_ = true;
                dragStartFrac_ = fraction - corner.start;
                dragging_ = true;
                break;
            }
        }
        if (dragging_) {
            if (onSetCursor) onSetCursor(Qt::SizeHorCursor);
            event->accept();
            return;
        }
    }

    pressX_ = x;
    selecting_ = true;
    selectionStart_ = fraction;
    selectionEnd_ = fraction;
    store_->setCursorFrac(fraction);
    if (onCursorChangedFromCanvas) onCursorChangedFromCanvas();
    if (onSetCursor) onSetCursor(Qt::CrossCursor);
    if (onOverlayChanged) onOverlayChanged();
    event->accept();
}

void TraceInteraction::mouseMoveEvent(QMouseEvent* event) {
    if (!store_) return;
    if (store_->resizingTraces()) {
        if (resizingBoundary_ >= 0)
            store_->previewTraceHeights(
                resizeKeys_, trace::resizeLaneBoundary(
                                 resizeHeights_, size_t(resizingBoundary_),
                                 event->position().y() - resizeOriginY_));
        event->accept();
        return;
    }
    const double x = event->position().x();
    if (dragging_ && dragCorner_ >= 0) {
        const double fraction = fracForX(x);
        const auto& corners = store_->corners();
        if (dragCorner_ >= corners.size()) {
            dragging_ = false;
            return;
        }
        const double start = corners[dragCorner_].start;
        const double end = corners[dragCorner_].end;
        if (!dragCornerMove_) {
            const double dx1 = std::fabs(x - xForFrac(start));
            const double dx2 = std::fabs(x - xForFrac(end));
            if (dx1 < dx2)
                store_->updateCorner(dragCorner_, qBound(0.0, fraction, end),
                                     end);
            else
                store_->updateCorner(dragCorner_, start,
                                     qBound(start, fraction, 1.0));
        } else {
            const double width = end - start;
            const double nextStart =
                qBound(0.0, fraction - dragStartFrac_, 1.0 - width);
            store_->updateCorner(dragCorner_, nextStart, nextStart + width);
        }
        if (onCornerEdited) onCornerEdited();
        return;
    }
    if (selecting_) {
        selectionEnd_ = fracForX(x);
        store_->setCursorFrac(selectionEnd_);
        if (onCursorChangedFromCanvas) onCursorChangedFromCanvas();
        if (onOverlayChanged) onOverlayChanged();
        return;
    }
    if (panning_) {
        const double fraction = fracForX(x);
        store_->pan(lastPanFrac_ - fraction);
        lastPanFrac_ = fraction;
        return;
    }
    updateHoveredMarker(event->position());
}

void TraceInteraction::mouseReleaseEvent(QMouseEvent* event) {
    if (resizingBoundary_ >= 0) {
        cancelLaneResize();
        event->accept();
        return;
    }
    dragging_ = false;
    panning_ = false;
    dragCorner_ = -1;
    if (selecting_) {
        selecting_ = false;
        if (std::fabs(event->position().x() - pressX_) < 3.0) {
            selectionStart_ = -1.0;
            selectionEnd_ = -1.0;
        }
    }
    if (onUnsetCursor) onUnsetCursor();
    if (onOverlayChanged) onOverlayChanged();
}

void TraceInteraction::wheelEvent(QWheelEvent* event) {
    if (!store_ || !store_->primaryUnified()) return;
    if (store_->resizingTraces()) {
        event->accept();
        return;
    }
    const QPoint pixel = event->pixelDelta();
    const QPoint angle = event->angleDelta();
    const double dx = pixel.x() != 0 ? double(pixel.x()) : double(angle.x());
    const double dy = pixel.y() != 0 ? double(pixel.y()) : double(angle.y());
    if (dx == 0.0 && dy == 0.0) return;
    if (std::abs(dx) > std::abs(dy)) {
        const double dataWidth =
            std::max(1.0, itemWidth_ - layout_->labelWidth());
        store_->pan(-dx / dataWidth * store_->viewSpan());
        event->accept();
        return;
    }
    const double anchor = fracForX(event->position().x());
    store_->zoomAt(anchor, std::pow(0.8, dy / 120.0));
    event->accept();
}

bool TraceInteraction::keyPressEvent(QKeyEvent* event) {
    if (!store_) return false;
    if (store_->resizingTraces()) {
        if (event->key() == Qt::Key_Escape) store_->cancelTraceResize();
        event->accept();
        return true;
    }
    int steps = 0;
    switch (event->key()) {
        case Qt::Key_Left: steps = -kCursorSamplesPerStep; break;
        case Qt::Key_Right: steps = kCursorSamplesPerStep; break;
        case Qt::Key_Escape:
            event->accept();
            if (store_->editingCorners())
                store_->cancelCornerEdit();
            else
                store_->clearCornerFocus();
            return true;
        case Qt::Key_Home:
            event->accept();
            store_->jumpToFraction(0.0);
            return true;
        case Qt::Key_End:
            event->accept();
            store_->jumpToFraction(1.0);
            return true;
        default: return false;
    }
    store_->moveCursorSteps(steps);
    if (onCursorChangedFromCanvas) onCursorChangedFromCanvas();
    event->accept();
    return true;
}

void TraceInteraction::hoverMoveEvent(QHoverEvent* event) {
    if (!store_ || !store_->primaryUnified()) return;
    if (store_->resizingTraces()) {
        const int boundary = resizeBoundaryAt(event->position());
        if (boundary != hoveredResizeBoundary_) {
            hoveredResizeBoundary_ = boundary;
            if (onOverlayChanged) onOverlayChanged();
        }
        if (boundary >= 0) {
            if (onSetCursor) onSetCursor(Qt::SizeVerCursor);
        } else if (onUnsetCursor)
            onUnsetCursor();
        event->accept();
        return;
    }
    updateZoneHoverCursor(event->position());
    updateHoveredCorner(event->position());
    updateHoveredSpan(event->position());
    if (cursorTimer_.isValid() && cursorTimer_.elapsed() < kHoverFrameMs) {
        event->accept();
        return;
    }
    cursorTimer_.restart();
    updateHoveredMarker(event->position());
    if (selecting_) {
        selectionEnd_ = fracForX(event->position().x());
        if (onOverlayChanged) onOverlayChanged();
    }
    event->accept();
}

void TraceInteraction::hoverLeaveEvent(QHoverEvent* event) {
    if (hoveredResizeBoundary_ >= 0) {
        hoveredResizeBoundary_ = -1;
        if (onOverlayChanged) onOverlayChanged();
    }
    if (hoveredMarker_ >= 0 || hoveredCorner_ >= 0 || hoveredSpan_ >= 0 ||
        spanHoverVisible_) {
        hoveredMarker_ = -1;
        hoveredCorner_ = -1;
        hoveredSpan_ = -1;
        spanHoverVisible_ = false;
        spanHoverTitle_.clear();
        spanHoverSubtitle_.clear();
        spanHoverMeta_.clear();
        if (onOverlayChanged) onOverlayChanged();
        if (onSpanHoverChanged) onSpanHoverChanged();
    }
    if (!dragging_ && !panning_ && !selecting_) {
        if (onUnsetCursor) onUnsetCursor();
    }
}
