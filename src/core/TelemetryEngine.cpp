// Implementation of the Omatrack core telemetry engine.
//
// Ports the Omatrack analysis layer (MoTecParser.swift + TelemetryUtils)
// on top of the Rust parsing bridge. Parsing itself is delegated to the
// pinned motorsport-telemetry-rs crates.

#include "TelemetryEngineInternal.h"
#include "omatrack_bridge.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>

namespace omatrack {

namespace detail {

constexpr double kPi = 3.14159265358979323846;
constexpr int kDefaultSampleRate = 50;

// ── channel mapping (port of MoTecParser.channelMappings) ───────────

using AliasTable = std::map<std::string, std::vector<std::string>>;

const AliasTable& channelMappings() {
    static const AliasTable table = {
        {"speed",
         {"corr speed", "ground speed", "wheel speed avg", "aero speed",
          "speed_ref", "vehrefspeed", "speed_wspd_app", "uspeed", "speed"}},
        {"throttle",
         {"tpsreal", "tps", "throttle pos", "aps", "driver throttle pos",
          "accel pedal pos", "acc pedal pos", "fbwdrivertps", "pps"}},
        {"brake",
         {"brake pressure f", "brake pressure fr", "p_f_brake",
          "p_brake_front"}},
        {"clutch", {"clutch pos", "clutch position", "clutch pedal", "clutch"}},
        {"brake_pos", {"brake pos"}},
        {"steering", {"steering angle", "steer"}},
        {"gear", {"gear_pos", "gear", "gearposdisplay"}},
        {"driver_id",
         {"driverid", "driver_id", "driver id", "activedriverid",
          "x2lnk_driverid"}},
        {"driver_throttle", {"driver throttle pos", "fbwdrivertps", "pps"}},
        {"g_long", {"g force long", "i_accel_long", "fia_accelx"}},
        {"g_lat",
         {"g force lat", "g_force_lat", "i_accel_lat", "fia_accely",
          "accel_lat", "lateral acceleration", "latacc", "g lat"}},
        {"distance",
         {"lap distance corrected", "lap distance", "distance_wspd_app"}},
        {"damper_fl", {"x_fl_damper", "damper travel fl"}},
        {"damper_fr", {"x_fr_damper", "damper travel fr"}},
        {"damper_rl", {"x_rl_damper", "damper travel rl"}},
        {"damper_rr", {"x_rr_damper", "damper travel rr"}},
        {"gps_lat", {"fia_gpslatn", "gps latitude"}},
        {"gps_lon", {"fia_gpslonge", "gps longitude"}},
        {"gps_speed", {"fia_gpsvel", "gps speed"}},
        {"gps_position_accuracy", {"gps position accuracy"}},
        {"gps_speed_accuracy", {"gps speed accuracy"}},
    };
    return table;
}

const std::map<std::string, double>& speedUnits() {
    static const std::map<std::string, double> m = {
        {"m/s", 3.6}, {"km/h", 1.0}, {"kph", 1.0},
        {"kmh", 1.0}, {"kmph", 1.0}, {"mph", 1.60934}};
    return m;
}

const std::map<std::string, double>& brakeUnits() {
    static const std::map<std::string, double> m = {{"bar", 1.0},
                                                    {"psi", 0.0689476},
                                                    {"kpa", 0.01},
                                                    {"mpa", 10.0},
                                                    {"pa", 0.00001}};
    return m;
}

std::string lowerTrimmed(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    std::string out = s.substr(a, b - a);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return out;
}

// ── lap split helpers (port of MoTecParser.pds*Splits) ──────────────

std::vector<double> pdsBeaconSplits(const std::vector<double>& values,
                                    int freq) {
    std::vector<double> splits;
    if (freq <= 0 || values.empty()) return splits;
    bool inPulse = false;
    for (size_t i = 0; i < values.size(); ++i) {
        bool active = std::llround(values[i]) != 0;
        if (active && !inPulse) splits.push_back(double(i) / double(freq));
        inPulse = active;
    }
    return splits;
}

std::vector<double> pdsLapTimeSplits(const std::vector<double>& values,
                                     int freq) {
    std::vector<double> splits;
    if (freq <= 0 || values.size() < 2) return splits;
    int lastSplitIndex = -std::max(1, freq);
    int clusterGap = std::max(1, freq / 2);
    for (size_t i = 1; i < values.size(); ++i) {
        if (values[i - 1] - values[i] > 5 &&
            int(i) - lastSplitIndex >= clusterGap) {
            splits.push_back(double(i) / double(freq));
            lastSplitIndex = int(i);
        }
    }
    return splits;
}

std::vector<double> pdsLapNumberSplits(const std::vector<double>& values,
                                       int freq) {
    std::vector<double> splits;
    if (freq <= 0 || values.size() < 2) return splits;
    int prev = std::llround(values[0]);
    bool prevValid = prev > 0;
    for (size_t i = 1; i < values.size(); ++i) {
        int current = std::llround(values[i]);
        if (current <= 0) {
            prevValid = false;
            continue;
        }
        // Logger dropouts often appear as zero before the counter restarts.
        // Recovery establishes a new baseline; it is not a start/finish trip.
        if (prevValid && current == prev + 1)
            splits.push_back(double(i) / double(freq));
        prev = current;
        prevValid = true;
    }
    return splits;
}

bool lapNumberCarriesState(const std::vector<double>& values) {
    return std::any_of(values.begin(), values.end(), [](double value) {
        return std::isfinite(value) && std::llround(value) > 0;
    });
}

std::vector<double> selectLapSplits(const std::vector<double>& beaconSplits,
                                    const std::vector<double>& lapNumberSplits,
                                    bool lapNumberActive,
                                    const std::vector<double>& lapTimeSplits,
                                    const std::vector<double>& distanceSplits) {
    if (lapNumberActive && lapNumberSplits.size() >= 2) return lapNumberSplits;
    if (beaconSplits.size() >= 2) return beaconSplits;
    if (lapTimeSplits.size() >= 2) return lapTimeSplits;
    if (distanceSplits.size() >= 2) return distanceSplits;
    return {};
}

std::vector<double> pdsDistanceSplits(const std::vector<double>& values,
                                      int freq) {
    std::vector<double> splits;
    if (freq <= 0 || values.size() < 2) return splits;
    int lastSplitIndex = -std::max(1, freq);
    int clusterGap = std::max(1, freq / 2);
    for (size_t i = 1; i < values.size(); ++i) {
        if (values[i - 1] - values[i] > 300 &&
            int(i) - lastSplitIndex >= clusterGap) {
            splits.push_back(double(i) / double(freq));
            lastSplitIndex = int(i);
        }
    }
    return splits;
}

std::vector<Lap> buildLapsFromSplits(const std::vector<double>& splitTimesIn,
                                     double duration,
                                     bool rejectShortCrossings) {
    std::vector<Lap> result;
    if (duration <= 0) return result;
    std::set<double> filtered;
    for (double s : splitTimesIn)
        if (s > 0 && s < duration) filtered.insert(s);
    std::vector<double> splits(filtered.begin(), filtered.end());
    // Without two crossings the whole recording is one unbounded fragment.
    if (splits.size() < 2) {
        return {Lap{0, 0, duration, duration * 1000.0, /*complete=*/false}};
    }
    struct Bound {
        double start;
        double end;
        bool complete;
    };
    std::vector<Bound> lapBounds;
    for (size_t i = 0; i + 1 < splits.size(); ++i) {
        double a = splits[i], b = splits[i + 1];
        if (b - a > 10) lapBounds.push_back({a, b, true});
    }
    if (lapBounds.empty()) {
        return {Lap{0, 0, duration, duration * 1000.0, /*complete=*/false}};
    }
    std::vector<double> durations;
    for (auto& p : lapBounds) durations.push_back(p.end - p.start);
    std::sort(durations.begin(), durations.end());
    double median = durations[durations.size() / 2];
    // Head and tail fragments are bounded by the recording, not by a
    // crossing: keep them selectable but never treat them as timed laps.
    if (!splits.empty()) {
        double tail = duration - splits.back();
        if (tail > std::max(10.0, median * 0.5) && tail < median * 1.8)
            lapBounds.push_back({splits.back(), duration, false});
    }
    double head = splits.front();
    if (head > std::max(10.0, median * 0.5) && head < median * 1.8)
        lapBounds.insert(lapBounds.begin(), {0.0, head, false});
    for (size_t i = 0; i < lapBounds.size(); ++i) {
        result.push_back(Lap{int(i), lapBounds[i].start, lapBounds[i].end,
                             (lapBounds[i].end - lapBounds[i].start) * 1000.0,
                             lapBounds[i].complete});
    }
    if (rejectShortCrossings) markShortCrossingsIncomplete(result);
    return result;
}

void markShortCrossingsIncomplete(std::vector<Lap>& laps) {
    std::vector<double> seconds;
    for (const Lap& lap : laps) {
        if (!lap.complete || !std::isfinite(lap.timeMs) || !(lap.timeMs > 0.0))
            continue;
        seconds.push_back(lap.timeMs / 1000.0);
    }
    if (seconds.size() < 2) return;
    std::sort(seconds.begin(), seconds.end());
    const double median = seconds[seconds.size() / 2];
    // A crossing pair much shorter than the session median is a double
    // trigger or a recording fragment, not a racing lap. With only two
    // timed laps, fall back to a conservative floor: no circuit lap is
    // under half a minute.
    const double minSeconds =
        seconds.size() >= 3 ? median * 0.5 : std::max(median * 0.5, 30.0);
    for (Lap& lap : laps) {
        if (lap.complete && lap.timeMs / 1000.0 < minSeconds)
            lap.complete = false;
    }
}

std::vector<Lap> pdsApplyPreviousLapTimes(
    const std::vector<Lap>& laps,
    const std::vector<double>& previousLapTimeValues, int freq,
    bool rejectMismatches) {
    if (laps.empty() || previousLapTimeValues.empty() || freq <= 0) return laps;
    auto samplePrevLapTime = [&](double time) -> double {
        const int center = int(std::llround(time * double(freq)));
        auto seconds = [](double value) {
            if (!std::isfinite(value)) return -1.0;
            if (value > 1000.0 && value < 600000.0) value /= 1000.0;
            return value > 1.0 && value < 600.0 ? value : -1.0;
        };
        for (int delta = 0; delta <= 2; ++delta) {
            for (int sign : {-1, 1}) {
                const int idx = center + sign * delta;
                if (idx < 0 || idx >= int(previousLapTimeValues.size()))
                    continue;
                const double value = seconds(previousLapTimeValues[idx]);
                if (value > 0.0) return value;
            }
        }
        return -1.0;
    };
    std::vector<Lap> out = laps;
    for (auto& lap : out) {
        const double prevLapSec = samplePrevLapTime(lap.endTime);
        if (prevLapSec < 0.0) continue;
        const double crossingSec = lap.endTime - lap.startTime;
        const double tolerance = std::max(3.0, crossingSec * 0.15);
        if (std::fabs(prevLapSec - crossingSec) > tolerance) {
            if (rejectMismatches) lap.complete = false;
            continue;
        }
        lap.timeMs = prevLapSec * 1000.0;
    }
    return out;
}

std::vector<Lap> pdsApplyLapDistanceCoverage(
    const std::vector<Lap>& laps, const std::vector<double>& lapDistanceValues,
    int freq) {
    if (laps.empty() || lapDistanceValues.size() < 2 || freq <= 0) return laps;

    auto range = [&](size_t begin, size_t end) {
        double low = std::numeric_limits<double>::infinity();
        double high = -std::numeric_limits<double>::infinity();
        end = std::min(end, lapDistanceValues.size());
        for (size_t i = begin; i < end; ++i) {
            const double value = lapDistanceValues[i];
            if (!std::isfinite(value)) continue;
            low = std::min(low, value);
            high = std::max(high, value);
        }
        return high >= low ? high - low : 0.0;
    };

    const double sessionRange = range(0, lapDistanceValues.size());
    if (!(sessionRange > 0.0)) return laps;

    std::vector<double> coverage(laps.size(), -1.0);
    double maxCoverage = 0.0;
    for (size_t i = 0; i < laps.size(); ++i) {
        if (!laps[i].complete) continue;
        const size_t begin =
            size_t(std::max(0.0, std::floor(laps[i].startTime * double(freq))));
        const size_t end = size_t(
            std::max(0.0, std::ceil(laps[i].endTime * double(freq)) + 1.0));
        if (begin >= lapDistanceValues.size() || end > lapDistanceValues.size())
            continue;
        coverage[i] = range(begin, end) / sessionRange;
        maxCoverage = std::max(maxCoverage, coverage[i]);
    }

    // A session-cumulative distance channel cannot validate individual laps:
    // no one crossing pair spans most of its total range.
    if (maxCoverage < 0.75) return laps;

    std::vector<Lap> out = laps;
    for (size_t i = 0; i < out.size(); ++i) {
        if (out[i].complete && coverage[i] >= 0.0 &&
            coverage[i] < maxCoverage * 0.5)
            out[i].complete = false;
    }
    return out;
}

// ── channel mapping scoring (port of MoTecParser) ───────────────────

int scoreNormalizedChannelMatch(const std::string& channelName,
                                const std::string& alias, int aliasPriority) {
    if (channelName.empty() || alias.empty())
        return std::numeric_limits<int>::min();
    if (channelName == alias) return 10000 - aliasPriority;
    if (alias.size() >= 4 && channelName.find(alias) != std::string::npos)
        return 7000 - aliasPriority;
    if (channelName.size() >= 4 && alias.find(channelName) != std::string::npos)
        return 6000 - aliasPriority;
    return std::numeric_limits<int>::min();
}

int scoreChannelMatch(const std::string& channelName, const std::string& alias,
                      int aliasPriority) {
    return scoreNormalizedChannelMatch(normalizeChannelName(channelName),
                                       normalizeChannelName(alias),
                                       aliasPriority);
}

// Driver-ID channel aliases, shared by the loaded-source and lightweight paths.
const std::vector<std::string>& driverIdAliases() {
    static const std::vector<std::string> aliases{
        "DriverID", "driver_id",      "driver id",
        "driverid", "activeDriverId", "X2LNK_driverID"};
    return aliases;
}

/// Most frequent positive code in a driver-id series; ties go to the earlier
/// one.
double dominantDriverId(const std::vector<double>& values,
                        uint32_t sampleTypeCode) {
    std::map<double, std::pair<size_t, size_t>> counts;
    for (size_t index = 0; index < values.size(); ++index) {
        double candidate = values[index];
        if (!std::isfinite(candidate) || candidate <= 0.0) continue;
        if (sampleTypeCode == 6) {
            const long double exponent =
                std::floor(std::log10(std::fabs(candidate)));
            const long double scale = std::pow(10.0L, 6.0L - exponent);
            candidate =
                double(std::round(static_cast<long double>(candidate) * scale) /
                       scale);
        }
        auto& entry = counts[candidate];
        ++entry.first;
        if (entry.first == 1) entry.second = index;
    }
    double bestId = 0.0;
    size_t bestCount = 0;
    size_t bestFirst = std::numeric_limits<size_t>::max();
    for (const auto& [candidate, count] : counts) {
        if (count.first > bestCount ||
            (count.first == bestCount && count.second < bestFirst)) {
            bestId = candidate;
            bestCount = count.first;
            bestFirst = count.second;
        }
    }
    return bestId;
}

}  // namespace detail

using namespace detail;

namespace {

std::vector<Lap> sourceLapsFromBridge(void* handle) {
    std::vector<OmatrackSourceLap> rawLaps;
    const size_t count = omatrack_source_lap_count(handle);
    rawLaps.reserve(count);
    for (size_t index = 0; index < count; ++index) {
        OmatrackSourceLap raw{};
        if (!omatrack_source_lap(handle, index, &raw) ||
            raw.end_ns <= raw.start_ns)
            continue;
        rawLaps.push_back(raw);
    }

    std::vector<Lap> laps;
    laps.reserve(rawLaps.size());
    std::set<int> usedIds;
    int nextFallbackId = 0;
    for (const OmatrackSourceLap& raw : rawLaps) {
        int id = 0;
        const bool numberFits = raw.number >= std::numeric_limits<int>::min() &&
                                raw.number <= std::numeric_limits<int>::max();
        const bool preservesSourceNumber =
            numberFits && !usedIds.count(int(raw.number));
        if (preservesSourceNumber) {
            id = int(raw.number);
        } else {
            while (usedIds.count(nextFallbackId)) ++nextFallbackId;
            id = nextFallbackId++;
        }
        usedIds.insert(id);

        const uint64_t durationNs =
            raw.duration_ns > 0 ? raw.duration_ns : raw.end_ns - raw.start_ns;
        laps.push_back(
            Lap{id, double(raw.start_ns) / 1e9, double(raw.end_ns) / 1e9,
                double(durationNs) / 1e6, raw.complete != 0,
                preservesSourceNumber ? std::optional<int>(id) : std::nullopt});
    }
    return laps;
}

}  // namespace

// ── public helpers ──────────────────────────────────────────────────

std::string normalizeChannelName(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (char c : raw) {
        unsigned char uc = (unsigned char)c;
        if (std::isalnum(uc)) out.push_back((char)std::tolower(uc));
    }
    return out;
}

std::string formatLapTime(double timeMs) {
    int mins = int(timeMs / 60000);
    double secs = std::fmod(timeMs, 60000.0) / 1000.0;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%d:%06.3f", mins, secs);
    return buf;
}

SessionMeta sessionMetaFromFilename(const std::string& stem) {
    SessionMeta meta;
    // YYMMDDHHMMSS prefix
    if (stem.size() >= 12) {
        auto isDigit = [](char c) { return c >= '0' && c <= '9'; };
        std::string token;
        for (char c : stem) {
            if (isDigit(c) && token.size() < 12)
                token.push_back(c);
            else
                break;
        }
        if (token.size() == 12) {
            int yy = std::stoi(token.substr(0, 2));
            int mm = std::stoi(token.substr(2, 2));
            int dd = std::stoi(token.substr(4, 2));
            int hh = std::stoi(token.substr(6, 2));
            int mi = std::stoi(token.substr(8, 2));
            int ss = std::stoi(token.substr(10, 2));
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%02d/%02d/%04d", dd, mm,
                          2000 + yy);
            meta.date = buf;
            std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", hh, mi, ss);
            meta.time = buf;
        }
    }
    meta.eventName = stem;
    return meta;
}

