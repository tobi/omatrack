// Omatrack core telemetry engine.
//
// Parsing is delegated to the pinned motorsport-telemetry-rs facade through
// the C ABI bridge (`omatrack_*` functions).
// This file ports Omatrack's analysis layer (channel mapping, lap detection,
// 50 Hz UnifiedLap unification) from MoTecParser.swift on top of the raw
// channels the bridge exposes.
//
// Qt-free on purpose: the CLI and the Qt app both link this core, and the
// core is unit-testable headless.

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace omatrack {

// ── raw channel (as decoded from the file) ─────────────────────────

struct RawChannel {
    std::string name;
    std::string unit;
    uint32_t sampleTypeCode = 0;
    /// Samples in physical units, decoded across all chunks in time order.
    std::vector<double> samples;
    /// Sampling frequency in Hz (from first chunk period).
    double frequencyHz = 0.0;
    /// Total duration in seconds.
    double durationSec = 0.0;
};

// ── lap ─────────────────────────────────────────────────────────────

struct Lap {
    Lap() = default;
    Lap(int lapId, double start, double end, double milliseconds,
        bool isComplete, std::optional<int> originalNumber = std::nullopt)
        : id(lapId),
          startTime(start),
          endTime(end),
          timeMs(milliseconds),
          complete(isComplete),
          sourceNumber(originalNumber) {}

    int id = 0;
    double startTime = 0.0;  // seconds from session start
    double endTime = 0.0;
    double timeMs = 0.0;
    /// True when both bounds are real start/finish crossings. A leading
    /// (out) or trailing (in) fragment of the recording is not a lap.
    bool complete = true;
    /// Original source lap number when the parser supplied a unique value.
    std::optional<int> sourceNumber;
};

// ── unified 50 Hz lap ───────────────────────────────────────────────

enum class DistanceSource {
    Native,
    SpeedFused,
};

struct UnifiedLap {
    int sampleRate = 50;
    DistanceSource distanceSource = DistanceSource::SpeedFused;
    std::vector<double> time;
    std::vector<double> speed;           // km/h
    std::vector<double> throttle;        // 0-1
    std::vector<double> driverThrottle;  // 0-1 (pre-TC)
    std::vector<double> brake;           // bar (or pos*100)
    std::vector<double> clutch;          // 0-1
    std::vector<double> steering;        // deg
    std::vector<int> gear;
    std::vector<double> distance;  // m
    std::vector<double> gForceLong;
    std::vector<double> damperFL, damperFR, damperRL, damperRR;
    std::vector<double> gpsLat, gpsLon;
    std::vector<double> gpsPositionAccuracy, gpsSpeedAccuracy;
    size_t size() const { return time.size(); }
};

// ── session metadata ────────────────────────────────────────────────

struct SessionMeta {
    std::string date;
    std::string time;
    std::string driverName;
    std::string driverTag;
    std::string vehicleId;
    std::string venue;
    std::string eventName;
};

/// User-selected canonical concept -> source channel name overrides. Missing
/// concepts keep the normal cross-format alias matching.
using ChannelOverrides = std::map<std::string, std::string>;

// ── telemtery source ────────────────────────────────────────────────

class TelemetrySource {
public:
    static std::unique_ptr<TelemetrySource> open(const std::string& path,
                                                 std::string* error = nullptr);
    /// Open a bounded metadata view for library indexing. AiM video lap and
    /// frame indexes are deliberately omitted; normal `open()` remains exact.
    static std::unique_ptr<TelemetrySource> openIndex(
        const std::string& path, std::string* error = nullptr);
    ~TelemetrySource();

    const std::string& path() const { return path_; }
    const std::string& formatName() const { return format_; }
    /// Offset satisfying video presentation time = telemetry file-relative
    /// time + offset. Absent for sources without embedded-video timing.
    std::optional<double> videoPresentationOffsetSec() const {
        return videoPresentationOffsetSec_;
    }

    std::vector<RawChannel>& channels() { return channels_; }
    const std::vector<RawChannel>& channels() const { return channels_; }
    std::vector<Lap>& sourceLaps() { return sourceLaps_; }
    const std::vector<Lap>& sourceLaps() const { return sourceLaps_; }

    /// Sample a channel at absolute time (seconds) with linear interpolation.
    /// Returns false when out of range.
    bool sampleAt(size_t channelIdx, double timeSec, double* out) const;

    /// Map channel concepts to channel indices (omatrack channelMappings).
    std::map<std::string, int> mapChannels(
        const ChannelOverrides& overrides = {}) const;

    /// Return authoritative source-provided laps, falling back to channel
    /// beacon/counter/time/distance heuristics when the parser has none.
    std::vector<Lap> detectLaps() const;

    /// Dominant positive driver code from the mapped numeric channel; 0 if
    /// absent.
    double detectDriverId(const ChannelOverrides& overrides = {}) const;

    /// Build a 50 Hz UnifiedLap over [startTime, endTime].
    UnifiedLap unifyLap(double startTime, double endTime,
                        const ChannelOverrides& overrides = {}) const;

    // Public so tests can populate channels_ with synthetic data without
    // going through the Rust bridge. Production code uses open().
    TelemetrySource() = default;

private:
    static std::unique_ptr<TelemetrySource> openImpl(const std::string& path,
                                                     bool indexOnly,
                                                     std::string* error);
    void* handle_ = nullptr;
    std::string path_;
    std::string format_;
    std::optional<double> videoPresentationOffsetSec_;
    std::vector<RawChannel> channels_;
    std::vector<Lap> sourceLaps_;
};

// ── helpers exposed for CLI/tests ───────────────────────────────────

/// Normalize a channel name: lowercase + strip non-alphanumeric.
std::string normalizeChannelName(const std::string& raw);

/// Session metadata extracted from a PDS-style filename.
SessionMeta sessionMetaFromFilename(const std::string& stem);

/// Format a lap time in ms as "M:SS.mmm".
std::string formatLapTime(double timeMs);

/// Decode only sidebar metadata channels without loading full telemetry.
std::vector<Lap> detectLapsLightweight(const std::string& path,
                                       double* driverId = nullptr);

/// Port of MoTecParser.resample (srcFreq -> targetFreq, linear).
std::vector<double> resample(const std::vector<double>& values, double srcFreq,
                             double targetFreq, double duration);

}  // namespace omatrack
