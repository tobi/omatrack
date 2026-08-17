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

#include <algorithm>
#include <array>
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
    /// First-sample time, file-relative nanoseconds (`t0`).
    std::uint64_t startNs = 0;
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
    /// Presentation-order frame at the lap start from source metadata.
    std::optional<std::uint64_t> firstVideoFrame;
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
    std::vector<double>
        gForceLat;  // lateral acceleration (same units as gForceLong)
    std::vector<double> damperFL, damperFR, damperRL, damperRR;
    std::vector<double> gpsLat, gpsLon;
    std::vector<double> gpsPositionAccuracy, gpsSpeedAccuracy;
    std::vector<double> fuel;  // litres remaining; NaN when unmapped
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

/// Right-aligned MTX group header chrome (`r` in the sidecar header).
struct SidecarChrome {
    enum class Kind { Text, Pill };
    Kind kind = Kind::Text;
    std::string text;
    std::string label;
    std::string value;
};

/// One MTX span (`k:"s"`): `[startNs, endNs)` on the sidecar axis.
struct SidecarSpan {
    std::uint64_t startNs = 0;
    std::uint64_t endNs = 0;
    bool visible = true;
    std::string name;
    std::string title;
    std::string subtitle;
    std::string color;
    std::vector<std::pair<std::string, std::string>> meta;
};

/// One video file linked to a telemetry recording.
struct VideoFileReference {
    std::string filename;
    std::uint32_t index = 0;
    std::optional<std::array<std::uint8_t, 32>> blake3;
    std::uint64_t frameCount = 0;
    std::optional<std::int64_t> presentationOffsetNs;
};

/// Exact telemetry-to-player mapping copied from the recording before the
/// decoded source arrays are released.
struct VideoClock {
    std::optional<std::int64_t> presentationOffsetNs;
    std::vector<std::uint64_t> presentationTimesNs;
    std::vector<VideoFileReference> files;

    bool valid() const {
        return !presentationTimesNs.empty() &&
               (presentationOffsetNs.has_value() ||
                std::any_of(files.cbegin(), files.cend(), [](const auto& file) {
                    return file.presentationOffsetNs.has_value();
                }));
    }
    std::optional<std::uint64_t> presentationTimeNs(
        std::uint64_t telemetryTimeNs,
        std::optional<std::uint32_t> fileIndex = std::nullopt) const;
    std::optional<std::uint64_t> telemetryTimeNs(
        std::uint64_t presentationTimeNs,
        std::optional<std::uint32_t> fileIndex = std::nullopt) const;
    std::optional<std::uint64_t> frameAt(std::uint64_t telemetryTimeNs) const;
};

/// Full BLAKE3-256 digest used to validate a linked video identity.
std::optional<std::array<std::uint8_t, 32>> blake3File(const std::string& path);

// ── telemtery source ────────────────────────────────────────────────

class TelemetrySource {
public:
    static std::unique_ptr<TelemetrySource> open(const std::string& path,
                                                 std::string* error = nullptr);
    /// Open a bounded metadata view for library indexing. AiM retains complete
    /// filmstrip lap signals but omits the video-frame index; normal `open()`
    /// remains the full analysis path.
    static std::unique_ptr<TelemetrySource> openIndex(
        const std::string& path, std::string* error = nullptr);
    ~TelemetrySource();

    const std::string& path() const { return path_; }
    const std::string& formatName() const { return format_; }
    const VideoClock& videoClock() const { return videoClock_; }
    /// Offset satisfying video presentation time = telemetry file-relative
    /// time + offset. Absent for sources without embedded-video timing.
    std::optional<double> videoPresentationOffsetSec() const;
    /// Exact player presentation time at file-relative telemetry time.
    std::optional<double> videoPresentationTime(double timeSec) const;
    /// Presentation-order video frame at file-relative telemetry time.
    std::optional<std::uint64_t> videoFrameAt(double timeSec) const;

    std::vector<RawChannel>& channels() { return channels_; }
    const std::vector<RawChannel>& channels() const { return channels_; }
    std::vector<Lap>& sourceLaps() { return sourceLaps_; }
    const std::vector<Lap>& sourceLaps() const { return sourceLaps_; }

