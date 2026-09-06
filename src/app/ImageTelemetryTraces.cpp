// origin: PUBLIC — native scene graph; no media, model, or cache I/O.
#include "ImageTelemetryTraces.h"
#include "ImageTelemetryController.h"
#include "ImageTelemetryTraceGeometry.h"
#include "TraceSceneBuilder.h"

#include <QElapsedTimer>
#include <QFontMetricsF>
#include <QMouseEvent>
#include <QQuickWindow>
#include <QSGNode>
#include <QWheelEvent>

#include <algorithm>
#include <array>
#include <cmath>

namespace {
// Avoid QQuickItem's inherited Top/Bottom transform-origin enum names.
constexpr qreal HeaderBandHeight = 20;
constexpr qreal FooterBandHeight = 24;
const std::array<const char*, 4> Labels{
    {"Gear", "Displayed lap", "Brake fill %", "Throttle fill %"}};

QColor tintedAlpha(QColor color, double factor) {
    if (color.isValid()) color.setAlphaF(color.alphaF() * factor);
    return color;
}
QString timeLabel(double seconds, double step) {
    const int decimals = step < 1 ? (step < 0.1 ? 2 : 1) : 0;
    if (seconds < 60)
        return QString::number(seconds, 'f', decimals) + QStringLiteral("s");
    const double scale = std::pow(10.0, decimals);
    const double rounded = std::round(seconds * scale) / scale;
    const qint64 minutes = qint64(std::floor(rounded / 60));
    const QString tail =
        QString::number(rounded - double(minutes) * 60, 'f', decimals)
            .rightJustified(decimals ? 3 + decimals : 2, QLatin1Char('0'));
    return QString::number(minutes) + QLatin1Char(':') + tail;
}
double tickStep(double span, qreal width) {
    const double raw = span / std::max(2.0, double(width) / 100.0);
    const double base =
        std::pow(10.0, std::floor(std::log10(std::max(1e-6, raw))));
    for (double multiple : {1.0, 2.0, 5.0, 10.0})
        if (base * multiple >= raw) return base * multiple;
    return base * 10;
}
struct TraceNode final : QSGNode {
    QSGNode* staticRoot = new QSGNode;
    QSGNode* cursorRoot = new QSGNode;
    TraceSceneBuilder staticBuilder;
    TraceSceneBuilder cursorBuilder;
    omatrack::inference::ImageTelemetrySnapshot snapshot;
    image_trace::Projection projection;
    QVector<QPointF> pathScratch;
    quint64 revision = 0;
    double position = -2;
    qreal dpr = 0;
    TraceNode() {
        appendChildNode(staticRoot);
        appendChildNode(cursorRoot);
    }
    ~TraceNode() override {
        // These builders and their cached textures are owned/destroyed by the
        // scene graph on its render thread, never by a GUI-thread item
        // finalizer.
        staticBuilder.releaseResources();
        cursorBuilder.releaseResources();
    }
};
}  // namespace

