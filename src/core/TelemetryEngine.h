// Omatrack core telemetry engine.
//
// Parsing is delegated to the vendored Rust crates from
// duckdb_motorsport_telemetry via the C ABI bridge (`omatrack_*` functions).
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
    int id = 0;
    double startTime = 0.0;  // seconds from session start
    double endTime = 0.0;
    double timeMs = 0.0;
    /// True when both bounds are real start/finish crossings. A leading
    /// (out) or trailing (in) fragment of the recording is not a lap.
    bool complete = true;
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

// ── telemtery source ────────────────────────────────────────────────

class TelemetrySource {
public:
    static std::unique_ptr<TelemetrySource> open(const std::string& path,
                                                 std::string* error = nullptr);
    ~TelemetrySource();

    const std::string& path() const { return path_; }
    const std::string& formatName() const { return format_; }
    double mediaTimeOffsetSec() const { return mediaTimeOffsetSec_; }

    std::vector<RawChannel>& channels() { return channels_; }
    const std::vector<RawChannel>& channels() const { return channels_; }

    /// Sample a channel at absolute time (seconds) with linear interpolation.
    /// Returns false when out of range.
    bool sampleAt(size_t channelIdx, double timeSec, double* out) const;

    /// Map channel concepts to channel indices (omatrack channelMappings).
    std::map<std::string, int> mapChannels() const;

    /// Detect laps using the PDS beacon/splits heuristics.
    std::vector<Lap> detectLaps() const;

    /// Dominant positive driver id from a DRIVER_ID-style channel; 0 if absent.
    int detectDriverId() const;

    /// Build a 50 Hz UnifiedLap over [startTime, endTime].
    UnifiedLap unifyLap(double startTime, double endTime) const;

    // Public so tests can populate channels_ with synthetic data without
    // going through the Rust bridge. Production code uses open().
    TelemetrySource() = default;

private:
    void* handle_ = nullptr;
    std::string path_;
    std::string format_;
    double mediaTimeOffsetSec_ = 0.0;
    std::vector<RawChannel> channels_;
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
                                       int* driverId = nullptr);

/// Port of MoTecParser.resample (srcFreq -> targetFreq, linear).
std::vector<double> resample(const std::vector<double>& values, double srcFreq,
                             double targetFreq, double duration);

}  // namespace omatrack
