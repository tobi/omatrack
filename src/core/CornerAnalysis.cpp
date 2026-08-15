#include "CornerAnalysis.h"

#include "TelemetryEngine.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <limits>

namespace omatrack {

namespace {

// Thresholds, ported from the ac-tracer corner analysis. They are named
// because the numbers are the product decision, not the code.
constexpr double kBrakeOnBar = 2.0;          ///< brake considered applied
constexpr double kBrakeZoneBar = 20.0;       ///< a corner braked for in earnest
constexpr double kHeavyBrakeBar = 30.0;      ///< "braking hard"
constexpr double kBlipThrottle = 0.30;       ///< above a heel-toe blip
constexpr double kBlipSeconds = 0.5;         ///< longest allowed overlap
constexpr double kThrottleOnFraction = 0.9;  ///< sustained application
constexpr double kLiftThrottle = 0.9;        ///< throttle lift-off
constexpr double kSustainedSeconds = 0.2;  ///< application must hold this long
constexpr double kGearShiftSeconds = 0.5;  ///< blip window around a shift
constexpr int kMinSamples = 8;

double nan() { return std::numeric_limits<double>::quiet_NaN(); }

bool finite(double value) { return std::isfinite(value); }

__attribute__((format(printf, 1, 2))) std::string format(const char* pattern,
                                                         ...) {
    char buffer[192];
    va_list args;
    va_start(args, pattern);
    const int written = std::vsnprintf(buffer, sizeof(buffer), pattern, args);
    va_end(args);
    if (written <= 0) return std::string();
    return std::string(buffer,
                       size_t(std::min<int>(written, int(sizeof(buffer)) - 1)));
}

/// Turn-in, ported from detectTurnIn(): find where lateral load starts
/// building, then walk back to where the steering left its approach baseline.
/// Lateral G drives it when the lap has it; steering alone is the fallback for
/// logs without an accelerometer channel.
int detectTurnIn(const UnifiedLap& lap, int first, int apex, bool hasLateralG) {
    const int count = apex - first + 1;
    if (count < kMinSamples || lap.steering.size() < size_t(apex + 1))
        return -1;

    double peakSteer = 0.0;
    double peakLat = 0.0;
    for (int i = first; i <= apex; ++i) {
        peakSteer = std::max(peakSteer, std::fabs(lap.steering[size_t(i)]));
        if (hasLateralG)
            peakLat = std::max(peakLat, std::fabs(lap.gForceLat[size_t(i)]));
    }

    // The first 15% of the approach is the baseline; averaging suppresses
    // single-sample noise.
    const int baselineCount = std::max(3, std::min(10, count * 15 / 100));
    double baselineSteer = 0.0;
    double baselineLat = 0.0;
    for (int i = first; i < first + baselineCount; ++i) {
        baselineSteer += std::fabs(lap.steering[size_t(i)]);
        if (hasLateralG) baselineLat += std::fabs(lap.gForceLat[size_t(i)]);
    }
    baselineSteer /= baselineCount;
    baselineLat /= baselineCount;

    const double steerRange = peakSteer - baselineSteer;
    if (steerRange < 5.0) return -1;
    const double committedSteer = baselineSteer + steerRange * 0.45;
    const double latRange = peakLat - baselineLat;
    const double latThreshold = baselineLat + std::max(0.12, latRange * 0.18);
    constexpr int kSustained = 3;

    int onset = -1;
    const bool useLateral = hasLateralG && latRange >= 0.15;
    for (int i = first + baselineCount; i <= apex - kSustained + 1; ++i) {
        bool sustained = true;
        for (int k = i; k < i + kSustained; ++k) {
            const double value = useLateral
                                     ? std::fabs(lap.gForceLat[size_t(k)])
                                     : std::fabs(lap.steering[size_t(k)]);
            const double threshold = useLateral ? latThreshold : committedSteer;
            if (value < threshold) {
                sustained = false;
                break;
            }
        }
        if (sustained) {
            onset = i;
            break;
        }
    }
    if (onset < 0) return -1;

    if (useLateral) {
        // Confirm the lateral event belongs to a committed steering input
        // rather than a bump or a kerb strike.
        bool commits = false;
        for (int i = std::max(first + baselineCount, onset - 6);
             i <= std::min(onset + 3, apex); ++i) {
            if (std::fabs(lap.steering[size_t(i)]) >= committedSteer) {
                commits = true;
                break;
            }
        }
        if (!commits) return -1;
    }

    // Walk back through the same contiguous steering build: the first sample
    // after steering leaves its baseline is the physical start of turn-in.
    const double baselineExit =
        baselineSteer + std::max(1.0, steerRange * 0.07);
    const int searchStart = std::max(first + baselineCount, onset - 20);
    while (onset > searchStart &&
           std::fabs(lap.steering[size_t(onset - 1)]) > baselineExit)
        --onset;
    return onset;
}

bool nearGearShift(const UnifiedLap& lap, int index, int window) {
    if (lap.gear.size() < 2) return false;
    const int last = int(lap.gear.size()) - 1;
    const int gear = lap.gear[size_t(std::clamp(index, 0, last))];
    for (int i = std::max(0, index - window);
         i <= std::min(last, index + window); ++i)
        if (lap.gear[size_t(i)] != gear) return true;
    return false;
}

}  // namespace

const char* severityName(NoteSeverity severity) {
    switch (severity) {
        case NoteSeverity::Error: return "error";
        case NoteSeverity::Warning: return "warning";
        case NoteSeverity::Info: break;
    }
    return "info";
}

// ── metrics ─────────────────────────────────────────────────────────

CornerMetrics measureCorner(const UnifiedLap& lap, double startFraction,
                            double endFraction, bool allowLateralG) {
    CornerMetrics metrics;
    const int last = int(lap.size()) - 1;
    if (last < 2 || lap.distance.size() < size_t(last + 1) ||
        lap.time.size() < size_t(last + 1))
        return metrics;

    const int first =
        std::clamp(int(std::floor(startFraction * last)), 0, last);
    const int finish =
        std::clamp(int(std::ceil(endFraction * last)), first, last);
    if (finish - first < 2) return metrics;

    metrics.valid = true;
    metrics.firstIndex = first;
    metrics.lastIndex = finish;
    metrics.startDistance = lap.distance[size_t(first)];
    metrics.lengthMeters =
        std::max(1.0, lap.distance[size_t(finish)] - metrics.startDistance);
    metrics.time = lap.time[size_t(finish)] - lap.time[size_t(first)];
    // An unmapped channel unifies to zeros, not to an empty vector, so the
    // lap "has" lateral G only when the corner actually carries a signal.
    const bool lateralSized = lap.gForceLat.size() >= size_t(finish + 1);

    const auto distanceFrom = [&](int index) {
        return lap.distance[size_t(index)] - metrics.startDistance;
    };

    // One pass for the extremes the rest of the metrics are defined against.
    double apexSpeed = std::numeric_limits<double>::infinity();
    int apexIndex = first;
    int minGear = 99;
    for (int i = first; i <= finish; ++i) {
        const double speed = lap.speed[size_t(i)];
        if (finite(speed) && speed < apexSpeed) {
            apexSpeed = speed;
            apexIndex = i;
        }
        metrics.maxSteering =
            std::max(metrics.maxSteering, std::fabs(lap.steering[size_t(i)]));
        metrics.maxBrake = std::max(metrics.maxBrake, lap.brake[size_t(i)]);
        metrics.minThrottle =
            std::min(metrics.minThrottle, lap.throttle[size_t(i)]);
        if (i < int(lap.gear.size()))
            minGear = std::min(minGear, lap.gear[size_t(i)]);
        if (lateralSized) {
            const double lateral = std::fabs(lap.gForceLat[size_t(i)]);
            if (finite(lateral) && lateral > 1.0e-6) metrics.hasLateralG = true;
            metrics.peakLateralG = std::max(metrics.peakLateralG, lateral);
        }
    }
    metrics.apexIndex = apexIndex;
    metrics.apexSpeed = std::isfinite(apexSpeed) ? apexSpeed : 0.0;
    metrics.apexPoint = distanceFrom(apexIndex);
    metrics.minGear = minGear == 99 ? 0 : minGear;

    // Entry is the fastest sample before the apex, exit the fastest after —
    // the corner's own speed profile, not the zone's arbitrary bounds.
    double entry = 0.0;
    for (int i = first; i <= apexIndex; ++i)
        entry = std::max(entry, lap.speed[size_t(i)]);
    double exit = 0.0;
    for (int i = apexIndex; i <= finish; ++i)
        exit = std::max(exit, lap.speed[size_t(i)]);
    metrics.entrySpeed = entry;
    metrics.exitSpeed = exit;

    // Brake application, throttle lift, and the coast between them.
    for (int i = first; i <= finish; ++i) {
        if (metrics.brakeIndex < 0 && lap.brake[size_t(i)] > kBrakeOnBar)
            metrics.brakeIndex = i;
        if (metrics.liftIndex < 0 && lap.throttle[size_t(i)] < kLiftThrottle)
            metrics.liftIndex = i;
    }
    metrics.brakePoint =
        metrics.brakeIndex >= 0 ? distanceFrom(metrics.brakeIndex) : nan();
    metrics.liftPoint =
        metrics.liftIndex >= 0 ? distanceFrom(metrics.liftIndex) : nan();
    metrics.coastMeters =
        (metrics.brakeIndex >= 0 && metrics.liftIndex >= 0)
            ? std::max(0.0, metrics.brakePoint - metrics.liftPoint)
            : nan();

    const int turnIn = detectTurnIn(lap, first, apexIndex,
                                    metrics.hasLateralG && allowLateralG);
    metrics.turnInIndex = turnIn;
    metrics.turnInPoint = turnIn >= 0 ? distanceFrom(turnIn) : nan();

    // Throttle pickup: first sustained application after the apex that is not
    // a rev-matching blip around a gear shift.
    const int rate = std::max(1, lap.sampleRate);
    const int sustainedSamples =
        std::max(1, int(std::lround(kSustainedSeconds * rate)));
    const int shiftWindow =
        std::max(1, int(std::lround(kGearShiftSeconds * rate)));
    for (int i = apexIndex; i <= finish; ++i) {
        if (lap.throttle[size_t(i)] < kThrottleOnFraction) continue;
        if (nearGearShift(lap, i, shiftWindow)) continue;
        bool sustained = true;
        for (int k = i; k < i + sustainedSamples && k <= finish; ++k)
            if (lap.throttle[size_t(k)] < kThrottleOnFraction) {
                sustained = false;
                break;
            }
        if (!sustained) continue;
        metrics.throttleIndex = i;
        break;
    }
    metrics.throttlePoint = metrics.throttleIndex >= 0
                                ? distanceFrom(metrics.throttleIndex)
                                : nan();

    // Brake pressure ramp: 10% → 90% of this corner's peak.
    metrics.brakeRiseRate = nan();
    if (metrics.maxBrake >= 20.0) {
        const double onsetBar = std::max(5.0, metrics.maxBrake * 0.1);
        const double targetBar = metrics.maxBrake * 0.9;
        double onsetTime = nan();
        for (int i = first; i <= finish; ++i) {
            const double pressure = lap.brake[size_t(i)];
            if (!finite(onsetTime) && pressure >= onsetBar)
                onsetTime = lap.time[size_t(i)];
            if (finite(onsetTime) && pressure >= targetBar) {
                const double rise =
                    std::max(0.02, lap.time[size_t(i)] - onsetTime);
                metrics.brakeRiseRate = (targetBar - onsetBar) / rise;
                break;
            }
        }
    }

    // Trail braking: peak pressure to release.
    metrics.trailBrakeSeconds = nan();
    if (metrics.maxBrake >= 20.0) {
        int peakIndex = -1;
        double peak = 0.0;
        for (int i = first; i <= finish; ++i)
            if (lap.brake[size_t(i)] > peak) {
                peak = lap.brake[size_t(i)];
                peakIndex = i;
            }
        if (peakIndex >= 0) {
            const double releaseBar = std::max(5.0, peak * 0.1);
            int below = 0;
            for (int i = peakIndex + 1; i <= finish; ++i) {
                if (lap.brake[size_t(i)] <= releaseBar) {
                    if (++below >= 3) {
                        metrics.trailBrakeSeconds =
                            std::max(0.0, lap.time[size_t(i - 2)] -
                                              lap.time[size_t(peakIndex)]);
                        break;
                    }
                } else {
                    below = 0;
                }
            }
        }
    }

    // Throttle held while braking hard, beyond a heel-toe blip.
    int overlapRun = 0;
    int longestOverlap = 0;
    for (int i = first; i <= finish; ++i) {
        if (lap.brake[size_t(i)] >= kHeavyBrakeBar &&
            lap.throttle[size_t(i)] >= kBlipThrottle) {
            longestOverlap = std::max(longestOverlap, ++overlapRun);
        } else {
            overlapRun = 0;
        }
    }
    metrics.brakeThrottleOverlapSeconds = double(longestOverlap) / rate;

    // Downshift sequence, measured from brake application.
    metrics.downshiftFirstMs = nan();
    metrics.downshiftLastMs = nan();
    metrics.downshiftDistance = nan();
    if (metrics.brakeIndex >= 0 && lap.gear.size() > size_t(finish)) {
        const double brakeTime = lap.time[size_t(metrics.brakeIndex)];
        int previous = lap.gear[size_t(metrics.brakeIndex)];
        for (int i = metrics.brakeIndex + 1; i <= finish; ++i) {
            const int gear = lap.gear[size_t(i)];
            if (gear < previous) {
                const double elapsed =
                    (lap.time[size_t(i)] - brakeTime) * 1000.0;
                if (!finite(metrics.downshiftFirstMs)) {
                    metrics.downshiftFirstMs = elapsed;
                    metrics.downshiftDistance =
                        distanceFrom(i) - metrics.brakePoint;
                }
                metrics.downshiftLastMs = elapsed;
            }
            previous = gear;
        }
    }

    // Combined lateral + braking grip, split into turn-in→mid and mid→apex.
    metrics.combinedGripEarly = nan();
    metrics.combinedGripMid = nan();
    if (metrics.hasLateralG && turnIn >= 0 && apexIndex > turnIn + 1) {
        const int split = (turnIn + apexIndex) / 2;
        const auto average = [&](int from, int to) {
            double sum = 0.0;
            int count = 0;
            for (int i = from; i <= to; ++i) {
                const double lateral = lap.gForceLat[size_t(i)];
                const double longitudinal = i < int(lap.gForceLong.size())
                                                ? lap.gForceLong[size_t(i)]
                                                : 0.0;
                if (!finite(lateral) || !finite(longitudinal)) continue;
                const double braking = std::max(0.0, -longitudinal);
                sum += std::sqrt(lateral * lateral + braking * braking);
                ++count;
            }
            return count >= 2 ? sum / count : nan();
        };
        metrics.combinedGripEarly = average(turnIn, split);
        metrics.combinedGripMid = average(split + 1, apexIndex);
    }

    return metrics;
}

// ── analyzers ───────────────────────────────────────────────────────

namespace {

using Notes = std::vector<CornerNote>;

/// Boilerplate for a check: an id, a reference requirement, and a body.
#define OMATRACK_ANALYZER(ClassName, Id, NeedsReference)                   \
    class ClassName : public CornerAnalyzer {                              \
    public:                                                                \
        const char* id() const override { return Id; }                     \
        bool requiresReference() const override { return NeedsReference; } \
        void analyze(const CornerContext& context,                         \
                     Notes& notes) const override;                         \
    };                                                                     \
    void ClassName::analyze(const CornerContext& context, Notes& notes) const

OMATRACK_ANALYZER(EntrySpeedAnalyzer, "entry_speed", true) {
    const double delta =
        context.primaryMetrics.entrySpeed - context.referenceMetrics.entrySpeed;
    if (std::fabs(delta) < 10.0) return;
    notes.push_back({id(),
                     format("entry %.0f km/h %s", std::fabs(delta),
                            delta > 0.0 ? "faster" : "slower"),
                     NoteSeverity::Info});
}

OMATRACK_ANALYZER(SteeringAnalyzer, "steering_input", true) {
    const double delta = context.primaryMetrics.maxSteering -
                         context.referenceMetrics.maxSteering;
    if (std::fabs(delta) <= 10.0) return;
    notes.push_back({id(),
                     format("%.0f° %s steering", std::fabs(delta),
                            delta > 0.0 ? "more" : "less"),
                     NoteSeverity::Info});
}

OMATRACK_ANALYZER(GearAnalyzer, "gear_usage", true) {
    const int delta =
        context.primaryMetrics.minGear - context.referenceMetrics.minGear;
    if (delta == 0 || context.primaryMetrics.minGear <= 0 ||
        context.referenceMetrics.minGear <= 0)
        return;
    const int magnitude = std::abs(delta);
    notes.push_back({id(),
                     format("%d gear%s %s", magnitude, magnitude > 1 ? "s" : "",
                            delta > 0 ? "higher" : "lower"),
                     NoteSeverity::Info});
}

OMATRACK_ANALYZER(CoastingAnalyzer, "coasting", true) {
    const double primary = context.primaryMetrics.coastMeters;
    const double reference = context.referenceMetrics.coastMeters;
    if (!finite(primary) || !finite(reference)) return;
    const double delta = primary - reference;
    if (std::fabs(delta) < 15.0) return;
    notes.push_back({id(),
                     format("%.0fm %s coasting", std::fabs(delta),
                            delta > 0.0 ? "more" : "less"),
                     NoteSeverity::Info});
}

double alignedOrRaw(double aligned, double primary, double reference) {
    if (finite(aligned)) return aligned;
    if (!finite(primary) || !finite(reference)) return nan();
    return primary - reference;
}

OMATRACK_ANALYZER(TurnInAnalyzer, "turn_in", true) {
    const double delta =
        alignedOrRaw(context.turnInDelta, context.primaryMetrics.turnInPoint,
                     context.referenceMetrics.turnInPoint);
    if (!finite(delta)) return;
    if (std::fabs(delta) < 10.0) return;
    notes.push_back({id(),
                     format("turn-in %.0fm %s than reference", std::fabs(delta),
                            delta > 0.0 ? "later" : "earlier"),
                     NoteSeverity::Info});
}

OMATRACK_ANALYZER(ThrottleTimingAnalyzer, "throttle_timing", true) {
    const double delta = alignedOrRaw(context.throttlePointDelta,
                                      context.primaryMetrics.throttlePoint,
                                      context.referenceMetrics.throttlePoint);
    if (!finite(delta)) return;
    if (std::fabs(delta) < 15.0) return;
    notes.push_back({id(),
                     format("throttle %.0fm %s", std::fabs(delta),
                            delta < 0.0 ? "early" : "late"),
                     NoteSeverity::Info});
}

OMATRACK_ANALYZER(BrakePressureAnalyzer, "brake_pressure", true) {
    const double primary = context.primaryMetrics.maxBrake;
    const double reference = context.referenceMetrics.maxBrake;
    // ac-tracer only guards against a zero reference. A corner that neither
    // lap really brakes for then produces "lighter braking (0 vs 11 bar)",
    // which is noise — require an actual brake zone on one of the two laps.
    if (std::max(primary, reference) < kBrakeZoneBar) return;
    if (reference <= 0.0) return;
    const double delta = primary - reference;
    if (std::fabs(delta) / reference <= 0.10) return;
    notes.push_back(
        {id(),
         format("%s braking (%.0f vs %.0f bar)",
                delta < 0.0 ? "lighter" : "harder", primary, reference),
         NoteSeverity::Info});
}

OMATRACK_ANALYZER(BrakeRateAnalyzer, "brake_application_rate", true) {
    const double primary = context.primaryMetrics.brakeRiseRate;
    const double reference = context.referenceMetrics.brakeRiseRate;
    if (!finite(primary) || !finite(reference) || reference < 50.0) return;
    const double ratio = primary / reference;
    // Elite references spike pressure very fast; only coach a ramp that is
    // less than half as quick and clearly separated in absolute terms.
    if (ratio >= 0.5 || reference - primary < 100.0) return;
    notes.push_back({id(),
                     format("brake application rate %.0f%% slower than "
                            "reference",
                            (1.0 - ratio) * 100.0),
                     NoteSeverity::Info});
}

OMATRACK_ANALYZER(TrailBrakingAnalyzer, "trail_braking", true) {
    const double primary = context.primaryMetrics.trailBrakeSeconds;
    const double reference = context.referenceMetrics.trailBrakeSeconds;
    if (!finite(primary) || !finite(reference)) return;
    const double delta = reference - primary;
    if (delta < 0.3 || reference < primary * 1.3) return;
    notes.push_back({id(), format("reference trail-brakes %.1fs longer", delta),
                     NoteSeverity::Info});
}

OMATRACK_ANALYZER(DownshiftReactionAnalyzer, "downshift_reaction", true) {
    const double distance = context.primaryMetrics.downshiftDistance;
    const double reaction = context.primaryMetrics.downshiftFirstMs;
    const double referenceReaction = context.referenceMetrics.downshiftFirstMs;
    if (!finite(distance) || !finite(reaction) || !finite(referenceReaction))
        return;
    // ac-tracer gates this on an absolute 5 m of braking. Every corner of a
    // real LMP2 lap clears that — the first downshift lands half a second
    // into the zone as a matter of technique — so the reference driver
    // defines normal and this reports the gap to them.
    const double delta = reaction - referenceReaction;
    if (delta <= 200.0 || distance <= 5.0) return;
    notes.push_back({id(),
                     format("first downshift %.0fms later than reference "
                            "(%.0fm into braking)",
                            delta, distance),
                     NoteSeverity::Info});
}

OMATRACK_ANALYZER(DownshiftTimingAnalyzer, "downshift_timing", true) {
    const double primary = context.primaryMetrics.downshiftLastMs;
    const double reference = context.referenceMetrics.downshiftLastMs;
    if (!finite(primary) || !finite(reference)) return;
    const double delta = primary - reference;
    if (delta < 300.0) return;
    notes.push_back(
        {id(),
         format("last downshift %.1fs later than reference", delta / 1000.0),
         NoteSeverity::Info});
}

OMATRACK_ANALYZER(BrakeThrottleOverlapAnalyzer, "brake_throttle_overlap",
                  false) {
    const double seconds = context.primaryMetrics.brakeThrottleOverlapSeconds;
    if (seconds <= kBlipSeconds) return;
    notes.push_back({id(), format("throttle while braking (%.1fs)", seconds),
                     NoteSeverity::Error});
}

class CombinedGripAnalyzer : public CornerAnalyzer {
public:
    explicit CombinedGripAnalyzer(bool early) : early_(early) {}
    const char* id() const override {
        return early_ ? "combined_grip_early" : "combined_grip_mid";
    }
    void analyze(const CornerContext& context, Notes& notes) const override {
        const double primary = early_ ? context.primaryMetrics.combinedGripEarly
                                      : context.primaryMetrics.combinedGripMid;
        const double reference =
            early_ ? context.referenceMetrics.combinedGripEarly
                   : context.referenceMetrics.combinedGripMid;
        if (!finite(primary) || !finite(reference) || reference < 0.2) return;
        const double deficit = reference - primary;
        if (deficit < 0.15 || primary / reference >= 0.88) return;
        notes.push_back({id(),
                         format("%.2fg less combined grip %s corner", deficit,
                                early_ ? "early" : "mid"),
                         NoteSeverity::Info});
    }

private:
    bool early_;
};

#undef OMATRACK_ANALYZER

}  // namespace

// ── registry ────────────────────────────────────────────────────────

CornerAnalysisRegistry::CornerAnalysisRegistry() {
    // Registration order is display order: the cheap comparisons a driver
    // reads first, then technique, then mistakes.
    add(std::make_unique<EntrySpeedAnalyzer>());
    add(std::make_unique<TurnInAnalyzer>());
    add(std::make_unique<SteeringAnalyzer>());
    add(std::make_unique<GearAnalyzer>());
    add(std::make_unique<CoastingAnalyzer>());
    add(std::make_unique<BrakePressureAnalyzer>());
    add(std::make_unique<BrakeRateAnalyzer>());
    add(std::make_unique<TrailBrakingAnalyzer>());
    add(std::make_unique<ThrottleTimingAnalyzer>());
    add(std::make_unique<DownshiftReactionAnalyzer>());
    add(std::make_unique<DownshiftTimingAnalyzer>());
    add(std::make_unique<CombinedGripAnalyzer>(true));
    add(std::make_unique<CombinedGripAnalyzer>(false));
    add(std::make_unique<BrakeThrottleOverlapAnalyzer>());
}

CornerAnalysisRegistry& CornerAnalysisRegistry::instance() {
    static CornerAnalysisRegistry registry;
    return registry;
}

void CornerAnalysisRegistry::add(std::unique_ptr<CornerAnalyzer> analyzer) {
    if (analyzer) analyzers_.push_back(std::move(analyzer));
}

void CornerAnalysisRegistry::clear() { analyzers_.clear(); }

std::vector<CornerNote> CornerAnalysisRegistry::run(
    const CornerContext& context) const {
    std::vector<CornerNote> notes;
    if (!context.primaryMetrics.valid) return notes;
    notes.reserve(analyzers_.size());
    const bool comparing = context.comparing();
    for (const std::unique_ptr<CornerAnalyzer>& analyzer : analyzers_) {
        if (analyzer->requiresReference() && !comparing) continue;
        analyzer->analyze(context, notes);
    }
    return notes;
}

std::vector<CornerNote> analyzeCorner(const CornerContext& context) {
    return CornerAnalysisRegistry::instance().run(context);
}

}  // namespace omatrack