ImageTelemetryTraces::ImageTelemetryTraces(QQuickItem* parent)
    : QQuickItem(parent) {
    setFlag(ItemHasContents, true);
    setClip(true);
    setAcceptedMouseButtons(Qt::LeftButton | Qt::MiddleButton);
    connect(this, &ImageTelemetryTraces::paletteChanged, this, [this]() {
        updateLayout();
        invalidateStatic();
    });
    updateLayout();
}
ImageTelemetryController* ImageTelemetryTraces::controller() const {
    return controller_;
}
void ImageTelemetryTraces::setController(ImageTelemetryController* controller) {
    if (controller_ == controller) return;
    if (controller_) disconnect(controller_, nullptr, this, nullptr);
    controller_ = controller;
    if (controller_) {
        connect(controller_, &ImageTelemetryController::timelineChanged, this,
                &ImageTelemetryTraces::refreshSeries);
        connect(controller_, &QObject::destroyed, this, [this]() {
            controller_ = nullptr;
            setSeries({});
            emit controllerChanged();
        });
    }
    refreshSeries();
    emit controllerChanged();
}
void ImageTelemetryTraces::refreshSeries() {
    setSeries(controller_ ? controller_->series() : nullptr);
}
void ImageTelemetryTraces::setSeries(
    omatrack::inference::ImageTelemetrySnapshot series) {
    if (series_ == series) return;
    const bool newTimeline = !series_ || !series;
    series_ = std::move(series);
    if (newTimeline) resetView();
    invalidateStatic();
}
void ImageTelemetryTraces::invalidateStatic() {
    ++staticRevision_;
    update();
}
void ImageTelemetryTraces::setDuration(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0 || duration_ == seconds) return;
    duration_ = seconds;
    if (viewEnd_ >= 0) {
        viewEnd_ = std::min(viewEnd_, duration_);
        viewStart_ = std::min(viewStart_, std::max(0.0, viewEnd_ - 0.001));
    }
    invalidateStatic();
    emit durationChanged();
    emit viewChanged();
}
void ImageTelemetryTraces::setPosition(double seconds) {
    if (!std::isfinite(seconds)) seconds = -1;
    if (position_ == seconds) return;
    position_ = seconds;
    // No snapshot projection, range scan, or static builder work here.
    update();
    emit positionChanged();
}
double ImageTelemetryTraces::viewEnd() const {
    return viewEnd_ < 0 ? duration_ : viewEnd_;
}
void ImageTelemetryTraces::setViewStart(double seconds) {
    setView(seconds, viewEnd());
}
void ImageTelemetryTraces::setViewEnd(double seconds) {
    setView(viewStart_, seconds);
}
void ImageTelemetryTraces::setView(double start, double end) {
    if (!std::isfinite(start) || !std::isfinite(end) || duration_ <= 0 ||
        end <= start)
        return;
    const double span =
        std::clamp(end - start, std::min(0.001, duration_), duration_);
    start = std::clamp(start, 0.0, duration_ - span);
    end = start + span;
    if (viewStart_ == start && viewEnd() == end) return;
    viewStart_ = start;
    viewEnd_ = end;
    invalidateStatic();
    emit viewChanged();
}
void ImageTelemetryTraces::resetView() {
    if (viewStart_ == 0 && viewEnd_ < 0) return;
    viewStart_ = 0;
    viewEnd_ = -1;
    invalidateStatic();
    emit viewChanged();
}
void ImageTelemetryTraces::updateLayout() {
    const QFontMetricsF metrics(font_);
    const qreal labels = std::min(
        width() * 0.42, std::max(112.0, metrics.horizontalAdvance(
                                            QStringLiteral("Throttle fill %")) +
                                            22));
    plot_ =
        QRectF(labels, HeaderBandHeight, std::max(0.0, width() - labels - 8),
               std::max(0.0, height() - HeaderBandHeight - FooterBandHeight));
}
void ImageTelemetryTraces::geometryChange(const QRectF& next,
                                          const QRectF& previous) {
    QQuickItem::geometryChange(next, previous);
    if (next.size() != previous.size()) {
        updateLayout();
        invalidateStatic();
    }
}

