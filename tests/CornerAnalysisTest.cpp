// Contract tests for the corner analysis ported from ac-tracer.
//
// Each test builds a synthetic 50 Hz corner whose shape is obvious by
// construction, then asserts what the metrics say and which analyzer notes
// fire. The point is the contract a driver reads — "later turn-in", "throttle
// while braking" — not the arithmetic behind it.

#include <QtTest/QtTest>

#include "core/CornerAnalysis.h"
#include "core/TelemetryEngine.h"

#include <cmath>

using namespace omatrack;

namespace {

struct CornerShape {
    double entrySpeed = 200.0;  ///< km/h on the straight
    double apexSpeed = 100.0;   ///< km/h at the slowest point
    double brakeStart = 0.20;   ///< fraction of the lap where braking begins
    double apex = 0.50;
    double throttleOn = 0.60;  ///< fraction where the driver picks up throttle
    double peakBrakeBar = 80.0;
    double brakeRampSeconds = 0.2;
    double trailSeconds = 1.0;  ///< peak pressure to release
    double maxSteering = 60.0;
    int gearIn = 6;
    int gearApex = 3;
    double overlapSeconds = 0.0;         ///< throttle held during heavy braking
    double gearShiftDelaySeconds = 0.0;  ///< delay before the first downshift
};

/// Builds a one-corner lap: a straight, a braking zone, an apex, and a
/// throttle-on exit, sampled at 50 Hz over 20 s.
UnifiedLap makeLap(const CornerShape& shape) {
    constexpr int kRate = 50;
    constexpr double kDuration = 20.0;
    const int count = int(kRate * kDuration);

    UnifiedLap lap;
    lap.sampleRate = kRate;
    lap.time.reserve(count);
    lap.speed.reserve(count);
    lap.throttle.reserve(count);
    lap.brake.reserve(count);
    lap.steering.reserve(count);
    lap.distance.reserve(count);
    lap.gear.reserve(count);
    lap.gForceLong.reserve(count);
    lap.gForceLat.reserve(count);

    double distance = 0.0;
    for (int i = 0; i < count; ++i) {
        const double t = double(i) / kRate;
        const double f = double(i) / double(count - 1);

        double speed = shape.entrySpeed;
        if (f >= shape.brakeStart && f < shape.apex) {
            const double phase =
                (f - shape.brakeStart) / (shape.apex - shape.brakeStart);
            speed =
                shape.entrySpeed + (shape.apexSpeed - shape.entrySpeed) * phase;
        } else if (f >= shape.apex) {
            const double phase = (f - shape.apex) / (1.0 - shape.apex);
            speed =
                shape.apexSpeed + (shape.entrySpeed - shape.apexSpeed) * phase;
        }

        double brake = 0.0;
        if (f >= shape.brakeStart && f < shape.apex) {
            const double since = t - shape.brakeStart * kDuration;
            const double ramp =
                std::min(1.0, since / std::max(0.02, shape.brakeRampSeconds));
            const double release = std::max(
                0.0, 1.0 - std::max(0.0, since - shape.brakeRampSeconds) /
                               std::max(0.05, shape.trailSeconds));
            brake = shape.peakBrakeBar * ramp * release;
        }

        double throttle = f < shape.brakeStart ? 1.0 : 0.0;
        if (f >= shape.throttleOn) throttle = 1.0;
        // Optional mistake: throttle held while the brake is still hard on.
        if (shape.overlapSeconds > 0.0) {
            const double overlapStart = shape.brakeStart * kDuration + 0.05;
            if (t >= overlapStart && t < overlapStart + shape.overlapSeconds)
                throttle = 0.5;
        }

        double steering = 0.0;
        if (f >= shape.brakeStart) {
            const double phase = std::min(
                1.0, (f - shape.brakeStart) / (shape.apex - shape.brakeStart));
            steering = shape.maxSteering * phase;
        }

        // Downshifts land in the first 0.6 s of braking, as a real driver
        // does; spreading them over the whole zone would look like a slow
        // reaction to the analysis and it would be right.
        int gear = shape.gearIn;
        if (f >= shape.brakeStart) {
            const double since = t - shape.brakeStart * kDuration;
            const double phase = std::min(
                1.0, std::max(0.0, since - shape.gearShiftDelaySeconds) / 0.6);
            gear = shape.gearIn -
                   int(std::lround(phase * (shape.gearIn - shape.gearApex)));
        }

        lap.time.push_back(t);
        lap.speed.push_back(speed);
        lap.throttle.push_back(throttle);
        lap.brake.push_back(brake);
        lap.steering.push_back(steering);
        lap.gear.push_back(gear);
        lap.gForceLong.push_back(brake > 0.0 ? -1.2 : 0.4);
        lap.gForceLat.push_back(0.0);
        distance += speed / 3.6 / kRate;
        lap.distance.push_back(distance);
    }
    return lap;
}

bool hasNote(const std::vector<CornerNote>& notes, const char* id) {
    for (const CornerNote& note : notes)
        if (note.id == id) return true;
    return false;
}

std::string noteText(const std::vector<CornerNote>& notes, const char* id) {
    for (const CornerNote& note : notes)
        if (note.id == id) return note.text;
    return std::string();
}

CornerContext contextFor(const UnifiedLap& primary, const UnifiedLap& reference,
                         double start, double end) {
    CornerContext context;
    context.primary = &primary;
    context.reference = &reference;
    context.primaryMetrics = measureCorner(primary, start, end);
    context.referenceMetrics = measureCorner(reference, start, end);
    return context;
}

}  // namespace

