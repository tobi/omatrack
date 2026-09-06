// origin: PUBLIC — data contracts only; no recording/model content.
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace omatrack::inference {

inline constexpr std::int64_t ImageTelemetryPeriodNs = 200'000'000;
inline constexpr std::int64_t ImageTelemetryMaxDurationNs =
    86'400'000'000'000;  // 24 hours, bounded allocation
inline constexpr const char* ImageTelemetrySchemaRevision = "image-series-v1";
inline constexpr const char* ImageTelemetryLayoutRevision =
    "orange-source-1920x1080-structure-v1";
inline constexpr const char* ImageTelemetryDecoderRevision =
    "rgb24-presentation-origin-v1";
inline constexpr std::array<const char*, 4> ImageTelemetryChannelNames{
    "image_derived_gear", "image_derived_stint_lap",
    "image_derived_brake_visible_fill_pct",
    "image_derived_throttle_visible_fill_pct"};

enum class ImageTelemetryField : std::size_t {
    Gear,
    StintLap,
    BrakeFillPct,
    ThrottleFillPct
};

struct ImageTelemetryFileIdentity {
    std::string
        canonicalPath;  // private runtime metadata, never a public fixture
    std::uint64_t device = 0;
    std::uint64_t inode = 0;
    std::int64_t size = -1;
    std::int64_t mtimeNs = 0;
    std::int64_t changeNs = 0;
};

struct ImageTelemetryIdentity {
    ImageTelemetryFileIdentity source;
    ImageTelemetryFileIdentity model;
    std::string modelSha256;
    std::string schemaRevision = ImageTelemetrySchemaRevision;
    std::string layoutRevision = ImageTelemetryLayoutRevision;
    std::string decoderRevision = ImageTelemetryDecoderRevision;
    // Set ONLY after the independent decoder/native-source veto has established
    // absence. A parser failure does not establish absence. Native samples are
    // never reader inputs, never copied into these channels.
    bool nativeTelemetryAbsent = false;
};

struct ImageTelemetrySlot {
    bool visited = false;
    bool layoutSupported =
        false;  // recognized structure, independent of per-field readability
    // Actual decoded frame time, NOT the lattice target, mpv time-pos, or an
    // ordinal/FPS-derived time. Unsupported frames can retain their actual PTS.
    std::optional<std::int64_t> presentationPtsNs;
    std::optional<std::int64_t> sourcePtsNs;
    std::array<std::optional<double>, 4> values;
};

// Mutate only a worker-owned draft. Publish shared_ptr<const
// ImageTelemetrySeries> and never mutate it again; the renderer can retain that
// snapshot safely.
struct ImageTelemetrySeries {
    ImageTelemetryIdentity identity;
    std::int64_t durationNs =
        0;  // exact source presentation duration (not rounded)
    std::int64_t timelineOriginNs =
        0;  // sourcePtsNs = presentationPtsNs + origin
    std::uint64_t revision =
        0;  // monotonically increasing snapshot revision, caller owned
    std::vector<ImageTelemetrySlot> cells;

    static std::size_t slotCount(std::int64_t duration) {
        if (duration <= 0 || duration > ImageTelemetryMaxDurationNs) return 0;
        return std::size_t((duration + ImageTelemetryPeriodNs - 1) /
                           ImageTelemetryPeriodNs);
    }
    std::size_t visitedCount() const {
        return std::count_if(cells.begin(), cells.end(),
                             [](const auto& slot) { return slot.visited; });
    }
    bool complete() const {
        return !cells.empty() && visitedCount() == cells.size();
    }
    // O(1) half-open slot range intersecting [start,end). The caller may
    // include one neighbour for clipping. Never connect across an
    // unvisited/unknown slot.
    std::pair<std::size_t, std::size_t> slotRange(std::int64_t start,
                                                  std::int64_t end) const {
        start = std::clamp(start, std::int64_t(0),
                           std::max(std::int64_t(0), durationNs));
        end = std::clamp(end, start, std::max(start, durationNs));
        const auto begin =
            std::min(cells.size(), std::size_t(start / ImageTelemetryPeriodNs));
        const auto finish = std::min(
            cells.size(),
            std::size_t(end == 0 ? 0 : 1 + (end - 1) / ImageTelemetryPeriodNs));
        return {begin, end == start ? begin : finish};
    }
};
using ImageTelemetrySnapshot = std::shared_ptr<const ImageTelemetrySeries>;

// Slot i represents lattice target i*200ms. Only a decoded frame inside
// [target,min(target+200ms,duration)) may occupy it. Frames beyond that
// interval do NOT fill earlier gaps: proven gaps/EOF may be visited/unknown.
// Transient decode/inference errors and cancellation remain
// unvisited/retryable; they must not make a cache appear complete. A known
// value requires visited + layoutSupported + both timestamps. sourcePts must
// equal presentationPts+timelineOrigin exactly. Unknown and unvisited are
// distinct. Cache MTJ uses regular targets for its sample axis, an explicit
// actual-PTS channel, visited/known mask channels and null gaps. Raw source PTS
// reconstructs losslessly from the integer origin in header pass provenance +
// actual PTS; it is not sent through a floating-point channel where large
// clocks lose bits.

}  // namespace omatrack::inference
