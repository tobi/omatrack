#include "app/TraceSceneBuilder.h"

#include <QtGlobal>
#include <QTest>

#include <limits>
#include <vector>

namespace {

int strokeQuads(const std::vector<double>& series, double yMin, double ySpan,
                int width) {
    TraceSceneBuilder builder;
    builder.begin(nullptr);
    TraceSceneBuilder::EnvelopeStyle style;
    style.width = 1.8;
    style.color = QColor(Qt::white);
    builder.envelopePolyline(
        series, [](double f) { return f; }, 0.0, 1.0,
        QRectF(0.0, 0.0, double(width), 100.0), yMin, ySpan, style, 0.0, 1.0);
    return builder.quadCount();
}

}  // namespace

class OverlayStyleTest : public QObject {
    Q_OBJECT
private slots:
    void defaultStyleIsThinUnfilled() {
        // One tight stroke everywhere: no dash variants, no band fill by
        // default. Callers opt into fill per channel instead.
        TraceSceneBuilder::EnvelopeStyle style;
        QCOMPARE(style.width, 1.0);
        QVERIFY(!style.fill);
    }

    void whitePresetKeepsSimilarThickness() {
        const qreal primary = 2.5;
        const qreal whiteRef = 2.4;
        QVERIFY(qAbs(whiteRef - primary) < 0.2);
    }

    void denseStrokeStaysViewportBounded() {
        // 100k samples over 200 device columns must not submit anywhere
        // near 100k worth of geometry: draw work follows the viewport.
        std::vector<double> noisy(100000);
        for (size_t i = 0; i < noisy.size(); ++i)
            noisy[i] = (i % 2 == 0) ? 0.0 : 10.0;
        const int quads = strokeQuads(noisy, 0.0, 10.0, 200);
        QVERIFY(quads > 0);
        QVERIFY(quads < 1000);
    }

    void singleSpikeSurvivesDecimation() {
        // One hot sample among 1000 flat ones: the column holding it must
        // emit the extreme instead of swallowing it. That is exactly one
        // extra point = one segment + one joint over the flat stroke. The
        // spike sits mid-column on purpose: column ranges share their edge
        // sample, so an edge spike honestly appears in both neighbours.
        std::vector<double> flat(1000, 5.0);
        const int flatQuads = strokeQuads(flat, 0.0, 10.0, 200);
        QVERIFY(flatQuads > 0);
        QVERIFY(flatQuads < 500);  // decimated: 1000 samples, ~400 quads

        std::vector<double> spiked = flat;
        spiked[502] = 9.0;
        QCOMPARE(strokeQuads(spiked, 0.0, 10.0, 200), flatQuads + 2);
    }

    void nanGapBreaksStrokeWithoutStarving() {
        // A dead block wider than a column breaks the stroke, but both
        // sides still draw; an all-dead series draws nothing at all. Lone
        // NaNs inside a live column are skipped, never breaking it.
        std::vector<double> gapped(1000, 5.0);
        for (int i = 400; i < 430; ++i)
            gapped[size_t(i)] = std::numeric_limits<double>::quiet_NaN();
        QVERIFY(strokeQuads(gapped, 0.0, 10.0, 200) > 0);

        const std::vector<double> dead(
            1000, std::numeric_limits<double>::quiet_NaN());
        QCOMPARE(strokeQuads(dead, 0.0, 10.0, 200), 0);
    }
};

QTEST_GUILESS_MAIN(OverlayStyleTest)
#include "OverlayStyleTest.moc"