QSGNode* ImageTelemetryTraces::updatePaintNode(QSGNode* oldNode,
                                               UpdatePaintNodeData*) {
    auto* node = oldNode ? static_cast<TraceNode*>(oldNode) : new TraceNode;
    const qreal dpr = window() ? window()->effectiveDevicePixelRatio() : 1;
    const bool rebuild = node->revision != staticRevision_ || node->dpr != dpr;
    const double start = viewStart_, end = viewEnd();
    const double span = end - start;
    const std::array<QColor, 4> colors{
        {gearColor_, lapColor_, brakeColor_, throttleColor_}};
    const auto xAt = [&](double seconds) {
        return plot_.left() + (seconds - start) / span * plot_.width();
    };
    const auto lane = [&](std::size_t field) {
        const qreal h = plot_.height() / 4;
        return QRectF(plot_.left(), plot_.top() + field * h + 6, plot_.width(),
                      std::max(1.0, h - 12));
    };
    if (rebuild) {
        QElapsedTimer elapsed;
        elapsed.start();
        node->snapshot =
            series_;  // GUI is blocked during this synchronization.
        auto& draw = node->staticBuilder;
        draw.begin(window());
        draw.rect(QRectF(0, 0, width(), height()), backgroundColor_);
        if (node->snapshot)
            image_trace::project(
                *node->snapshot, start, end, node->projection,
                int(std::min(32768.0, std::ceil(plot_.width() * dpr))));
        else {
            node->projection = {};
            if (span > 0) node->projection.unvisited.push_back({start, end});
        }
        if (plot_.width() > 2 && plot_.height() > 0 && span > 0) {
            draw.text(QStringLiteral("Video time"), font_, mutedColor_,
                      QRectF(5, height() - FooterBandHeight, plot_.left() - 10,
                             FooterBandHeight),
                      Qt::AlignLeft | Qt::AlignVCenter);
            draw.text(
                QStringLiteral("Shaded: unvisited   Dashes: observed unknown"),
                font_, mutedColor_,
                QRectF(plot_.left(), 0, plot_.width(), HeaderBandHeight),
                Qt::AlignLeft | Qt::AlignVCenter);
            for (const auto& uncovered : node->projection.unvisited)
                draw.rect(QRectF(xAt(uncovered.start), plot_.top(),
                                 xAt(uncovered.end) - xAt(uncovered.start),
                                 plot_.height()),
                          tintedAlpha(mutedColor_, 0.08));
            const double step = tickStep(span, plot_.width());
            for (double t = std::ceil(start / step) * step; t <= end;
                 t += step) {
                const qreal x = xAt(t);
                draw.vLine(x, plot_.top(), plot_.bottom(), 1,
                           tintedAlpha(gridColor_, 0.65));
                QRectF label(x - 40, height() - FooterBandHeight, 80,
                             FooterBandHeight);
                Qt::Alignment horizontal = Qt::AlignHCenter;
                if (x - plot_.left() < 40) {
                    label.moveLeft(x);
                    horizontal = Qt::AlignLeft;
                } else if (plot_.right() - x < 40) {
                    label.moveRight(x);
                    horizontal = Qt::AlignRight;
                }
                draw.text(timeLabel(t, step), font_, mutedColor_, label,
                          horizontal | Qt::AlignVCenter);
            }
            for (std::size_t field = 0; field < 4; ++field) {
                const QRectF rect = lane(field);
                const auto& curve = node->projection.curves[field];
                draw.text(QString::fromLatin1(Labels[field]), font_,
                          colors[field],
                          QRectF(5, rect.top(), plot_.left() - 12,
                                 std::min(18.0, rect.height())),
                          Qt::AlignLeft | Qt::AlignVCenter);
                const QString scale =
                    field >= 2 ? QStringLiteral("0–100")
                    : curve.hasValues
                        ? QStringLiteral("%1–%2")
                              .arg(std::ceil(curve.minimum), 0, 'f', 0)
                              .arg(std::floor(curve.maximum), 0, 'f', 0)
                        : QStringLiteral("—");
                draw.text(scale, font_, mutedColor_,
                          QRectF(5, rect.top() + 18, (plot_.left() - 12) * 0.55,
                                 std::max(0.0, rect.height() - 18)),
                          Qt::AlignLeft | Qt::AlignVCenter);
                draw.hLine(plot_.top() + field * plot_.height() / 4, 0, width(),
                           1, gridColor_);
                for (const auto& unknown : node->projection.unknown[field]) {
                    const qreal left = xAt(unknown.start),
                                right = xAt(unknown.end);
                    draw.rect(
                        QRectF(left, rect.top(), right - left, rect.height()),
                        tintedAlpha(mutedColor_, 0.04));
                    draw.dashedHLine(rect.center().y(), left, right,
                                     tintedAlpha(mutedColor_, 0.5), 2, 4);
                }
                image_trace::path(curve, start, end, rect, dpr,
                                  node->pathScratch);
                TraceSceneBuilder::EnvelopeStyle style;
                style.width = 1.25;
                style.color = colors[field];
                style.fill = field >= 2;
                style.fillColor = tintedAlpha(colors[field], 0.18);
                draw.seriesPath(node->pathScratch, rect, curve.minimum,
                                curve.maximum - curve.minimum, style);
                image_trace::markers(curve, start, end, rect, dpr,
                                     node->pathScratch);
                // An isolated observation is a tiny coverage stroke, not a
                // joined segment across missing cells. Avoid thousands of
                // tessellated circles in a sparse full-recording overview.
                for (const QPointF& point : node->pathScratch)
                    draw.line(point - QPointF(1.0 / dpr, 0),
                              point + QPointF(1.0 / dpr, 0), 1.25,
                              colors[field]);
            }
            draw.hLine(plot_.bottom(), 0, width(), 1, gridColor_);
        }
        draw.commit(node->staticRoot);
        node->revision = staticRevision_;
        node->dpr = dpr;
        staticNs_.store(elapsed.nsecsElapsed());
        ++staticBuilds_;
    }
    if (rebuild || node->position != position_) {
        QElapsedTimer elapsed;
        elapsed.start();
        auto& draw = node->cursorBuilder;
        draw.begin(window());
        if (plot_.width() > 2 && plot_.height() > 0 && span > 0) {
            const bool onScreen = position_ >= start && position_ <= end;
            if (onScreen)
                draw.vLine(xAt(position_), plot_.top(), plot_.bottom(), 1,
                           cursorColor_);
            for (std::size_t field = 0; field < 4; ++field) {
                const auto value = node->snapshot
                                       ? image_trace::valueAt(*node->snapshot,
                                                              field, position_)
                                       : std::nullopt;
                const QRectF rect = lane(field);
                const QString text =
                    value ? QString::number(*value, 'f', field < 2 ? 0 : 1)
                          : QStringLiteral("—");
                draw.text(text, font_, value ? colors[field] : mutedColor_,
                          QRectF(5, rect.top() + 18, plot_.left() - 12,
                                 std::max(0.0, rect.height() - 18)),
                          Qt::AlignRight | Qt::AlignVCenter);
                if (onScreen && value) {
                    const auto& curve = node->projection.curves[field];
                    const double y =
                        rect.bottom() - (*value - curve.minimum) /
                                            (curve.maximum - curve.minimum) *
                                            rect.height();
                    draw.dot(QPointF(xAt(position_),
                                     std::clamp(y, rect.top(), rect.bottom())),
                             2.2, colors[field]);
                }
            }
            if (position_ >= 0)
                draw.text(timeLabel(position_, 0.1), font_, foregroundColor_,
                          QRectF(5, 0, plot_.left() - 12, HeaderBandHeight),
                          Qt::AlignRight | Qt::AlignVCenter);
        }
        draw.commit(node->cursorRoot);
        node->position = position_;
        cursorNs_.store(elapsed.nsecsElapsed());
        ++cursorBuilds_;
    }
    return node;
}