// ── TelemetrySource ─────────────────────────────────────────────────

std::unique_ptr<TelemetrySource> TelemetrySource::open(const std::string& path,
                                                       std::string* error) {
    return openImpl(path, false, error);
}

std::unique_ptr<TelemetrySource> TelemetrySource::openIndex(
    const std::string& path, std::string* error) {
    return openImpl(path, true, error);
}

std::unique_ptr<TelemetrySource> TelemetrySource::openImpl(
    const std::string& path, bool indexOnly, std::string* error) {
    void* handle = indexOnly ? omatrack_open_index(path.c_str())
                             : omatrack_open(path.c_str());
    if (!handle) {
        if (error) {
            const char* message = omatrack_last_error();
            *error = message ? message : "Unknown telemetry parser error";
        }
        return nullptr;
    }
    std::unique_ptr<TelemetrySource> src(new TelemetrySource());
    src->handle_ = handle;
    src->path_ = path;
    const char* fmt = omatrack_format(handle);
    src->format_ = fmt ? fmt : "";
    src->sourceLaps_ = sourceLapsFromBridge(handle);
    int64_t videoPresentationOffsetNs = 0;
    if (omatrack_video_presentation_offset_ns(handle,
                                              &videoPresentationOffsetNs)) {
        src->videoPresentationOffsetSec_ =
            double(videoPresentationOffsetNs) / 1e9;
    }
    size_t n = omatrack_channel_count(handle);
    src->channels_.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        RawChannel ch;
        const char* name = omatrack_channel_name(handle, i);
        const char* unit = omatrack_channel_unit(handle, i);
        ch.name = name ? name : "";
        ch.unit = unit ? unit : "";
        ch.sampleTypeCode = omatrack_channel_type_code(handle, i);
        ch.durationSec = double(omatrack_channel_duration_ns(handle, i)) / 1e9;
        uint64_t period = omatrack_chunk_period_ns(handle, i, 0);
        ch.frequencyHz = period > 0 ? 1e9 / double(period) : 0.0;
        uint64_t count = omatrack_channel_sample_count(handle, i);
        ch.samples.resize((size_t)count);
        if (count > 0) {
            size_t written = omatrack_channel_decode_all(
                handle, i, ch.samples.data(), (size_t)count);
            ch.samples.resize(written);
        }
        src->channels_.push_back(std::move(ch));
    }
    return src;
}

