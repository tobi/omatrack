#include "app/ChannelAppearance.h"
#include "app/TraceDecimator.h"
#include "app/TraceSceneBuilder.h"

#include <QSGGeometryNode>
#include <QTest>

#include <cmath>
#include <limits>
#include <vector>

namespace {
QVector<QPointF> path(
    const std::vector<double>& values, double start = 0, double span = 1,
    double dpr = 1,
    const std::function<double(double)>& map = [](double f) { return f; }) {
    QVector<QPointF> points;
    trace::decimate(values, map, start, span, QRectF(0, 0, 200, 100), 0, 10,
                    dpr, 0, 1, points);
    return points;
}
}  // namespace

class OverlayStyleTest : public QObject {
    Q_OBJECT
private slots:
    void appearanceDefaultsAndRoundTrip() {
        auto style = ChannelAppearance::defaults(QStringLiteral("throttle"));
        QCOMPARE(style.strokeWidth, 1.25);
        QCOMPARE(style.fillOpacity, 0.28);
        QCOMPARE(
            ChannelAppearance::defaults(QStringLiteral("speed")).fillOpacity,
            0.0);
        style.strokeWidth = 0.75;
        style.fillOpacity = 0.65;
        style.referenceColor = QColor(Qt::cyan);
        QVariantMap entry{{QStringLiteral("visible"), true}};
        style.writeTo(entry);
        QVERIFY(ChannelAppearance::fromMap(QStringLiteral("throttle"), entry) ==
                style);
        QVERIFY(entry.value(QStringLiteral("visible")).toBool());
        entry.insert(QStringLiteral("stroke_width"), -20.0);
        entry.insert(QStringLiteral("fill_opacity"), 8.0);
        auto clamped =
            ChannelAppearance::fromMap(QStringLiteral("speed"), entry);
        QCOMPARE(clamped.strokeWidth, 0.5);
        QCOMPARE(clamped.fillOpacity, 1.0);
        entry.insert(QStringLiteral("stroke_width"), "not a number");
        QCOMPARE(ChannelAppearance::fromMap(QStringLiteral("speed"), entry)
                     .strokeWidth,
                 1.25);
    }

    void rampHasNoSawtoothAtAnyZoom() {
        std::vector<double> ramp(100000);
        for (size_t i = 0; i < ramp.size(); ++i)
            ramp[i] = 10.0 * i / (ramp.size() - 1);
        for (double dpr : {1.0, 1.5, 2.0}) {
            for (double span : {1.0, 0.2, 0.002, 0.000001}) {
                const auto points = path(ramp, 0.0, span, dpr);
                QVERIFY(points.size() >= 2);
                for (int i = 1; i < points.size(); ++i) {
                    QVERIFY(points[i].x() > points[i - 1].x());
                    QVERIFY(points[i].y() <= points[i - 1].y());
                }
                for (const auto& p : points)
                    QVERIFY(std::abs(p.y() - (100.0 - p.x() * span / 2.0)) <
                            1e-7);
            }
        }
    }

    void denseStrokeIsBoundedAndPreservesExtremaOrder() {
        std::vector<double> values(100000, 5.0);
        values[50100] = 9.0;
        values[50200] = 1.0;
        const auto points = path(values);
        int high = -1, low = -1;
        for (int i = 0; i < points.size(); ++i) {
            if (std::abs(points[i].y() - 10) < 1e-8) high = i;
            if (std::abs(points[i].y() - 90) < 1e-8) low = i;
        }
        QVERIFY(high >= 0);
        QVERIFY(low > high);  // neither sorted by value nor stamped at one x
        QVERIFY(points[low].x() > points[high].x());
        for (size_t i = 0; i < values.size(); ++i) values[i] = i % 2 ? 0 : 10;
        QVERIFY(path(values).size() <= 4 * 200 + 2);
    }

    void nonlinearAlignmentIsLocalAndNeverEndpointInverted() {
        std::vector<double> ramp(101);
        for (size_t i = 0; i < ramp.size(); ++i) ramp[i] = double(i) / 10.0;
        const auto points = path(ramp, 0, 1, 1, [](double f) { return f * f; });
        for (const auto& point : points) {
            const double f = point.x() / 200;
            QVERIFY(std::abs(point.y() - (100 - 100 * f * f)) < 0.01);
        }
    }

