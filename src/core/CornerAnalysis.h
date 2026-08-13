// Corner analysis: per-lap corner metrics and the pluggable checks that turn
// a primary/reference pair into driver-facing notes.
//
// Ported from the corner analysis in tobi/ac-tracer
// (lib/windows/corner_analysis.lua), which is the reference implementation for
// what a corner comparison should say. Kept Qt-free so `omatrack-cli` and the
// unit tests exercise the same code the GUI does.
//
// Shape of the pipeline:
//
//   measureCorner(lap, start, end)  ->  CornerMetrics   (one pass per lap)
//   CornerContext{primary, reference metrics + time deltas}
//   CornerAnalysisRegistry::run(context)  ->  std::vector<CornerNote>
//
// Efficiency is a contract, not an aspiration. Every scan of the sample arrays
// happens once, in measureCorner(); an analyzer only reads scalars out of
// CornerMetrics and formats a short string, so adding checks costs nothing per
// frame. Analyzers never touch the raw channel arrays, never allocate
// per-sample, and are pure functions of the context — the store caches the
// resulting notes until the selection, the corner list or the alignment
// changes.
//
// Adding a check: subclass CornerAnalyzer, give it a stable `id()`, and
// register it in registerBuiltinAnalyzers() (or through
// CornerAnalysisRegistry::add() from another translation unit). Nothing else
// in the app needs to change: the store renders whatever notes come back.

#pragma once

#include <memory>
#include <string>
#include <vector>

namespace omatrack {

struct UnifiedLap;

enum class NoteSeverity { Info, Warning, Error };

/// One driver-facing observation about a corner.
struct CornerNote {
    std::string id;  ///< stable analyzer id, for tests and filtering
    std::string text;
    NoteSeverity severity = NoteSeverity::Info;
};

/// Everything the checks need to know about one lap through one corner.
/// Distances are metres from the corner's start; times are seconds; speeds
/// km/h; brake pressure bar; steering degrees. A value that could not be
/// determined is NaN (or -1 for indices), never a silent zero.
struct CornerMetrics {
    bool valid = false;

    int firstIndex = -1;
    int lastIndex = -1;
    int apexIndex = -1;
    int turnInIndex = -1;
    int brakeIndex = -1;
    int liftIndex = -1;
    int throttleIndex = -1;

    double startDistance = 0.0;
    double lengthMeters = 0.0;
    double time = 0.0;

    double entrySpeed = 0.0;  ///< fastest before the apex
    double apexSpeed = 0.0;   ///< slowest in the zone
    double exitSpeed = 0.0;   ///< fastest after the apex

    double brakePoint = 0.0;
    double liftPoint = 0.0;
    double turnInPoint = 0.0;
    double apexPoint = 0.0;
    double throttlePoint = 0.0;
    double coastMeters = 0.0;  ///< lift-off to brake application

    double maxSteering = 0.0;
    double maxBrake = 0.0;
    double minThrottle = 1.0;
    int minGear = 0;

    /// Brake pressure ramp from 10% to 90% of this corner's peak, bar/s.
    double brakeRiseRate = 0.0;
    /// Time from peak brake pressure to release.
    double trailBrakeSeconds = 0.0;
    /// Longest sustained throttle application while braking hard.
    double brakeThrottleOverlapSeconds = 0.0;

    /// Brake application to first/last downshift.
    double downshiftFirstMs = 0.0;
    double downshiftLastMs = 0.0;
    double downshiftDistance = 0.0;

    /// Combined lateral+braking acceleration, averaged over turn-in→mid and
    /// mid→apex. NaN when the lap has no lateral-G channel.
    double combinedGripEarly = 0.0;
    double combinedGripMid = 0.0;
    double peakLateralG = 0.0;
    bool hasLateralG = false;
};

/// Inputs to one corner's checks. `reference` is null for a single lap.
struct CornerContext {
    const UnifiedLap* primary = nullptr;
    const UnifiedLap* reference = nullptr;
    CornerMetrics primaryMetrics;
    CornerMetrics referenceMetrics;
    /// Time lost/gained across the corner, from the caller's cached delta
    /// trace so every surface agrees. Positive means the primary is slower.
    double timeDelta = 0.0;
    double entryTimeDelta = 0.0;
    double exitTimeDelta = 0.0;

    bool comparing() const {
        return reference != nullptr && referenceMetrics.valid;
    }
};

/// One check. Stateless, cheap, and free to say nothing.
class CornerAnalyzer {
public:
    virtual ~CornerAnalyzer() = default;
    virtual const char* id() const = 0;
    /// Checks that only describe the primary lap return false and still run
    /// without a reference.
    virtual bool requiresReference() const { return true; }
    virtual void analyze(const CornerContext& context,
                         std::vector<CornerNote>& notes) const = 0;
};

class CornerAnalysisRegistry {
public:
    static CornerAnalysisRegistry& instance();

    void add(std::unique_ptr<CornerAnalyzer> analyzer);
    void clear();
    const std::vector<std::unique_ptr<CornerAnalyzer>>& analyzers() const {
        return analyzers_;
    }

    /// Runs every registered analyzer in registration order.
    std::vector<CornerNote> run(const CornerContext& context) const;

private:
    CornerAnalysisRegistry();
    std::vector<std::unique_ptr<CornerAnalyzer>> analyzers_;
};

/// Measures one corner of one lap. `startFraction`/`endFraction` are lap
/// fractions. The caller maps the reference zone through the same
/// primary→reference track-station map used by traces and delta.
CornerMetrics measureCorner(const UnifiedLap& lap, double startFraction,
                            double endFraction);

/// Convenience: measure both laps and run the registry.
std::vector<CornerNote> analyzeCorner(const CornerContext& context);

const char* severityName(NoteSeverity severity);

}  // namespace omatrack