TelemetrySource::~TelemetrySource() {
    if (handle_) omatrack_close(handle_);
}

bool TelemetrySource::writeTelemetry(const std::string& path,
                                     std::string* error) const {
    if (error) error->clear();
    if (!handle_) {
        if (error)
            *error = "Native .telemetry export requires an opened source";
        return false;
    }
    if (path.empty()) {
        if (error) *error = "Native .telemetry export path is empty";
        return false;
    }
    if (omatrack_write_telemetry(handle_, path.c_str()) != 0) return true;
    if (error) {
        const char* message = omatrack_last_error();
        *error = message && message[0] ? message
                                       : "Unable to write .telemetry recording";
    }
    return false;
}

bool writeUnsupportedTelemetry(const std::string& path, std::string* error) {
    if (error) error->clear();
    if (path.empty()) {
        if (error) *error = "Native .telemetry export path is empty";
        return false;
    }
    if (omatrack_write_unsupported_telemetry(path.c_str()) != 0) return true;
    if (error) {
        const char* message = omatrack_last_error();
        *error = message && message[0] ? message
                                       : "Unable to write .telemetry recording";
    }
    return false;
}

bool telemetryHasVideoClock(const std::string& path) {
    return !path.empty() &&
           omatrack_telemetry_has_video_clock(path.c_str()) != 0;
}