    /// Unix-epoch nanoseconds at file `t = 0`, or -1 when the source has no
    /// placement stamp. Join with an MTX sidecar is
    /// `host_file_ns = ext_file_ns + ext.utc − host.utc`.
    std::int64_t utcStartNs() const { return utcStartNs_; }
    /// Exclusive file-relative duration in nanoseconds.
    std::uint64_t durationNs() const { return durationNs_; }
    const std::string& timezone() const { return timezone_; }
    /// True when this handle is an MTX sidecar, not a host recording.
    bool isExtension() const { return isExtension_; }
    const std::string& sidecarName() const { return sidecarName_; }
    /// Header `vis`: the overlay group starts expanded when true.
    bool groupVisible() const { return groupVisible_; }
    const std::vector<SidecarChrome>& sidecarChrome() const {
        return sidecarChrome_;
    }
    const std::vector<SidecarSpan>& spans() const { return spans_; }
    /// Per-channel default visibility from the MTX `vis` field.
    bool channelDefaultVisible(size_t index) const;

    /// Sample a channel at absolute time (seconds). Linear interpolation is
    /// the default; pass linear=false for ordinals such as gear.
    /// Returns false when out of range.
    bool sampleAt(size_t channelIdx, double timeSec, double* out,
                  bool linear = true) const;
    /// Sample a channel at file-relative nanoseconds. Prefer this for MTX
    /// join so the integer-ns key is not converted through a double.
    bool sampleAtNs(size_t channelIdx, std::uint64_t timeNs, double* out,
                    bool linear = true) const;

    /// Map channel concepts to channel indices (omatrack channelMappings).
    std::map<std::string, int> mapChannels(
        const ChannelOverrides& overrides = {}) const;

    /// Return reliable format-neutral laps, falling back to local
    /// beacon/counter/time/distance heuristics when needed.
    std::vector<Lap> detectLaps() const;

    /// Dominant positive driver code from the mapped numeric channel; 0 if
    /// absent.
    double detectDriverId(const ChannelOverrides& overrides = {}) const;

    /// Build a 50 Hz UnifiedLap over [startTime, endTime].
    UnifiedLap unifyLap(double startTime, double endTime,
                        const ChannelOverrides& overrides = {}) const;

    /// Serialise this source to a native `.telemetry` recording. Requires a
    /// live parser handle (`open()`, not a synthetic source).
    bool writeTelemetry(const std::string& path, std::string* error = nullptr,
                        const std::string& linkedVideoFilename = {}) const;

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
    VideoClock videoClock_;
    std::vector<RawChannel> channels_;
    std::vector<Lap> sourceLaps_;
    std::int64_t utcStartNs_ = -1;
    std::uint64_t durationNs_ = 0;
    std::string timezone_;
    bool isExtension_ = false;
    bool groupVisible_ = true;
    std::string sidecarName_;
    std::vector<SidecarChrome> sidecarChrome_;
    std::vector<SidecarSpan> spans_;
    std::vector<std::uint8_t> channelVisible_;
};

/// True when `path` is an MTJ/MTX JSONL document (plain or zstd).
bool isJsonlPath(const std::string& path);
/// True when `path` names an MTX sidecar (`.ext.jsonl` / `.mtx.jsonl`).
bool isJsonlExtPath(const std::string& path);
/// Unix-epoch nanoseconds at file `t = 0` from a GPS week / iTOW sample
/// taken at `fileTimeSec`. Returns -1 when the fix is unusable.
std::int64_t utcStartNsFromGps(double week, double itowMs, double fileTimeSec);
/// `ext.utc − host.utc`, or 0 when the host has no utc (`hostUtcNs < 0`).
std::int64_t sidecarJoinShiftNs(std::int64_t hostUtcNs, std::int64_t extUtcNs);
/// Half-open `[a0, a1)` overlaps `[b0, b1)`.
bool nsRangesOverlap(std::int64_t a0, std::int64_t a1, std::int64_t b0,
                     std::int64_t b1);

// ── helpers exposed for CLI/tests ───────────────────────────────────

/// Catalog-only `.telemetry` meaning this video has no usable telemetry.
bool writeUnsupportedTelemetry(const std::string& path,
                               std::string* error = nullptr);

/// Header-only: the `.telemetry` stores a presentation offset and video
/// frames. A v1 companion without `video_frames.bin` is false.
bool telemetryHasVideoClock(const std::string& path);

/// Side-by-side dump of GPS, main channels, laps, and video-frame sync.
/// `left` is typically the AiM extract; `right` is the native `.telemetry`.
std::string compareTelemetrySources(
    const TelemetrySource& left, const TelemetrySource& right,
    const std::string& leftLabel = "aimd",
    const std::string& rightLabel = "telemetry");

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