double ImageTelemetryTraces::timeAt(qreal x) const {
    if (plot_.width() <= 0) return viewStart_;
    const double fraction =
        std::clamp(double((x - plot_.left()) / plot_.width()), 0.0, 1.0);
    return viewStart_ + fraction * (viewEnd() - viewStart_);
}
void ImageTelemetryTraces::mousePressEvent(QMouseEvent* event) {
    if (!plot_.contains(event->position()) || duration_ <= 0) {
        event->ignore();
        return;
    }
    pressedButton_ = event->button();
    pressPosition_ = event->position();
    pressStart_ = viewStart_;
    pressEnd_ = viewEnd();
    event->accept();
}
void ImageTelemetryTraces::mouseMoveEvent(QMouseEvent* event) {
    if (pressedButton_ != Qt::MiddleButton || plot_.width() <= 0) {
        event->ignore();
        return;
    }
    const double shift = (pressPosition_.x() - event->position().x()) /
                         plot_.width() * (pressEnd_ - pressStart_);
    setView(pressStart_ + shift, pressEnd_ + shift);
    event->accept();
}
void ImageTelemetryTraces::mouseReleaseEvent(QMouseEvent* event) {
    if (pressedButton_ == Qt::LeftButton && plot_.contains(event->position()) &&
        (event->position() - pressPosition_).manhattanLength() < 6)
        emit seekRequested(timeAt(event->position().x()));
    pressedButton_ = Qt::NoButton;
    event->accept();
}
void ImageTelemetryTraces::mouseDoubleClickEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        pressedButton_ = Qt::NoButton;
        resetView();
        event->accept();
    } else
        event->ignore();
}
void ImageTelemetryTraces::pan(double seconds) {
    setView(viewStart_ + seconds, viewEnd() + seconds);
}
void ImageTelemetryTraces::wheelEvent(QWheelEvent* event) {
    if (!plot_.contains(event->position()) || duration_ <= 0) {
        event->ignore();
        return;
    }
    if (event->pixelDelta().x() != 0 &&
        std::abs(event->pixelDelta().x()) > std::abs(event->pixelDelta().y())) {
        pan(-double(event->pixelDelta().x()) / plot_.width() *
            (viewEnd() - viewStart_));
    } else {
        const double steps = event->angleDelta().y()
                                 ? event->angleDelta().y() / 120.0
                                 : event->pixelDelta().y() / 80.0;
        const double anchor = timeAt(event->position().x());
        const double factor = std::pow(1.25, -std::clamp(steps, -20.0, 20.0));
        setView(anchor + (viewStart_ - anchor) * factor,
                anchor + (viewEnd() - anchor) * factor);
    }
    event->accept();
}