std::optional<std::uint64_t> TelemetrySource::videoFrameAt(
    double timeSec) const {
    if (!handle_ || !std::isfinite(timeSec) || timeSec < 0.0)
        return std::nullopt;
    const uint64_t timeNs = uint64_t(std::llround(timeSec * 1e9));
    uint64_t frame = 0;
    if (!omatrack_video_frame_at(handle_, timeNs, &frame)) return std::nullopt;
    return frame;
}

namespace {

double sourceDuration(const TelemetrySource& source) {
    double duration = 0.0;
    for (const RawChannel& channel : source.channels())
        duration = std::max(duration, channel.durationSec);
    for (const Lap& lap : source.sourceLaps())
        duration = std::max(duration, lap.endTime);
    return duration;
}

const RawChannel* mappedChannel(const TelemetrySource& source,
                                const std::map<std::string, int>& mapping,
                                const std::string& conceptName) {
    const auto it = mapping.find(conceptName);
    if (it == mapping.end() || it->second < 0 ||
        it->second >= int(source.channels().size()))
        return nullptr;
    return &source.channels()[size_t(it->second)];
}

void formatOptional(std::ostringstream& out, const std::optional<double>& value,
                    int precision) {
    if (!value || !std::isfinite(*value)) {
        out << "-";
        return;
    }
    out << std::fixed << std::setprecision(precision) << *value;
}

std::string sampleText(const TelemetrySource& source, int channelIndex,
                       double timeSec, bool linear) {
    if (channelIndex < 0) return "-";
    double value = 0.0;
    if (!source.sampleAt(size_t(channelIndex), timeSec, &value, linear))
        return "-";
    if (!std::isfinite(value)) return "nan";
    std::ostringstream out;
    out << std::fixed << std::setprecision(6) << value;
    return out.str();
}

void appendRawAnchors(std::ostringstream& out, const RawChannel* channel) {
    if (!channel || channel->samples.empty()) {
        out << "    raw: -\n";
        return;
    }
    const auto finiteAt = [&](size_t start, int step) -> std::string {
        for (size_t i = start; i < channel->samples.size(); i += size_t(step)) {
            if (std::isfinite(channel->samples[i])) {
                std::ostringstream value;
                value << std::fixed << std::setprecision(6)
                      << channel->samples[i];
                return value.str();
            }
        }
        return "nan";
    };
    const size_t last = channel->samples.size() - 1;
    const size_t mid = last / 2;
    out << "    raw[" << 0 << "]=" << finiteAt(0, 1) << "  [" << mid
        << "]=" << finiteAt(mid, 1) << "  [" << last
        << "]=" << finiteAt(last, -1) << "\n";
}

}  // namespace

std::string compareTelemetrySources(const TelemetrySource& left,
                                    const TelemetrySource& right,
                                    const std::string& leftLabel,
                                    const std::string& rightLabel) {
    static const char* kConcepts[] = {
        "speed", "throttle", "driver_throttle", "brake",   "steering",
        "gear",  "distance", "gps_lat",         "gps_lon", "gps_speed"};
    const auto leftMap = left.mapChannels();
    const auto rightMap = right.mapChannels();
    const double leftDuration = sourceDuration(left);
    const double rightDuration = sourceDuration(right);
    const double duration = std::max(leftDuration, rightDuration);

    std::ostringstream out;
    out << "compare " << leftLabel << " vs " << rightLabel << "\n";
    out << "  format  " << left.formatName() << " / " << right.formatName()
        << "\n";
    out << "  path    " << left.path() << "\n";
    out << "          " << right.path() << "\n";
    out << "  channels " << left.channels().size() << " / "
        << right.channels().size() << "\n";
    out << "  duration " << std::fixed << std::setprecision(6) << leftDuration
        << " / " << rightDuration << " s  d=" << (rightDuration - leftDuration)
        << "\n";
    out << "  offset  ";
    formatOptional(out, left.videoPresentationOffsetSec(), 9);
    out << " / ";
    formatOptional(out, right.videoPresentationOffsetSec(), 9);
    out << " s\n";

    const auto leftLaps =
        left.sourceLaps().empty() ? left.detectLaps() : left.sourceLaps();
    const auto rightLaps =
        right.sourceLaps().empty() ? right.detectLaps() : right.sourceLaps();
    out << "  laps    " << leftLaps.size() << " / " << rightLaps.size() << "\n";
    const size_t lapCount = std::max(leftLaps.size(), rightLaps.size());
    for (size_t i = 0; i < lapCount && i < 12; ++i) {
        out << "    L" << (i + 1) << "  ";
        if (i < leftLaps.size())
            out << std::fixed << std::setprecision(3) << leftLaps[i].startTime
                << "->" << leftLaps[i].endTime;
        else
            out << "-";
        out << "  /  ";
        if (i < rightLaps.size())
            out << std::fixed << std::setprecision(3) << rightLaps[i].startTime
                << "->" << rightLaps[i].endTime;
        else
            out << "-";
        if (i < leftLaps.size() && i < rightLaps.size()) {
            out << "  dStart=" << std::setprecision(6)
                << (rightLaps[i].startTime - leftLaps[i].startTime)
                << "  dEnd=" << (rightLaps[i].endTime - leftLaps[i].endTime);
        }
        out << "\n";
    }

    out << "  mapped channels:\n";
    for (const char* conceptName : kConcepts) {
        const RawChannel* leftChannel =
            mappedChannel(left, leftMap, conceptName);
        const RawChannel* rightChannel =
            mappedChannel(right, rightMap, conceptName);
        out << "    " << std::left << std::setw(16) << conceptName << std::right
            << " " << leftLabel << "=";
        if (leftChannel) {
            out << leftChannel->name << " unit='" << leftChannel->unit << "' "
                << std::setprecision(3) << leftChannel->frequencyHz
                << "Hz n=" << leftChannel->samples.size();
        } else {
            out << "-";
        }
        out << "\n                    " << rightLabel << "=";
        if (rightChannel) {
            out << rightChannel->name << " unit='" << rightChannel->unit << "' "
                << std::setprecision(3) << rightChannel->frequencyHz
                << "Hz n=" << rightChannel->samples.size();
        } else {
            out << "-";
        }
        out << "\n";
        if (leftChannel) appendRawAnchors(out, leftChannel);
        if (rightChannel && rightChannel != leftChannel)
            appendRawAnchors(out, rightChannel);
    }

    std::vector<double> times;
    if (duration > 0.0) {
        times.push_back(0.0);
        times.push_back(std::min(1.0, duration));
        times.push_back(duration * 0.25);
        times.push_back(duration * 0.5);
        times.push_back(duration * 0.75);
        times.push_back(std::max(0.0, duration - 1.0));
    }
    for (const Lap& lap : leftLaps) {
        times.push_back(lap.startTime);
        if (lap.endTime > lap.startTime)
            times.push_back(0.5 * (lap.startTime + lap.endTime));
    }
    std::sort(times.begin(), times.end());
    times.erase(
        std::unique(times.begin(), times.end(),
                    [](double a, double b) { return std::fabs(a - b) < 1e-6; }),
        times.end());
    if (times.size() > 16) times.resize(16);

    out << "  samples:\n";
    for (double timeSec : times) {
        out << "    t=" << std::fixed << std::setprecision(6) << timeSec
            << "s\n";
        for (const char* conceptName : kConcepts) {
            const auto leftIt = leftMap.find(conceptName);
            const auto rightIt = rightMap.find(conceptName);
            const int leftIdx = leftIt == leftMap.end() ? -1 : leftIt->second;
            const int rightIdx =
                rightIt == rightMap.end() ? -1 : rightIt->second;
            const bool linear = std::string(conceptName) != "gear";
            const std::string leftText =
                sampleText(left, leftIdx, timeSec, linear);
            const std::string rightText =
                sampleText(right, rightIdx, timeSec, linear);
            out << "      " << std::left << std::setw(16) << conceptName
                << std::right << " " << leftLabel << "=" << leftText << "  "
                << rightLabel << "=" << rightText;
            double leftValue = 0.0;
            double rightValue = 0.0;
            if (leftIdx >= 0 && rightIdx >= 0 &&
                left.sampleAt(size_t(leftIdx), timeSec, &leftValue, linear) &&
                right.sampleAt(size_t(rightIdx), timeSec, &rightValue,
                               linear) &&
                std::isfinite(leftValue) && std::isfinite(rightValue)) {
                out << "  d=" << std::setprecision(6)
                    << (rightValue - leftValue);
            }
            out << "\n";
        }
        const auto leftFrame = left.videoFrameAt(timeSec);
        const auto rightFrame = right.videoFrameAt(timeSec);
        out << "      " << std::left << std::setw(16) << "video_frame"
            << std::right << " " << leftLabel << "=";
        if (leftFrame)
            out << *leftFrame;
        else
            out << "-";
        out << "  " << rightLabel << "=";
        if (rightFrame)
            out << *rightFrame;
        else
            out << "-";
        if (leftFrame && rightFrame) {
            const auto delta = int64_t(*rightFrame) - int64_t(*leftFrame);
            out << "  d=" << delta;
        }
        out << "\n";
    }
    return out.str();
}