class CornerAnalysisTest : public QObject {
    Q_OBJECT
private slots:
    // ── metrics ─────────────────────────────────────────────────────

    void measuresTheCornerShape() {
        const UnifiedLap lap = makeLap({});
        const CornerMetrics metrics = measureCorner(lap, 0.10, 0.80);
        QVERIFY(metrics.valid);
        // Entry is the fastest sample before the apex, exit the fastest after.
        QVERIFY(metrics.entrySpeed > metrics.apexSpeed);
        QVERIFY(metrics.exitSpeed > metrics.apexSpeed);
        QVERIFY(std::fabs(metrics.apexSpeed - 100.0) < 1.0);
        QCOMPARE(metrics.minGear, 3);
        QVERIFY(metrics.maxBrake > 70.0);
        QVERIFY(metrics.brakePoint > 0.0);
        QVERIFY(metrics.apexPoint > metrics.brakePoint);
        QVERIFY(std::isfinite(metrics.trailBrakeSeconds));
        QVERIFY(std::isfinite(metrics.brakeRiseRate));
    }

    void anEmptyRangeIsInvalid() {
        const UnifiedLap lap = makeLap({});
        QVERIFY(!measureCorner(lap, 0.5, 0.5).valid);
    }

    void unmappedLateralGIsNotLateralG() {
        // An unmapped channel unifies to zeros; that must not read as data.
        const UnifiedLap lap = makeLap({});
        const CornerMetrics metrics = measureCorner(lap, 0.10, 0.80);
        QVERIFY(!metrics.hasLateralG);
        QVERIFY(!std::isfinite(metrics.combinedGripEarly));
    }

    void turnInFallsBackToSteeringWithoutLateralG() {
        const UnifiedLap lap = makeLap({});
        const CornerMetrics metrics = measureCorner(lap, 0.10, 0.80);
        QVERIFY(std::isfinite(metrics.turnInPoint));
        // Turn-in happens on the way in, before the apex.
        QVERIFY(metrics.turnInPoint < metrics.apexPoint);
    }

    // ── analyzers ───────────────────────────────────────────────────

    void identicalLapsSayNothing() {
        const UnifiedLap lap = makeLap({});
        const std::vector<CornerNote> notes =
            CornerAnalysisRegistry::instance().run(
                contextFor(lap, lap, 0.10, 0.80));
        QVERIFY2(notes.empty(),
                 qPrintable(QString::fromStdString(
                     notes.empty() ? std::string() : notes.front().text)));
    }

    void reportsASlowDownshiftReaction() {
        // Hold the upshifted gear well into the brake zone.
        CornerShape lazy;
        lazy.gearShiftDelaySeconds = 1.2;
        const UnifiedLap primary = makeLap(lazy);
        const UnifiedLap reference = makeLap({});
        const std::vector<CornerNote> notes =
            CornerAnalysisRegistry::instance().run(
                contextFor(primary, reference, 0.10, 0.80));
        QVERIFY(hasNote(notes, "downshift_reaction"));
    }

    void reportsASlowerEntry() {
        CornerShape slower;
        slower.entrySpeed = 180.0;
        const UnifiedLap primary = makeLap(slower);
        const UnifiedLap reference = makeLap({});
        const std::vector<CornerNote> notes =
            CornerAnalysisRegistry::instance().run(
                contextFor(primary, reference, 0.10, 0.80));
        QVERIFY(hasNote(notes, "entry_speed"));
        QVERIFY(noteText(notes, "entry_speed").find("slower") !=
                std::string::npos);
    }

