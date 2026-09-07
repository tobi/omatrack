#include "app/GaugeSetup.h"
#include <QtTest>
#include <algorithm>

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
        QCOMPARE(decoded.readingFingerprint(), original.readingFingerprint());
        QVERIFY(decoded.readableFields()[0]);
        e.propose(decoded);
        QCOMPARE(e.sampleCount(), 0);
        QVERIFY(!e.reviewedLayoutVerified());
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
        QCOMPARE(restored.readingFingerprint(), e.setup.readingFingerprint());
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
    void boundaryInventorySurvivesYamlRounding() {
        GaugeEvidence e;
        e.setup.detectorIdentity = "experimental-v2";
        e.observe(0, {1920, 1080}, GaugeSetup::reviewedRegions());
        for (auto& r : e.setup.regions) r.confirmed = true;
        GaugeRegion edge;
        edge.id = "unselected-edge";
        edge.enabled = false;
        edge.box = {.8, .95387518095, .1, .04612481905};
        e.setup.regions.append(edge);
        QVERIFY(e.setup.valid());
        const auto restored = GaugeSetup::fromMap(e.setup.toMap());
        QVERIFY(restored.valid());
        QCOMPARE(restored.readingFingerprint(), e.setup.readingFingerprint());
        auto outside = restored;
        outside.regions.last().box.setHeight(.05);
        QVERIFY(!outside.valid());
    }
    void reviewedProfileAlongsideInventory() {
        GaugeEvidence e;
        e.setup.detectorIdentity = "experimental-v2";
        auto profile = GaugeSetup::reviewedRegions();
        auto inventory = profile;
        for (auto& r : inventory) {
            r.profileKey.clear();
            r.enabled = false;
        }
        for (int i = 0; i < 3; ++i)
            QVERIFY(e.observe(i * 3'000'000'000LL, {1920, 1080},
                              inventory + profile));
        QCOMPARE(e.setup.regions.size(), 8);
        QVERIFY(e.reviewedLayoutVerified());
        for (const auto& r : e.setup.regions) {
            QCOMPARE(r.hits, 3);
            QCOMPARE(r.enabled, !r.profileKey.isEmpty());
        }
        // Same pixels in the learned inventory cannot consume profile evidence.
        auto& gear = e.setup.regions[4];
        QCOMPARE(gear.profileKey, QString("gear"));
        gear.box.translate(.01, 0);
        gear.semantic = "speed";
        gear.enabled = false;
        gear.edited = true;
        const auto id = gear.id;
        const auto box = gear.box;
        for (int i = 3; i < 6; ++i)
            QVERIFY(e.observe(i * 3'000'000'000LL, {1920, 1080},
                              profile + inventory));
        QCOMPARE(e.setup.regions.size(), 8);
        const auto edited = e.setup.regions[4];
        QCOMPARE(edited.id, id);
        QCOMPARE(edited.box, box);
        QCOMPARE(edited.semantic, QString("speed"));
        QVERIFY(!edited.enabled);
        QCOMPARE(edited.hits, 0);
        const auto saved = GaugeSetup::fromMap(e.setup.toMap());
        e.propose(saved);
        QCOMPARE(e.sampleCount(), 0);
        QVERIFY(e.observe(0, {1920, 1080}, profile + inventory));
        QCOMPARE(e.setup.regions[4].profileKey, QString("gear"));
        QVERIFY(!e.setup.regions[4].enabled);
        QCOMPARE(e.setup.regions.size(), 8);
    }
    void detectorBackendsDoNotShareEvidence() {
        GaugeEvidence e;
        e.setup.detectorIdentity = "experimental-route-v1";
        GaugeRegion proposal;
        proposal.box = {.2, .2, .1, .1};
        proposal.enabled = false;
        proposal.detectorIdentity = "tiny-v2:hash";
        QVERIFY(e.observe(0, {1920, 1080}, {proposal}));
        proposal.detectorIdentity = "aim-large-v1:hash";
        QVERIFY(e.observe(3'000'000'000LL, {1920, 1080}, {proposal}));
        QVERIFY(e.observe(6'000'000'000LL, {1920, 1080}, {proposal}));
        QCOMPARE(e.setup.regions.size(), 2);
        QCOMPARE(e.setup.regions[0].hits, 0);
        QCOMPARE(e.setup.regions[1].hits, 2);
        QVERIFY(!e.setup.regions[1].enabled);
    }
    void profileCapacityIsReserved() {
        GaugeEvidence e;
        e.setup.detectorIdentity = "experimental-v2";
        QVector<GaugeRegion> inventory;
        for (int i = 0; i < GaugeSetup::MaxInventoryRegions; ++i) {
            GaugeRegion r;
            r.box = {i / 40.0, .2, .02, .02};
            r.enabled = false;
            inventory.append(r);
        }
        QVERIFY(e.observe(0, {1920, 1080}, inventory));
        QCOMPARE(e.setup.regions.size(), 32);
        QVERIFY(e.observe(3'000'000'000LL, {1920, 1080},
                          inventory + GaugeSetup::reviewedRegions()));
        QCOMPARE(e.setup.regions.size(), 36);
        QVERIFY(e.setup.valid());
        QCOMPARE(e.setup.regions[32].profileKey, QString("gear"));
    }
    void readingIdentityIgnoresUnselectedInventoryChurn() {
        GaugeEvidence e;
        e.setup.detectorIdentity = "experimental-v2:model-and-metadata-hashes";
        e.observe(0, {1920, 1080}, GaugeSetup::reviewedRegions());
        for (auto& r : e.setup.regions) r.confirmed = true;
        const auto original = e.setup;
        const auto reading = original.readingFingerprint();
        auto changed = original;
        GaugeRegion extra;
        extra.id = "detector-glyph";
        extra.box = {.2, .2, .1, .1};
        extra.enabled = false;
        changed.regions.prepend(extra);
        std::reverse(changed.regions.begin(), changed.regions.end());
        changed.regions[0].id = "new-track-id";
        changed.regions[0].edited = true;
        QVERIFY(changed.fingerprint() != original.fingerprint());
        QCOMPARE(changed.readingFingerprint(), reading);
        changed.regions[0].box.translate(.001, 0);
        QVERIFY(changed.readingFingerprint() != reading);
        changed = original;
        changed.regions[0].enabled = false;
        QVERIFY(changed.readingFingerprint() != reading);
        changed = original;
        changed.detectorIdentity += "different-model";
        QVERIFY(changed.readingFingerprint() != reading);
        changed = original;
        changed.sourceSize = {1280, 720};
        QVERIFY(changed.readingFingerprint() != reading);
        changed = original;
        changed.regions[2].direction = "top_to_bottom";
        QVERIFY(changed.readingFingerprint() != reading);
        changed = original;
        changed.regions[0].semantic = "speed";
        QVERIFY(changed.readingFingerprint() != reading);
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