bool TelemetrySource::sampleAt(size_t channelIdx, double timeSec, double* out,
                               bool linear) const {
    if (channelIdx >= channels_.size() || !out || !std::isfinite(timeSec) ||
        timeSec < 0.0)
        return false;
    if (handle_) {
        const uint64_t timeNs = uint64_t(std::llround(timeSec * 1e9));
        return omatrack_sample_at(handle_, channelIdx, timeNs, linear ? 1 : 0,
                                  out) != 0;
    }

    const RawChannel& channel = channels_[channelIdx];
    if (channel.samples.empty() || !(channel.frequencyHz > 0.0)) return false;
    const double position = timeSec * channel.frequencyHz;
    if (position < 0.0 || position > double(channel.samples.size() - 1))
        return false;
    const size_t low = size_t(std::floor(position));
    const size_t high = std::min(low + 1, channel.samples.size() - 1);
    if (!linear) {
        *out = (position - double(low) < 0.5) ? channel.samples[low]
                                              : channel.samples[high];
        return true;
    }
    const double fraction = position - double(low);
    *out = channel.samples[low] +
           (channel.samples[high] - channel.samples[low]) * fraction;
    return true;
}

std::map<std::string, int> TelemetrySource::mapChannels(
    const ChannelOverrides& overrides) const {
    std::map<std::string, int> mapping;
    std::vector<std::string> normalizedChannels;
    normalizedChannels.reserve(channels_.size());
    for (const RawChannel& channel : channels_)
        normalizedChannels.push_back(normalizeChannelName(channel.name));

    for (const auto& [fieldName, aliases] : channelMappings()) {
        const auto overrideIt = overrides.find(fieldName);
        if (overrideIt != overrides.end()) {
            bool matched = false;
            for (size_t c = 0; c < channels_.size(); ++c) {
                if (!channels_[c].samples.empty() &&
                    channels_[c].name == overrideIt->second) {
                    mapping[fieldName] = int(c);
                    matched = true;
                    break;
                }
            }
            if (matched) continue;
            const std::string wanted = normalizeChannelName(overrideIt->second);
            if (wanted.empty()) continue;
            for (size_t c = 0; c < channels_.size(); ++c) {
                if (!channels_[c].samples.empty() &&
                    normalizedChannels[c] == wanted) {
                    mapping[fieldName] = int(c);
                    break;
                }
            }
            continue;
        }
        std::vector<std::string> normalizedAliases;
        normalizedAliases.reserve(aliases.size());
        for (const std::string& alias : aliases)
            normalizedAliases.push_back(normalizeChannelName(alias));
        int bestScore = std::numeric_limits<int>::min();
        int best = -1;
        for (size_t c = 0; c < channels_.size(); ++c) {
            if (channels_[c].samples.empty()) continue;
            for (size_t a = 0; a < aliases.size(); ++a) {
                const int score = scoreNormalizedChannelMatch(
                    normalizedChannels[c], normalizedAliases[a], int(a));
                if (score > bestScore) {
                    bestScore = score;
                    best = int(c);
                }
            }
        }
        if (best >= 0) mapping[fieldName] = best;
    }
    return mapping;
}

double TelemetrySource::detectDriverId(
    const ChannelOverrides& overrides) const {
    const auto mapping = mapChannels(overrides);
    const auto driver = mapping.find("driver_id");
    if (driver == mapping.end() || driver->second < 0 ||
        driver->second >= int(channels_.size()))
        return 0.0;
    const RawChannel& channel = channels_[size_t(driver->second)];
    return dominantDriverId(channel.samples, channel.sampleTypeCode);
}

