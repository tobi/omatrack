// origin: PUBLIC — generated mathematical samples only; no recording data.
#include "app/ImageTelemetryTraceGeometry.h"
#include "app/TraceSceneBuilder.h"

#include <QElapsedTimer>
#include <QtTest>

#include <algorithm>
#include <cmath>
#include <limits>

using namespace omatrack::inference;

namespace {
ImageTelemetrySeries emptySeries(std::int64_t duration) {
    ImageTelemetrySeries series;
    series.durationNs = duration;
    series.timelineOriginNs = -1'000'000'000;
    series.cells.resize(ImageTelemetrySeries::slotCount(duration));
    return series;
}
void observe(ImageTelemetrySeries& series, std::size_t index, double gear = 3,
             double fill = 40, std::int64_t offset = 50'000'000) {
    auto& slot = series.cells[index];
    slot.visited = slot.layoutSupported = true;
    slot.presentationPtsNs =
        std::int64_t(index) * ImageTelemetryPeriodNs + offset;
    slot.sourcePtsNs = *slot.presentationPtsNs + series.timelineOriginNs;
    slot.values = {{gear, 12, fill, 100 - fill}};
}
}  // namespace

class ImageTelemetryTraceGeometryTest : public QObject {
    Q_OBJECT
private slots:
    void actualPresentationClock() {
        auto series = emptySeries(2'000'000'000);
        observe(series, 1);
        QVERIFY(!image_trace::valueAt(series, 0, 0.22));
        QCOMPARE(image_trace::valueAt(series, 0, 0.26).value(), 3.0);
        QVERIFY(!image_trace::valueAt(series, 0, 0.4));
        image_trace::Projection projection;
        image_trace::project(series, 0, 2, projection);
        const auto& curve = projection.curves[0];
        bool actual = false;
        for (std::size_t i = 0; i < curve.times.size(); ++i)
            if (std::isfinite(curve.values[i])) {
                QVERIFY(curve.times[i] >= 0.25 && curve.times[i] < 0.4);
                actual |= curve.times[i] == 0.25;
            }
        QVERIFY(actual);
        QCOMPARE(curve.isolated.size(), 1);
        QCOMPARE(curve.isolated[0], QPointF(0.25, 3));
    }
    void gapAndFieldMasks() {
        auto series = emptySeries(2'000'000'000);
        observe(series, 0);
        observe(series, 1);
        observe(series, 3);
        series.cells[1].values[1].reset();
        series.cells[4].visited = true;  // observed unsupported, not unvisited
        QVERIFY(!image_trace::valueAt(series, 0, 0.45));
        QVERIFY(!image_trace::valueAt(series, 1, 0.3));
        QVERIFY(image_trace::valueAt(series, 0, 0.3));
        QVERIFY(!image_trace::valueAt(series, 0, 0.9));
        image_trace::Projection projection;
        image_trace::project(series, 0, 1, projection);
        QCOMPARE(projection.unknown[1].size(), std::size_t(2));
        QCOMPARE(projection.unknown[0].size(), std::size_t(1));
        QCOMPARE(projection.unknown[0][0].start, 0.8);
        QCOMPARE(projection.unvisited[0].start, 0.4);
        QVector<QPointF> path;
        image_trace::path(projection.curves[0], 0, 1, QRectF(0, 0, 1000, 100),
                          1, path);
        QVERIFY(std::any_of(path.begin(), path.end(),
                            [](QPointF p) { return !std::isfinite(p.x()); }));
        for (const auto& p : path)
            QVERIFY(!std::isfinite(p.x()) || p.x() <= 400 || p.x() >= 650);
    }
    void stepsAndContinuousFill() {
        auto series = emptySeries(1'000'000'000);
        observe(series, 0, 2, 20);
        observe(series, 1, 5, 80);
        QCOMPARE(image_trace::valueAt(series, 0, 0.15).value(), 2.0);
        QCOMPARE(image_trace::valueAt(series, 0, 0.249).value(), 2.0);
        QCOMPARE(image_trace::valueAt(series, 0, 0.25).value(), 5.0);
        QCOMPARE(image_trace::valueAt(series, 2, 0.15).value(), 50.0);
        // Unknown next cell forbids extending either kind of channel into it.
        QVERIFY(!image_trace::valueAt(series, 0, 0.4));
        QVERIFY(!image_trace::valueAt(series, 2, 0.4));
        image_trace::Projection projection;
        image_trace::project(series, 0, 1, projection);
        const auto& times = projection.curves[0].times;
        QCOMPARE(std::count(times.begin(), times.end(), 0.25), 2);
    }
    void exactTailAndInvalidEvidence() {
        auto series = emptySeries(450'000'000);
        observe(series, 2, 4, 0, 20'000'000);
        QCOMPARE(image_trace::valueAt(series, 2, 0.449).value(), 0.0);
        QVERIFY(!image_trace::valueAt(series, 2, 0.450));
        series.cells[2].sourcePtsNs = 0;
        QVERIFY(!image_trace::valueAt(series, 0, 0.43));
        observe(series, 2, 4, 0, 20'000'000);
        series.cells[2].layoutSupported = false;
        QVERIFY(!image_trace::valueAt(series, 0, 0.43));
        observe(series, 2, 4, 0, 20'000'000);
        series.cells[2].values[2] = std::numeric_limits<double>::infinity();
        QVERIFY(!image_trace::valueAt(series, 2, 0.43));
        series.cells[2].values[0] = 2.5;
        QVERIFY(!image_trace::valueAt(series, 0, 0.43));
        QVERIFY(!image_trace::valueAt(series, 0, -1));
        QVERIFY(!image_trace::valueAt(series, 4, 0.43));
    }
    void minMaxBudgetAndSingletons() {
        auto series = emptySeries(100'000'000'000);
        for (std::size_t i = 0; i < series.cells.size(); ++i)
            observe(series, i, 3, i == 200 ? 100 : 0);
        image_trace::Projection projection;
        image_trace::project(series, 0, 100, projection);
        QVector<QPointF> path;
        image_trace::path(projection.curves[2], 0, 100, QRectF(0, 0, 100, 100),
                          2, path);
        QVERIFY(path.size() <= 5 * 200 + 4);
        QVERIFY(std::any_of(path.begin(), path.end(), [](QPointF p) {
            return std::isfinite(p.y()) && p.y() < 0.1;
        }));
        for (std::size_t i = 1; i < series.cells.size(); i += 2)
            series.cells[i] = {};
        image_trace::project(series, 0, 100, projection);
        image_trace::markers(projection.curves[2], 0, 100,
                             QRectF(0, 0, 30, 100), 1, path);
        QVERIFY(!path.isEmpty());
        QVERIFY(path.size() <= 60);
        QVERIFY(std::all_of(path.begin(), path.end(), [](QPointF p) {
            return std::isfinite(p.x()) && std::isfinite(p.y());
        }));
    }
    void denseOverviewPreservesPeaksAndBoundsMasks() {
        auto series = emptySeries(200'000'000'000);
        for (std::size_t i = 0; i < series.cells.size(); ++i)
            observe(series, i, 3, i == 501 ? 100 : 0);
        image_trace::Projection projection;
        image_trace::project(series, 0, 200, projection, 10);
        const auto& curve = projection.curves[2];
        bool retained = false;
        for (std::size_t i = 0; i < curve.times.size(); ++i)
            retained |= curve.times[i] == 100.25 && curve.values[i] == 100;
        QVERIFY(retained);
        QVERIFY(curve.times.size() <= 62);
        QVERIFY(std::is_sorted(curve.times.begin(), curve.times.end()));
        for (std::size_t i = 0; i < series.cells.size(); i += 2)
            series.cells[i] = {};
        series.cells[3].visited = true;
        series.cells[3].values[2].reset();
        image_trace::project(series, 0, 200, projection, 10);
        QVERIFY(projection.unvisited.size() <= 10);
        QVERIFY(projection.unknown[2].size() <= 10);
        QVERIFY(projection.curves[2].times.size() <= 62);
        QVERIFY(projection.curves[2].isolated.size() <= 20);
        QVERIFY(std::any_of(projection.curves[2].isolated.begin(),
                            projection.curves[2].isolated.end(),
                            [](QPointF point) {
                                return point.x() == 100.25 && point.y() == 100;
                            }));
    }
    void viewportBoundAndCursorBenchmark() {
        auto series = emptySeries(ImageTelemetryMaxDurationNs);
        for (std::size_t i = 0; i < series.cells.size(); ++i)
            observe(series, i, double(i % 8), double(i % 101));
        image_trace::Projection projection;
        image_trace::project(series, 100, 101, projection);
        QVERIFY(projection.inspectedSlots <= 8);
        const auto totalSlots = series.cells.size();
        QElapsedTimer timer;
        timer.start();
        image_trace::project(series, 0, 86400, projection);
        const double projectionMs = timer.nsecsElapsed() / 1e6;
        QVector<QPointF> path;
        timer.restart();
        for (const auto& curve : projection.curves)
            image_trace::path(curve, 0, 86400, QRectF(0, 0, 1280, 80), 1, path);
        const double pathsMs = timer.nsecsElapsed() / 1e6;
        // Deliberately sparse: every overview column contains missing evidence.
        // Coverage masks and isolated markers must still have a pixel budget.
        for (std::size_t i = 1; i < series.cells.size(); i += 2)
            series.cells[i] = {};
        timer.restart();
        image_trace::project(series, 0, 86400, projection, 1280);
        TraceSceneBuilder builder;
        builder.begin(nullptr);
        const QColor color = QColor::fromRgbF(0.3, 0.7, 0.4);
        for (const auto& curve : projection.curves) {
            image_trace::markers(curve, 0, 86400, QRectF(0, 0, 1280, 80), 1,
                                 path);
            for (const auto& point : path)
                builder.line(point - QPointF(1, 0), point + QPointF(1, 0), 1.25,
                             color);
        }
        const double sparseGeometryMs = timer.nsecsElapsed() / 1e6;
        QVERIFY(builder.quadCount() <= 1280 * 2 * 4 * 8);
        timer.restart();
        double sum = 0;
        for (int i = 0; i < 100000; ++i)
            sum += image_trace::valueAt(series, std::size_t(i % 4),
                                        100 + (i % 1000) * 0.2 + 0.1)
                       .value_or(0);
        const double cursorNs = double(timer.nsecsElapsed()) / 100000;
        QVERIFY(sum > 0);
        QCOMPARE(series.cells.size(), totalSlots);
        qInfo() << "Synthetic 24h/432k-cell projection ms" << projectionMs
                << "four viewport paths ms" << pathsMs
                << "sparse projection+marker geometry ms" << sparseGeometryMs
                << "O(1) cursor lookup ns" << cursorNs;
    }
};
QTEST_APPLESS_MAIN(ImageTelemetryTraceGeometryTest)
#include "ImageTelemetryTraceGeometryTest.moc"