    void nanEvenInsideOneColumnBreaksStroke() {
        std::vector<double> values(10000, 5.0);
        values[5010] = std::numeric_limits<double>::quiet_NaN();
        const auto points = path(values);
        int gaps = 0;
        for (const auto& point : points) gaps += !std::isfinite(point.x());
        QCOMPARE(gaps, 1);
        QVERIFY(std::isfinite(points.front().x()));
        QVERIFY(std::isfinite(points.back().x()));
        values.assign(10000, std::numeric_limits<double>::quiet_NaN());
        QVERIFY(path(values).isEmpty());
    }

    void simplificationStaysInsideSubpixelErrorBound() {
        std::vector<double> values(501);
        for (size_t i = 0; i < values.size(); ++i)
            values[i] =
                5.0 + 3.0 * std::sin(i * 0.018) + 0.01 * std::sin(i * 0.41);
        const auto points = path(values, 0, 1, 3.0);
        int segment = 1;
        for (size_t i = 0; i < values.size(); ++i) {
            const double x = double(i) / (values.size() - 1) * 200;
            while (segment + 1 < points.size() && points[segment].x() < x)
                ++segment;
            const QPointF a = points[segment - 1], b = points[segment];
            const double y =
                a.y() + (b.y() - a.y()) * (x - a.x()) / (b.x() - a.x());
            QVERIFY(std::abs(y - (100 - 10 * values[i])) <= 0.1 / 3.0 + 1e-7);
        }
    }

    void clipDoesNotExtrapolateAcrossLapBoundary() {
        const auto points = path({0, 10}, -0.5, 2.0);
        QVERIFY(!points.isEmpty());
        QCOMPARE(points.front().x(), 50.0);
        QCOMPARE(points.back().x(), 150.0);
    }

    void joinedStrokeHasOneDevicePixelCoverageAndConstantWidth() {
        for (double dpr : {1.0, 1.5, 2.0}) {
            TraceSceneBuilder builder;
            builder.begin(nullptr, dpr);
            const QPointF points[] = {{0, 50}, {100, 50}, {200, 50}};
            builder.polyline(points, 3, 1.25, QColor(Qt::white));
            QSGNode root;
            builder.commit(&root);
            auto* node = static_cast<QSGGeometryNode*>(root.firstChild());
            QVERIFY(node);
            const auto* geometry = node->geometry();
            const auto* vertices = geometry->vertexDataAsColoredPoint2D();
            float minY = 100, maxY = 0;
            bool transparent = false, opaque = false;
            for (int i = 0; i < geometry->vertexCount(); ++i) {
                minY = std::min(minY, vertices[i].y);
                maxY = std::max(maxY, vertices[i].y);
                transparent |= vertices[i].a == 0;
                opaque |= vertices[i].a == 255;
            }
            QVERIFY(transparent && opaque);
            QVERIFY(std::abs((maxY - minY) - (1.25 + 1.0 / dpr)) < 1e-4);
            QCOMPARE(geometry->indexType(),
                     unsigned(QSGGeometry::UnsignedIntType));
        }
    }

    void largeGeometryKeeps32BitIndices() {
        TraceSceneBuilder builder;
        builder.begin(nullptr);
        QVector<QPointF> points;
        for (int i = 0; i < 20000; ++i) points.append(QPointF(i, i % 2));
        builder.polyline(points.constData(), points.size(), 1.25,
                         QColor(Qt::white));
        QSGNode root;
        builder.commit(&root);
        const auto* geometry =
            static_cast<QSGGeometryNode*>(root.firstChild())->geometry();
        QVERIFY(geometry->vertexCount() > 65535);
        QCOMPARE(geometry->indexType(), unsigned(QSGGeometry::UnsignedIntType));
        QCOMPARE(builder.quadCount(), geometry->indexCount() / 6);
    }
};

QTEST_GUILESS_MAIN(OverlayStyleTest)
#include "OverlayStyleTest.moc"