std::vector<Lap> TelemetrySource::detectLaps() const {
    // Exact normalized match first, then contains fallback for longer aliases.
    auto firstId = [&](const std::vector<std::string>& aliases) -> int {
        for (auto& alias : aliases) {
            std::string nAlias = normalizeChannelName(alias);
            for (size_t i = 0; i < channels_.size(); ++i) {
                if (channels_[i].samples.empty()) continue;
                if (normalizeChannelName(channels_[i].name) == nAlias)
                    return int(i);
            }
        }
        for (auto& alias : aliases) {
            std::string nAlias = normalizeChannelName(alias);
            if (nAlias.size() < 4) continue;
            for (size_t i = 0; i < channels_.size(); ++i) {
                if (channels_[i].samples.empty()) continue;
                if (normalizeChannelName(channels_[i].name).find(nAlias) !=
                    std::string::npos)
                    return int(i);
            }
        }
        return -1;
    };

    auto series = [&](int id) -> std::pair<std::vector<double>, int> {
        if (id < 0 || id >= int(channels_.size())) return {};
        const RawChannel& channel = channels_[id];
        if (channel.samples.empty() || channel.frequencyHz <= 0.0 ||
            channel.durationSec <= 0.0)
            return {};
        const int frequency =
            std::max(1, int(std::lround(channel.frequencyHz)));
        const size_t count =
            size_t(std::ceil(channel.durationSec * double(frequency)));
        std::vector<double> values(count, 0.0);
        bool sampled = false;
        double lastValue = 0.0;
        for (size_t index = 0; index < count; ++index) {
            double value = 0.0;
            const double time = double(index) / double(frequency);
            if (sampleAt(size_t(id), time, &value)) {
                if (!sampled)
                    std::fill(values.begin(), values.begin() + index, value);
                sampled = true;
                lastValue = value;
            } else if (sampled) {
                value = lastValue;
            }
            values[index] = value;
        }
        if (!sampled) return {};
        return {std::move(values), frequency};
    };

    int lapBeaconId = firstId({"lap_beacon_trig", "laptrigger", "lap_beacon"});
    int lapNumberId = firstId({"lap number"});
    int lapDistanceId = firstId({"lap distance corrected", "lap distance"});
    int lapTimeId = firstId({"lap time"});
    int previousLapTimeId = firstId({"previous lap time", "previous lt"});

    auto [lapBeacon, beaconFreq] = series(lapBeaconId);
    auto [lapNumber, numberFreq] = series(lapNumberId);
    auto [lapDistance, distanceFreq] = series(lapDistanceId);
    auto [lapTime, timeFreq] = series(lapTimeId);
    auto [prevLapTime, prevFreq] = series(previousLapTimeId);

    if (!sourceLaps_.empty()) {
        std::vector<Lap> laps = sourceLaps_;
        markShortCrossingsIncomplete(laps);
        if (!lapDistance.empty())
            laps = pdsApplyLapDistanceCoverage(laps, lapDistance,
                                               std::max(1, distanceFreq));
        return laps;
    }

    double maxDuration = 0.0;
    for (const RawChannel& channel : channels_)
        maxDuration = std::max(maxDuration, channel.durationSec);

    const std::vector<double> beaconSplits =
        pdsBeaconSplits(lapBeacon, beaconFreq);
    const std::vector<double> lapNumberSplits =
        pdsLapNumberSplits(lapNumber, numberFreq);
    const bool lapNumberIsAuthority = lapNumberCarriesState(lapNumber);
    const std::vector<double> splitTimes =
        selectLapSplits(beaconSplits, lapNumberSplits, lapNumberIsAuthority,
                        pdsLapTimeSplits(lapTime, timeFreq),
                        pdsDistanceSplits(lapDistance, distanceFreq));

    // Counter increments define boundaries, but a double trigger can still
    // create an impossible crossing pair. Keep the shared short-lap rejection
    // so such candidates never participate in best-lap selection.
    std::vector<Lap> laps = buildLapsFromSplits(splitTimes, maxDuration);
    if (!prevLapTime.empty())
        laps = pdsApplyPreviousLapTimes(
            laps, prevLapTime, std::max(1, prevFreq), !lapNumberIsAuthority);
    if (!lapNumberIsAuthority && !lapDistance.empty())
        laps = pdsApplyLapDistanceCoverage(laps, lapDistance,
                                           std::max(1, distanceFreq));
    return laps;
}

std::vector<Lap> detectLapsLightweight(const std::string& path,
                                       double* driverId) {
    const auto closeBridge = [](void* handle) {
        if (handle) omatrack_close(handle);
    };
    std::unique_ptr<void, decltype(closeBridge)> bridge(
        omatrack_open(path.c_str()), closeBridge);
    if (!bridge) return {};
    void* handle = bridge.get();
    const std::vector<Lap> sourceLaps = sourceLapsFromBridge(handle);

    const size_t channelCount = omatrack_channel_count(handle);
    auto firstId = [&](const std::vector<std::string>& aliases) -> int {
        for (const std::string& alias : aliases) {
            const std::string normalizedAlias = normalizeChannelName(alias);
            for (size_t i = 0; i < channelCount; ++i) {
                const char* name = omatrack_channel_name(handle, i);
                if (name && normalizeChannelName(name) == normalizedAlias)
                    return int(i);
            }
        }
        for (const std::string& alias : aliases) {
            const std::string normalizedAlias = normalizeChannelName(alias);
            if (normalizedAlias.size() < 4) continue;
            for (size_t i = 0; i < channelCount; ++i) {
                const char* name = omatrack_channel_name(handle, i);
                if (name && normalizeChannelName(name).find(normalizedAlias) !=
                                std::string::npos)
                    return int(i);
            }
        }
        return -1;
    };
    auto decodeRaw = [&](int id) {
        std::pair<std::vector<double>, int> result;
        if (id < 0) return result;
        const uint64_t count =
            omatrack_channel_sample_count(handle, size_t(id));
        result.first.resize(size_t(count));
        if (count > 0) {
            const size_t written = omatrack_channel_decode_all(
                handle, size_t(id), result.first.data(), size_t(count));
            result.first.resize(written);
        }
        const uint64_t period = omatrack_chunk_period_ns(handle, size_t(id), 0);
        result.second =
            period > 0 ? std::max(1, int(std::lround(1e9 / period))) : 1;
        return result;
    };
    auto sampleRegular = [&](int id) {
        std::pair<std::vector<double>, int> result;
        if (id < 0) return result;
        const uint64_t period = omatrack_chunk_period_ns(handle, size_t(id), 0);
        result.second =
            period > 0 ? std::max(1, int(std::lround(1e9 / period))) : 1;
        const double duration =
            double(omatrack_channel_duration_ns(handle, size_t(id))) / 1e9;
        const size_t count =
            size_t(std::ceil(duration * double(result.second)));
        result.first.assign(count, 0.0);
        bool sampled = false;
        double lastValue = 0.0;
        for (size_t index = 0; index < count; ++index) {
            double value = 0.0;
            const uint64_t timeNs = uint64_t(
                std::llround(double(index) * 1e9 / double(result.second)));
            if (omatrack_sample_at(handle, size_t(id), timeNs, 1, &value)) {
                if (!sampled)
                    std::fill(result.first.begin(),
                              result.first.begin() + index, value);
                sampled = true;
                lastValue = value;
            } else if (sampled) {
                value = lastValue;
            }
            result.first[index] = value;
        }
        if (!sampled) result.first.clear();
        return result;
    };

    const int beaconId =
        firstId({"lap_beacon_trig", "laptrigger", "lap_beacon"});
    const int lapNumberId = firstId({"lap number"});
    const int lapDistanceId =
        firstId({"lap distance corrected", "lap distance"});
    const int lapTimeId = firstId({"lap time"});
    const int previousLapTimeId = firstId({"previous lap time", "previous lt"});
    const int driverIdChannel = firstId(driverIdAliases());

    auto [driverValues, driverFreq] = decodeRaw(driverIdChannel);
    (void)driverFreq;
    if (driverId) {
        const uint32_t sampleTypeCode =
            driverIdChannel >= 0
                ? omatrack_channel_type_code(handle, size_t(driverIdChannel))
                : 0;
        const double detected = dominantDriverId(driverValues, sampleTypeCode);
        if (detected > 0.0) *driverId = detected;
    }
    auto [beacon, beaconFreq] = sampleRegular(beaconId);
    auto [lapNumber, numberFreq] = sampleRegular(lapNumberId);
    auto [lapDistance, distanceFreq] = sampleRegular(lapDistanceId);
    auto [lapTime, timeFreq] = sampleRegular(lapTimeId);
    auto [previousLapTime, previousFreq] = sampleRegular(previousLapTimeId);

    if (!sourceLaps.empty()) {
        std::vector<Lap> laps = sourceLaps;
        markShortCrossingsIncomplete(laps);
        if (!lapDistance.empty())
            laps = pdsApplyLapDistanceCoverage(laps, lapDistance,
                                               std::max(1, distanceFreq));
        return laps;
    }

    double maxDuration = 0.0;
    for (size_t i = 0; i < channelCount; ++i)
        maxDuration = std::max(
            maxDuration, double(omatrack_channel_duration_ns(handle, i)) / 1e9);

    const std::vector<double> beaconSplits =
        pdsBeaconSplits(beacon, beaconFreq);
    const std::vector<double> lapNumberSplits =
        pdsLapNumberSplits(lapNumber, numberFreq);
    const bool lapNumberIsAuthority = lapNumberCarriesState(lapNumber);
    const std::vector<double> splits =
        selectLapSplits(beaconSplits, lapNumberSplits, lapNumberIsAuthority,
                        pdsLapTimeSplits(lapTime, timeFreq),
                        pdsDistanceSplits(lapDistance, distanceFreq));

    std::vector<Lap> laps = buildLapsFromSplits(splits, maxDuration);
    if (!previousLapTime.empty())
        laps = pdsApplyPreviousLapTimes(laps, previousLapTime,
                                        std::max(1, previousFreq),
                                        !lapNumberIsAuthority);
    if (!lapNumberIsAuthority && !lapDistance.empty())
        laps = pdsApplyLapDistanceCoverage(laps, lapDistance,
                                           std::max(1, distanceFreq));

    return laps;
}

