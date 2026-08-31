#include "app/OverlayResample.h"

#include <QTest>

#include <cmath>

class OverlayResampleTest : public QObject {
    Q_OBJECT
private slots:
    void interpolatesHourlyRowsOntoALapGrid() {
        // Hourly weather around a lap starting 100 s into the file.
        const std::vector<qint64> times{-3600LL * 1000000000LL, 0,
                                        3600LL * 1000000000LL};
        const std::vector<double> values{10.0, 20.0, 30.0};
        LapEntry lap;
        lap.startTime = 100.0;
        lap.endTime = 200.0;
        std::vector<double> grid;
        for (int i = 0; i <= 100; ++i) grid.push_back(double(i));  // 1 Hz
        const auto out = omatrack::resampleSeriesOntoLap(
            times, values, lap, grid, 100LL * 1000000000LL,
            200LL * 1000000000LL, 0);
        QCOMPARE(out->size(), grid.size());
        // t = 100 s → 20 + 10 * 100/3600
        QVERIFY(std::abs(out->at(0) - (20.0 + 10.0 * 100.0 / 3600.0)) < 1e-9);
        QVERIFY(std::abs(out->at(99) - (20.0 + 10.0 * 199.0 / 3600.0)) < 1e-9);
        QVERIFY(std::isnan(out->at(100)));  // clip end is exclusive
    }
    void outsideCoverageAndGapsStayNaN() {
        const std::vector<qint64> times{0, 10LL * 1000000000LL,
                                        1000LL * 1000000000LL};
        const std::vector<double> values{1.0, 2.0, 3.0};
        LapEntry lap;
        lap.startTime = 0.0;
        lap.endTime = 2000.0;
        const std::vector<double> grid{-5.0, 5.0, 500.0, 1500.0};
        const auto out = omatrack::resampleSeriesOntoLap(
            times, values, lap, grid, 0, 0, 60LL * 1000000000LL);
        QVERIFY(std::isnan(out->at(0)));  // before first sample
        QCOMPARE(out->at(1), 1.5);
        QVERIFY(std::isnan(out->at(2)));  // 990 s gap > 60 s max
        QVERIFY(std::isnan(out->at(3)));  // after last sample
    }
    void nanNeighboursAreNotBridged() {
        const std::vector<qint64> times{0, 1000000000LL};
        const std::vector<double> values{1.0, std::nan("")};
        LapEntry lap;
        const std::vector<double> grid{0.0, 0.5};
        const auto out =
            omatrack::resampleSeriesOntoLap(times, values, lap, grid, 0, 0, 0);
        QCOMPARE(out->at(0), 1.0);
        QVERIFY(std::isnan(out->at(1)));
    }
};

QTEST_GUILESS_MAIN(OverlayResampleTest)
#include "OverlayResampleTest.moc"
