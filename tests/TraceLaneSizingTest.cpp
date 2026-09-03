#include "app/TraceLaneSizing.h"

#include <QTest>
#include <limits>
#include <numeric>

class TraceLaneSizingTest : public QObject {
    Q_OBJECT
private slots:
    void proportionalFit() {
        const auto heights = trace::fitLaneHeights({1, 2, 1}, 400);
        QCOMPARE(heights, (std::vector<double>{100, 200, 100}));
    }
    void noFixedMultiplierCap() {
        const auto heights = trace::fitLaneHeights({10000, 1, 1, 1}, 600);
        QCOMPARE(heights, (std::vector<double>{540, 20, 20, 20}));
    }
    void minimumFitsSmallPanes() {
        const auto heights = trace::fitLaneHeights({1, 100, 0.001}, 30);
        for (double height : heights) QVERIFY(std::abs(height - 10) < 1e-8);
    }
    void largeFiniteWeightsDoNotOverflow() {
        const auto heights = trace::fitLaneHeights({1e300, 1e300, 1}, 420);
        QCOMPARE(heights, (std::vector<double>{200, 200, 20}));
    }
    void invalidWeightsAreSafe() {
        const auto heights = trace::fitLaneHeights(
            {-1, 0, std::numeric_limits<double>::quiet_NaN()}, 300);
        QCOMPARE(heights, (std::vector<double>{100, 100, 100}));
        QVERIFY(trace::fitLaneHeights({}, 300).empty());
        QCOMPARE(trace::fitLaneHeights({1, 1}, 0), (std::vector<double>{0, 0}));
    }
    void dividerBorrowsAcrossNeighbours() {
        const std::vector<double> original{100, 100, 100, 100};
        const auto heights = trace::resizeLaneBoundary(original, 0, 200);
        QCOMPARE(heights, (std::vector<double>{300, 20, 20, 60}));
        QCOMPARE(std::accumulate(heights.begin(), heights.end(), 0.0), 400.0);
        QCOMPARE(original[0], 100.0);
        QCOMPARE(trace::resizeLaneBoundary(original, 0, 10000),
                 (std::vector<double>{340, 20, 20, 20}));
    }
    void lastLaneCanTakeSpaceFromAbove() {
        QCOMPARE(trace::resizeLaneBoundary({100, 100, 100, 100}, 2, -200),
                 (std::vector<double>{60, 20, 20, 300}));
    }
    void previewWeightsRoundTripToExactHeights() {
        const QStringList keys{"speed", "brake", "throttle", "raw:test"};
        const auto desired =
            trace::resizeLaneBoundary({140, 100, 100, 100}, 1, 130);
        double total = std::accumulate(desired.begin(), desired.end(), 0.0);
        double normal = 0;
        for (const auto& key : keys) normal += trace::laneHeightBoost(key);
        std::vector<double> effective;
        for (int i = 0; i < keys.size(); ++i) {
            const double weight = desired[size_t(i)] / total * normal /
                                  trace::laneHeightBoost(keys[i]);
            effective.push_back(weight * trace::laneHeightBoost(keys[i]));
        }
        const auto heights = trace::fitLaneHeights(effective, total);
        for (size_t i = 0; i < desired.size(); ++i)
            QVERIFY(std::abs(heights[i] - desired[i]) < 1e-8);
    }
};
QTEST_GUILESS_MAIN(TraceLaneSizingTest)
#include "TraceLaneSizingTest.moc"