UnifiedLap TelemetrySource::unifyLap(double startTime, double endTime,
                                     const ChannelOverrides& overrides) const {
    if (!std::isfinite(startTime) || !std::isfinite(endTime) ||
        !(endTime > startTime))
        return {};
    const double duration = endTime - startTime;
    if (duration > double(std::numeric_limits<int>::max() - 1) /
                       double(kDefaultSampleRate))
        return {};
    const int nSamples = int(duration * kDefaultSampleRate) + 1;
    auto mapping = mapChannels(overrides);
    // Sample each channel on the shared 50 Hz absolute-time grid. Source
    // chunks may start late or contain acquisition gaps; slicing flattened
    // arrays by index silently shifts every event after a gap.
    std::map<std::string, std::vector<double>> resampled;
    std::map<std::string, std::string> channelUnits;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    for (auto& [field, idx] : mapping) {
        const RawChannel& channel = channels_[idx];
        if (channel.samples.empty()) continue;
        const bool gpsPosition = field == "gps_lat" || field == "gps_lon";
        const bool nearest = field == "gear";
        std::vector<double> values(size_t(nSamples), gpsPosition ? nan : 0.0);
        bool sampled = false;
        for (int sample = 0; sample < nSamples; ++sample) {
            double value = 0.0;
            const double time =
                startTime + double(sample) / double(kDefaultSampleRate);
            if (sampleAt(size_t(idx), time, &value, !nearest)) {
                values[size_t(sample)] = value;
                sampled = true;
            } else if (sample > 0 && !gpsPosition) {
                values[size_t(sample)] = values[size_t(sample - 1)];
            }
        }
        if (!sampled) continue;
        channelUnits[field] = lowerTrimmed(channel.unit);
        resampled[field] = std::move(values);
    }

    auto get = [&](const std::string& f, int i) -> double {
        auto it = resampled.find(f);
        if (it == resampled.end() || i >= (int)it->second.size())
            return (f == "gps_lat" || f == "gps_lon") ? nan : 0;
        return it->second[i];
    };
    const std::string emptyUnit;
    auto unitOf = [&](const std::string& f) -> const std::string& {
        const auto it = channelUnits.find(f);
        return it != channelUnits.end() ? it->second : emptyUnit;
    };
    auto pedalFactor = [](const std::string& unit) {
        if (unit == "rad") return 1.0 / 1.7453292519943295;
        if (unit == "deg") return 0.01;
        return 1.0;
    };
    auto speedFactor = [&](const std::string& field, double fallback) {
        const auto it = speedUnits().find(unitOf(field));
        return it != speedUnits().end() ? it->second : fallback;
    };

    const bool hasSpeed = resampled.count("speed") != 0;
    const bool hasBrake = resampled.count("brake") != 0;
    const bool hasGpsSpeedAccuracy = resampled.count("gps_speed_accuracy") != 0;
    // Empty unit is km/h. AiM scalars are unitless by design; the Motec
    // companion copies that emptiness. Treating it as m/s made wheel speed
    // 3.6× high and rejected native lap distance.
    const double wheelSpeedFactor = speedFactor("speed", 1.0);
    const double throttleFactor = pedalFactor(unitOf("throttle"));
    const double clutchFactor = pedalFactor(unitOf("clutch"));
    const double driverThrottleFactor = pedalFactor(unitOf("driver_throttle"));
    const auto brakeUnit = brakeUnits().find(unitOf("brake"));
    const double brakeFactor =
        brakeUnit != brakeUnits().end() ? brakeUnit->second : 1.0;
    const double steeringFactor =
        unitOf("steering") == "rad" ? 180.0 / kPi : 1.0;
    const bool gpsLatRadians = unitOf("gps_lat") == "rad";
    const bool gpsLonRadians = unitOf("gps_lon") == "rad";
    double positionAccuracyFactor = 1.0;
    if (unitOf("gps_position_accuracy") == "cm")
        positionAccuracyFactor = 0.01;
    else if (unitOf("gps_position_accuracy") == "mm")
        positionAccuracyFactor = 0.001;
    else if (unitOf("gps_position_accuracy") == "ft")
        positionAccuracyFactor = 0.3048;
    const double gpsSpeedAccuracyFactor =
        speedFactor("gps_speed_accuracy", 3.6) / 3.6;
    const double gpsSpeedFactor =
        speedFactor("gps_speed", format_ == "aimd" ? 3.6 : 1.0) / 3.6;

    int gearOffset = 0;
    auto gearIt = resampled.find("gear");
    if (gearIt != resampled.end()) {
        int minPositive = std::numeric_limits<int>::max();
        for (double v : gearIt->second) {
            int g = int(std::llround(v));
            if (g > 0) minPositive = std::min(minPositive, g);
        }
        if (minPositive != std::numeric_limits<int>::max() && minPositive >= 2)
            gearOffset = 1;
    }

    UnifiedLap unified;
    unified.sampleRate = kDefaultSampleRate;
    unified.time.reserve(nSamples);
    unified.speed.reserve(nSamples);
    unified.throttle.reserve(nSamples);
    unified.brake.reserve(nSamples);
    unified.clutch.reserve(nSamples);
    unified.steering.reserve(nSamples);
    unified.gear.reserve(nSamples);
    unified.distance.reserve(nSamples);
    unified.gForceLong.reserve(nSamples);
    unified.gForceLat.reserve(nSamples);
    unified.gpsLat.reserve(nSamples);
    unified.gpsLon.reserve(nSamples);
    unified.gpsPositionAccuracy.reserve(nSamples);
    unified.gpsSpeedAccuracy.reserve(nSamples);
    unified.damperFL.reserve(nSamples);
    unified.damperFR.reserve(nSamples);
    unified.damperRL.reserve(nSamples);
    unified.damperRR.reserve(nSamples);
    unified.driverThrottle.reserve(nSamples);

    std::vector<double> gpsSpeedMps;
    gpsSpeedMps.reserve(nSamples);

    double dt = 1.0 / double(kDefaultSampleRate);
    for (int i = 0; i < nSamples; ++i) {
        unified.time.push_back(double(i) * dt);

        // speed → km/h
        unified.speed.push_back(hasSpeed ? get("speed", i) * wheelSpeedFactor
                                         : 0.0);

        // driver controls → normalized ranges / physical pressure
        double th = get("throttle", i) * throttleFactor;
        if (th > 1.5) th /= 100;
        unified.throttle.push_back(std::clamp(th, 0.0, 1.0));

        if (hasBrake) {
            unified.brake.push_back(
                std::max(0.0, get("brake", i) * brakeFactor));
        } else {
            double bp = get("brake_pos", i);
            if (bp > 1.5) bp /= 100;
            unified.brake.push_back(std::max(0.0, bp * 100));
        }

        double clutch = get("clutch", i) * clutchFactor;
        if (clutch > 1.5) clutch /= 100;
        unified.clutch.push_back(std::clamp(clutch, 0.0, 1.0));

        unified.steering.push_back(get("steering", i) * steeringFactor);
        unified.gear.push_back(
            std::max(0, int(std::llround(get("gear", i))) - gearOffset));

        double dth = get("driver_throttle", i) * driverThrottleFactor;
        if (dth > 1.5) dth /= 100;
        unified.driverThrottle.push_back(std::clamp(dth, 0.0, 1.0));

        unified.gForceLong.push_back(get("g_long", i));
        unified.gForceLat.push_back(get("g_lat", i));
        unified.damperFL.push_back(get("damper_fl", i));
        unified.damperFR.push_back(get("damper_fr", i));
        unified.damperRL.push_back(get("damper_rl", i));
        unified.damperRR.push_back(get("damper_rr", i));

        // GPS coordinates and quality metadata
        double rawLat = get("gps_lat", i);
        double rawLon = get("gps_lon", i);
        if (gpsLatRadians) rawLat *= 180.0 / kPi;
        if (gpsLonRadians) rawLon *= 180.0 / kPi;
        unified.gpsLat.push_back(rawLat);
        unified.gpsLon.push_back(rawLon);
        unified.gpsPositionAccuracy.push_back(std::max(
            0.0, get("gps_position_accuracy", i) * positionAccuracyFactor));
        unified.gpsSpeedAccuracy.push_back(std::max(
            0.0, get("gps_speed_accuracy", i) * gpsSpeedAccuracyFactor));
        gpsSpeedMps.push_back(
            std::max(0.0, get("gps_speed", i) * gpsSpeedFactor));
    }

    // Distance propagates from wheel/vehicle speed at 50 Hz. GPS Doppler
    // speed gently corrects drift only while its reported accuracy is useful;
    // poor GPS therefore cannot inject position jitter into the distance axis.
    std::vector<double> fusedDistance(size_t(nSamples), 0.0);
    std::vector<double> fusedSpeed(size_t(nSamples), 0.0);
    for (int i = 0; i < nSamples; ++i) {
        const double wheel = std::max(0.0, unified.speed[i] / 3.6);
        const double gps = gpsSpeedMps[size_t(i)];
        double gpsWeight = 0.0;
        if (gps > 0.0) {
            const double accuracy = unified.gpsSpeedAccuracy[size_t(i)];
            if (hasGpsSpeedAccuracy && accuracy > 0.0) {
                gpsWeight = std::clamp((1.5 - accuracy) / 1.25, 0.0, 1.0) * 0.5;
            } else {
                gpsWeight = 0.2;
            }
        }
        if (wheel <= 0.0) gpsWeight = gps > 0.0 ? 1.0 : 0.0;
        fusedSpeed[size_t(i)] = wheel * (1.0 - gpsWeight) + gps * gpsWeight;
    }
    for (int i = 1; i < nSamples; ++i) {
        const double step =
            0.5 * (fusedSpeed[size_t(i - 1)] + fusedSpeed[size_t(i)]) * dt;
        fusedDistance[size_t(i)] =
            fusedDistance[size_t(i - 1)] + std::max(0.0, step);
    }

    // A native lap-distance signal wins only after proving that its total and
    // continuity agree with the independently integrated velocity. Logger
    // math channels can carry a different scale, freeze, or reset mid-lap;
    // accepting those silently makes cross-car video alignment unusable.
    auto distIt = resampled.find("distance");
    bool nativeAccepted = false;
    if (distIt != resampled.end() && int(distIt->second.size()) >= nSamples) {
        const auto& rawDistance = distIt->second;
        std::vector<double> nativeDistance(size_t(nSamples), 0.0);
        int rejectedSteps = 0;
        for (int i = 1; i < nSamples; ++i) {
            const double fallback =
                fusedDistance[size_t(i)] - fusedDistance[size_t(i - 1)];
            double delta = rawDistance[size_t(i)] - rawDistance[size_t(i - 1)];
            const double maximumPlausible =
                std::max(10.0, fallback * 8.0 + 1.0);
            if (!std::isfinite(delta) || delta < -0.25 ||
                delta > maximumPlausible) {
                delta = fallback;
                ++rejectedSteps;
            } else {
                delta = std::max(0.0, delta);
            }
            nativeDistance[size_t(i)] = nativeDistance[size_t(i - 1)] + delta;
        }

        const double fusedTotal = fusedDistance.back();
        const double nativeTotal = nativeDistance.back();
        const double ratio =
            fusedTotal > 100.0 ? nativeTotal / fusedTotal : 1.0;
        const double rejectedFraction =
            nSamples > 1 ? double(rejectedSteps) / double(nSamples - 1) : 1.0;
        nativeAccepted =
            fusedTotal <= 100.0 ||
            (ratio >= 0.97 && ratio <= 1.03 && rejectedFraction <= 0.02);
        if (nativeAccepted) unified.distance = std::move(nativeDistance);
    }
    if (!nativeAccepted) unified.distance = std::move(fusedDistance);
    unified.distanceSource =
        nativeAccepted ? DistanceSource::Native : DistanceSource::SpeedFused;
    return unified;
}

std::vector<double> resample(const std::vector<double>& values, double srcFreq,
                             double targetFreq, double duration) {
    std::vector<double> out;
    if (values.empty() || !std::isfinite(srcFreq) ||
        !std::isfinite(targetFreq) || !std::isfinite(duration) ||
        !(srcFreq > 0.0) || !(targetFreq > 0.0) || !(duration > 0.0))
        return out;
    if (std::fabs(srcFreq - targetFreq) < 1e-9) return values;
    if (duration > double(std::numeric_limits<int>::max() - 1) / targetFreq)
        return out;
    const int nOut = int(duration * targetFreq) + 1;
    out.reserve(nOut);
    int maxIdx = (int)values.size() - 1;
    for (int i = 0; i < nOut; ++i) {
        double t = double(i) / targetFreq;
        double srcIdx = t * srcFreq;
        int lo = std::min((int)std::floor(srcIdx), maxIdx);
        int hi = std::min(lo + 1, maxIdx);
        double frac = srcIdx - std::floor(srcIdx);
        out.push_back(values[lo] + (values[hi] - values[lo]) * frac);
    }
    return out;
}

}  // namespace omatrack
