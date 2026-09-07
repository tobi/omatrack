#include "app/GaugeSetup.h"
#include <QtTest>

using namespace omatrack;
class GaugeSetupTest : public QObject {
    Q_OBJECT
private slots:
    void independentFrames() {
        GaugeEvidence evidence;
        evidence.setup.detectorIdentity = "heuristic-v1";
        const auto regions = GaugeSetup::reviewedRegions();
        QVERIFY(!evidence.observe(0, {0, 0}, regions));
        QVERIFY(evidence.observe(0, {1920, 1080}, regions));
        QVERIFY(!evidence.observe(0, {1920, 1080}, regions));
        QVERIFY(!evidence.observe(1'999'999'999, {1920, 1080}, regions));
        QVERIFY(evidence.observe(3'000'000'000, {1920, 1080}, regions));
        QVERIFY(evidence.observe(6'000'000'000, {1920, 1080}, regions));
        QCOMPARE(evidence.setup.regions.size(), 4);
        QCOMPARE(evidence.setup.regions[0].hits, 3);
        QVERIFY(!evidence.observe(0, {1920, 1080}, regions));  // seek back
        QCOMPARE(evidence.sampleCount(), 3);
        QVERIFY(evidence.observe(9'000'000'000, {1920, 1080}, {}));
        QCOMPARE(evidence.setup.regions[0].hits, 2);
    }
    void proposalAndGeometry() {
        GaugeEvidence e;
        e.setup.detectorIdentity = "heuristic-v1";
        e.observe(0, {1920, 1080}, GaugeSetup::reviewedRegions());
        for (auto& r : e.setup.regions) r.confirmed = true;
        const auto original = e.setup;
        const auto decoded = GaugeSetup::fromMap(original.toMap());
        QVERIFY(decoded.valid());
        QCOMPARE(decoded.fingerprint(), original.fingerprint());
        QVERIFY(decoded.readableFields()[0]);
        e.propose(decoded);
        QCOMPARE(e.sampleCount(), 0);
        QVERIFY(!e.setup.regions[0].confirmed);
        QVERIFY(!e.setup.readableFields()[0]);
        e.observe(0, {1280, 720}, {});
        for (auto& r : e.setup.regions) r.confirmed = true;
        QVERIFY(!e.setup.readableFields()[0]);
        QVERIFY(e.setup.fingerprint() != original.fingerprint());
    }
    void persistedGeometryIdentity() {
        GaugeEvidence e;
        e.setup.detectorIdentity = "heuristic-v1";
        e.observe(0, {1920, 1080}, GaugeSetup::reviewedRegions());
        for (auto& r : e.setup.regions) r.confirmed = true;
        auto persisted = e.setup.toMap();
        auto rows = persisted.value("regions").toList();
        for (auto& value : rows) {
            auto row = value.toMap();
            for (const auto* key : {"x", "y", "width", "height"})
                row[key] =
                    QString::number(row[key].toDouble(), 'g', 10).toDouble();
            value = row;
        }
        persisted["regions"] = rows;  // Same numeric precision as omatrack.yml.
        const auto restored = GaugeSetup::fromMap(persisted);
        QCOMPARE(restored.fingerprint(), e.setup.fingerprint());
        const auto before = e.setup.readerConfiguration();
        const auto after = restored.readerConfiguration();
        for (int i = 0; i < 4; ++i) {
            QVERIFY(after.crops[i].enabled);
            QCOMPARE(after.crops[i].left, before.crops[i].left);
            QCOMPARE(after.crops[i].top, before.crops[i].top);
            QCOMPARE(after.crops[i].right, before.crops[i].right);
            QCOMPARE(after.crops[i].bottom, before.crops[i].bottom);
        }
    }
    void retireOnlyAutomaticMisses() {
        GaugeEvidence e;
        e.setup.detectorIdentity = "candidate";
        auto observations = GaugeSetup::reviewedRegions();
        e.observe(0, {1920, 1080}, observations);
        e.setup.regions[0].edited = true;
        e.setup.regions[1].proposal = true;
        e.observe(3'000'000'000, {1920, 1080}, {});
        e.observe(6'000'000'000, {1920, 1080}, {});
        e.observe(9'000'000'000, {1920, 1080}, {});
        QCOMPARE(e.setup.regions.size(), 2);
        QCOMPARE(e.setup.regions[0].id, QString("g1"));
        QCOMPARE(e.setup.regions[1].id, QString("g2"));
        QCOMPARE(e.setup.regions[0].hits, 0);
        e.observe(12'000'000'000, {1920, 1080}, {observations[2]});
        QCOMPARE(e.setup.regions.size(), 3);
        QVERIFY(e.setup.valid());
        // Persisted IDs are stable even after retirement left gaps, so a
        // revalidated/confirmed setup can reuse its content-addressed cache.
        auto proposal = e.setup;
        for (auto& r : proposal.regions) r.confirmed = true;
        const auto hash = proposal.fingerprint();
        e.propose(proposal);
        for (auto& r : e.setup.regions) r.confirmed = true;
        QCOMPARE(e.setup.fingerprint(), hash);
    }
    void editsAndUnknowns() {
        GaugeEvidence e;
        e.setup.detectorIdentity = "heuristic-v1";
        e.observe(0, {1920, 1080}, GaugeSetup::reviewedRegions());
        for (auto& r : e.setup.regions) r.confirmed = true;
        const auto key = e.setup.fingerprint();
        e.setup.regions[0].enabled = false;
        QVERIFY(!e.setup.readableFields()[0]);
        QVERIFY(e.setup.readableFields()[1]);
        QVERIFY(key != e.setup.fingerprint());
        e.setup.regions[1].box.translate(.001, 0);
        QVERIFY(!e.setup.readableFields()[1]);
        e.setup.regions[2].semantic = "speed";
        QVERIFY(!e.setup.readableFields()[2]);
        e.setup.regions[3].direction = "top_to_bottom";
        QVERIFY(!e.setup.readableFields()[3]);
        QVERIFY(e.setup.valid());  // unsupported != invalid
        const auto experimental = e.setup.readerConfiguration(false);
        QVERIFY(!experimental.crops[0].enabled);  // disabled
        QVERIFY(experimental.crops[1]
                    .enabled);  // moved digits API, not UI approval
        QVERIFY(!experimental.crops[2].enabled);  // unknown semantic
        QVERIFY(
            experimental.crops[3].enabled);  // explicit noncanonical direction
        QCOMPARE(experimental.crops[3].direction,
                 omatrack::inference::GaugeFillDirection::TopToBottom);
        auto invalid = e.setup;
        invalid.regions[0].box.setX(-.1);
        QVERIFY(!invalid.valid());
        QVERIFY(!GaugeSetup::fromMap(invalid.toMap()).valid());
    }
};
QTEST_GUILESS_MAIN(GaugeSetupTest)
#include "GaugeSetupTest.moc"