    void reportsALaterBrakeRelease() {
        CornerShape shortTrail;
        shortTrail.trailSeconds = 0.35;
        const UnifiedLap primary = makeLap(shortTrail);
        const UnifiedLap reference = makeLap({});
        const std::vector<CornerNote> notes =
            CornerAnalysisRegistry::instance().run(
                contextFor(primary, reference, 0.10, 0.80));
        QVERIFY(hasNote(notes, "trail_braking"));
    }

    void reportsThrottleWhileBraking() {
        CornerShape sloppy;
        sloppy.overlapSeconds = 1.2;  // well past a heel-toe blip
        const UnifiedLap primary = makeLap(sloppy);
        const UnifiedLap reference = makeLap({});
        const std::vector<CornerNote> notes =
            CornerAnalysisRegistry::instance().run(
                contextFor(primary, reference, 0.10, 0.80));
        QVERIFY(hasNote(notes, "brake_throttle_overlap"));
        for (const CornerNote& note : notes)
            if (note.id == "brake_throttle_overlap")
                QCOMPARE(note.severity, NoteSeverity::Error);
    }

    void ignoresAHeelToeBlip() {
        CornerShape blip;
        blip.overlapSeconds = 0.2;  // shorter than the 0.5 s blip allowance
        const UnifiedLap primary = makeLap(blip);
        const UnifiedLap reference = makeLap({});
        const std::vector<CornerNote> notes =
            CornerAnalysisRegistry::instance().run(
                contextFor(primary, reference, 0.10, 0.80));
        QVERIFY(!hasNote(notes, "brake_throttle_overlap"));
    }

    void reportsMoreSteering() {
        CornerShape sawing;
        sawing.maxSteering = 90.0;
        const UnifiedLap primary = makeLap(sawing);
        const UnifiedLap reference = makeLap({});
        const std::vector<CornerNote> notes =
            CornerAnalysisRegistry::instance().run(
                contextFor(primary, reference, 0.10, 0.80));
        QVERIFY(hasNote(notes, "steering_input"));
        QVERIFY(noteText(notes, "steering_input").find("more") !=
                std::string::npos);
    }

    void reportsALowerGear() {
        CornerShape lower;
        lower.gearApex = 2;
        const UnifiedLap primary = makeLap(lower);
        const UnifiedLap reference = makeLap({});
        const std::vector<CornerNote> notes =
            CornerAnalysisRegistry::instance().run(
                contextFor(primary, reference, 0.10, 0.80));
        QVERIFY(hasNote(notes, "gear_usage"));
        QVERIFY(noteText(notes, "gear_usage").find("lower") !=
                std::string::npos);
    }

    void reportsLighterBraking() {
        CornerShape light;
        light.peakBrakeBar = 50.0;
        const UnifiedLap primary = makeLap(light);
        const UnifiedLap reference = makeLap({});
        const std::vector<CornerNote> notes =
            CornerAnalysisRegistry::instance().run(
                contextFor(primary, reference, 0.10, 0.80));
        QVERIFY(hasNote(notes, "brake_pressure"));
        QVERIFY(noteText(notes, "brake_pressure").find("lighter") !=
                std::string::npos);
    }

    void singleLapRunsOnlyPrimaryChecks() {
        CornerShape sloppy;
        sloppy.overlapSeconds = 1.2;
        const UnifiedLap primary = makeLap(sloppy);
        CornerContext context;
        context.primary = &primary;
        context.primaryMetrics = measureCorner(primary, 0.10, 0.80);
        const std::vector<CornerNote> notes =
            CornerAnalysisRegistry::instance().run(context);
        QVERIFY(!context.comparing());
        QVERIFY(hasNote(notes, "brake_throttle_overlap"));
        QVERIFY(!hasNote(notes, "entry_speed"));
    }

    void everyAnalyzerHasAStableId() {
        QSet<QString> ids;
        for (const auto& analyzer :
             CornerAnalysisRegistry::instance().analyzers()) {
            const QString id = QString::fromLatin1(analyzer->id());
            QVERIFY2(!id.isEmpty(), "analyzer without an id");
            QVERIFY2(!ids.contains(id), qPrintable("duplicate id " + id));
            ids.insert(id);
        }
        QVERIFY(ids.size() >= 12);
    }
};

QTEST_APPLESS_MAIN(CornerAnalysisTest)
#include "CornerAnalysisTest.moc"
