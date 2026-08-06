// Implementation of the Omatrack core telemetry engine.
//
// Ports the Omatrack analysis layer (MoTecParser.swift + TelemetryUtils)
// on top of the Rust parsing bridge. Parsing itself is delegated to the
// vendored duckdb_motorsport_telemetry crates.

#include "TelemetryEngineInternal.h"
#include "omatrack_bridge.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <set>

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
         {"driver throttle pos", "accel pedal pos", "acc pedal pos",
          "fbwdrivertps", "pps", "tpsreal", "tps", "aps", "throttle pos"}},
        {"brake",
         {"brake pressure f", "brake pressure fr", "p_f_brake",
          "p_brake_front"}},
        {"clutch", {"clutch pos", "clutch position", "clutch pedal", "clutch"}},
        {"brake_pos", {"brake pos"}},
        {"steering", {"steering angle", "steer"}},
        {"gear", {"gear_pos", "gear", "gearposdisplay"}},
        {"driver_throttle", {"driver throttle pos", "fbwdrivertps", "pps"}},
        {"g_long", {"g force long", "i_accel_long", "fia_accelx"}},
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
        {"m/s", 3.6}, {"km/h", 1.0}, {"mph", 1.60934}};
    return m;
}

const std::map<std::string, double>& brakeUnits() {
    static const std::map<std::string, double> m = {
        {"bar", 1.0}, {"psi", 0.0689476}, {"kpa", 0.01}, {"pa", 0.00001}};
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
    for (size_t i = 1; i < values.size(); ++i) {
        int current = std::llround(values[i]);
        if (current > prev) splits.push_back(double(i) / double(freq));
        prev = current;
    }
    return splits;
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
                                     double duration) {
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
    // A crossing pair much shorter than the session median is a double
    // trigger, not a lap. With too few bounds to trust a median, fall back
    // to a conservative floor: no circuit lap is under half a minute.
    const bool medianIsEvidence = lapBounds.size() >= 3;
    const double minLapSeconds =
        medianIsEvidence ? median * 0.5 : std::max(median * 0.5, 30.0);
    for (Bound& bound : lapBounds)
        if (bound.complete && bound.end - bound.start < minLapSeconds)
            bound.complete = false;
    for (size_t i = 0; i < lapBounds.size(); ++i) {
        result.push_back(Lap{int(i), lapBounds[i].start, lapBounds[i].end,
                             (lapBounds[i].end - lapBounds[i].start) * 1000.0,
                             lapBounds[i].complete});
    }
    return result;
}

std::vector<Lap> pdsApplyPreviousLapTimes(
    const std::vector<Lap>& laps,
    const std::vector<double>& previousLapTimeValues, int freq) {
    if (laps.empty() || previousLapTimeValues.empty() || freq <= 0) return laps;
    auto samplePrevLapTime = [&](double time) -> double {
        int center = int(std::llround(time * double(freq)));
        struct Best {
            int delta;
            double value;
        };
        Best best{-1, 0.0};
        for (int delta = 0; delta <= 2; ++delta) {
            bool found = false;
            for (int sign : {-1, 1}) {
                int idx = center + sign * delta;
                if (idx < 0 || idx >= int(previousLapTimeValues.size()))
                    continue;
                double value = previousLapTimeValues[idx];
                if (!(value > 1 && value < 600)) continue;
                if (best.delta < 0 || delta < best.delta) {
                    best = {delta, value};
                    found = true;
                }
                if (delta == 0) return value;
            }
            (void)found;
        }
        return best.delta >= 0 ? best.value : -1.0;
    };
    std::vector<Lap> out = laps;
    for (auto& lap : out) {
        double prevLapSec = samplePrevLapTime(lap.endTime);
        if (prevLapSec < 0) continue;
        double fallbackSec = lap.timeMs / 1000.0;
        if (std::fabs(prevLapSec - fallbackSec) > 30) continue;
        lap.timeMs = prevLapSec * 1000.0;
    }
    return out;
}

// ── channel mapping scoring (port of MoTecParser) ───────────────────

int scoreChannelMatch(const std::string& channelName, const std::string& alias,
                      int aliasPriority) {
    const std::string nChannel = normalizeChannelName(channelName);
    const std::string nAlias = normalizeChannelName(alias);
    if (nChannel.empty() || nAlias.empty())
        return std::numeric_limits<int>::min();
    if (nChannel == nAlias) return 10000 - aliasPriority;
    if (nAlias.size() >= 4 && nChannel.find(nAlias) != std::string::npos)
        return 7000 - aliasPriority;
    if (nAlias.size() >= 6 && nAlias.find(nChannel) != std::string::npos)
        return 6000 - aliasPriority;
    return std::numeric_limits<int>::min();
}

// Driver-ID channel aliases, shared by the loaded-source and lightweight paths.
const std::vector<std::string>& driverIdAliases() {
    static const std::vector<std::string> aliases{
        "DriverID", "driver_id",      "driver id",
        "driverid", "activeDriverId", "X2LNK_driverID"};
    return aliases;
}

/// Most frequent positive id in a driver-id series; ties go to the earlier one.
int dominantDriverId(const std::vector<double>& values) {
    std::map<int, std::pair<size_t, size_t>> counts;
    for (size_t index = 0; index < values.size(); ++index) {
        const int candidate = int(std::llround(values[index]));
        if (candidate <= 0) continue;
        auto& entry = counts[candidate];
        ++entry.first;
        if (entry.first == 1) entry.second = index;
    }
    int bestId = 0;
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

std::unique_ptr<TelemetrySource> TelemetrySource::open(
    const std::string& path) {
    void* handle = omatrack_open(path.c_str());
    if (!handle) {
        const char* err = omatrack_last_error();
        fprintf(stderr, "omatrack: failed to open %s: %s\n", path.c_str(),
                err ? err : "unknown");
        return nullptr;
    }
    std::unique_ptr<TelemetrySource> src(new TelemetrySource());
    src->handle_ = handle;
    src->path_ = path;
    const char* fmt = omatrack_format(handle);
    src->format_ = fmt ? fmt : "";
    int64_t mediaTimeOffsetNs = 0;
    if (omatrack_media_time_offset_ns(handle, &mediaTimeOffsetNs))
        src->mediaTimeOffsetSec_ = double(mediaTimeOffsetNs) / 1e9;
    size_t n = omatrack_channel_count(handle);
    src->channels_.reserve(n);
    const std::string lower = [&] {
        std::string s(path);
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        return s;
    }();
    bool isPds = lower.size() > 4 && lower.substr(lower.size() - 4) == ".pds";
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
        // PDS stores SI values; keep physical units as-is (port: no formula).
        (void)isPds;
        src->channels_.push_back(std::move(ch));
    }
    return src;
}

TelemetrySource::~TelemetrySource() {
    if (handle_) omatrack_close(handle_);
}

bool TelemetrySource::sampleAt(size_t channelIdx, double timeSec,
                               double* out) const {
    if (channelIdx >= channels_.size() || !out) return false;
    uint64_t timeNs = (uint64_t)std::llround(timeSec * 1e9);
    return omatrack_sample_at(handle_, channelIdx, timeNs, /*linear=*/1, out) !=
           0;
}

std::map<std::string, int> TelemetrySource::mapChannels() const {
    std::map<std::string, int> mapping;
    for (auto& [fieldName, aliases] : channelMappings()) {
        int bestScore = std::numeric_limits<int>::min();
        int best = -1;
        for (size_t c = 0; c < channels_.size(); ++c) {
            if (channels_[c].samples.empty()) continue;
            for (size_t a = 0; a < aliases.size(); ++a) {
                int score =
                    scoreChannelMatch(channels_[c].name, aliases[a], (int)a);
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

int TelemetrySource::detectDriverId() const {
    for (const bool exact : {true, false}) {
        for (const std::string& alias : driverIdAliases()) {
            const std::string normalized = normalizeChannelName(alias);
            if (!exact && normalized.size() < 4) continue;
            for (const RawChannel& channel : channels_) {
                if (channel.samples.empty()) continue;
                const std::string name = normalizeChannelName(channel.name);
                const bool hit =
                    exact ? name == normalized
                          : name.find(normalized) != std::string::npos;
                if (hit) return dominantDriverId(channel.samples);
            }
        }
    }
    return 0;
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
        std::vector<double> values;
        values.reserve(count);
        for (size_t index = 0; index < count; ++index) {
            double value = 0.0;
            const double time = double(index) / double(frequency);
            if (!sampleAt(size_t(id), time, &value)) break;
            values.push_back(value);
        }
        return {std::move(values), frequency};
    };

    int lapBeaconId = firstId({"lap_beacon_trig", "laptrigger", "lap_beacon"});
    int lapNumberId = firstId({"lap number"});
    int lapDistanceId = firstId({"lap distance corrected", "lap distance"});
    int lapTimeId = firstId({"lap time"});
    int previousLapTimeId = firstId({"previous lap time"});

    auto [lapBeacon, beaconFreq] = series(lapBeaconId);
    auto [lapNumber, numberFreq] = series(lapNumberId);
    auto [lapDistance, distanceFreq] = series(lapDistanceId);
    auto [lapTime, timeFreq] = series(lapTimeId);
    auto [prevLapTime, prevFreq] = series(previousLapTimeId);

    double maxDuration = 0.0;
    for (const RawChannel& channel : channels_)
        maxDuration = std::max(maxDuration, channel.durationSec);

    std::vector<double> splitTimes;
    if (!lapBeacon.empty()) splitTimes = pdsBeaconSplits(lapBeacon, beaconFreq);
    if (splitTimes.size() < 2 && !lapTime.empty())
        splitTimes = pdsLapTimeSplits(lapTime, timeFreq);
    if (splitTimes.size() < 2 && !lapNumber.empty())
        splitTimes = pdsLapNumberSplits(lapNumber, numberFreq);
    if (splitTimes.size() < 2 && !lapDistance.empty())
        splitTimes = pdsDistanceSplits(lapDistance, distanceFreq);

    std::vector<Lap> laps = buildLapsFromSplits(splitTimes, maxDuration);
    if (!prevLapTime.empty())
        laps =
            pdsApplyPreviousLapTimes(laps, prevLapTime, std::max(1, prevFreq));
    return laps;
}

std::vector<Lap> detectLapsLightweight(const std::string& path, int* driverId) {
    void* handle = omatrack_open(path.c_str());
    if (!handle) return {};

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
        result.first.reserve(count);
        for (size_t index = 0; index < count; ++index) {
            double value = 0.0;
            const uint64_t timeNs = uint64_t(
                std::llround(double(index) * 1e9 / double(result.second)));
            if (!omatrack_sample_at(handle, size_t(id), timeNs, 1, &value))
                break;
            result.first.push_back(value);
        }
        return result;
    };

    const int beaconId =
        firstId({"lap_beacon_trig", "laptrigger", "lap_beacon"});
    const int lapNumberId = firstId({"lap number"});
    const int lapDistanceId =
        firstId({"lap distance corrected", "lap distance"});
    const int lapTimeId = firstId({"lap time"});
    const int previousLapTimeId = firstId({"previous lap time"});
    const int driverIdChannel = firstId(driverIdAliases());

    auto [driverValues, driverFreq] = decodeRaw(driverIdChannel);
    (void)driverFreq;
    if (driverId) {
        const int detected = dominantDriverId(driverValues);
        if (detected > 0) *driverId = detected;
    }
    auto [beacon, beaconFreq] = sampleRegular(beaconId);
    auto [lapNumber, numberFreq] = sampleRegular(lapNumberId);
    auto [lapDistance, distanceFreq] = sampleRegular(lapDistanceId);
    auto [lapTime, timeFreq] = sampleRegular(lapTimeId);
    auto [previousLapTime, previousFreq] = sampleRegular(previousLapTimeId);

    double maxDuration = 0.0;
    for (size_t i = 0; i < channelCount; ++i)
        maxDuration = std::max(
            maxDuration, double(omatrack_channel_duration_ns(handle, i)) / 1e9);

    std::vector<double> splits;
    if (!beacon.empty()) splits = pdsBeaconSplits(beacon, beaconFreq);
    if (splits.size() < 2 && !lapTime.empty())
        splits = pdsLapTimeSplits(lapTime, timeFreq);
    if (splits.size() < 2 && !lapNumber.empty())
        splits = pdsLapNumberSplits(lapNumber, numberFreq);
    if (splits.size() < 2 && !lapDistance.empty())
        splits = pdsDistanceSplits(lapDistance, distanceFreq);

    std::vector<Lap> laps = buildLapsFromSplits(splits, maxDuration);
    if (!previousLapTime.empty())
        laps = pdsApplyPreviousLapTimes(laps, previousLapTime,
                                        std::max(1, previousFreq));

    omatrack_close(handle);
    return laps;
}

UnifiedLap TelemetrySource::unifyLap(double startTime, double endTime) const {
    auto mapping = mapChannels();

    auto has = [&](const std::string& f) { return mapping.count(f) != 0; };

    double duration = endTime - startTime;
    int nSamples = int(duration * kDefaultSampleRate) + 1;

    // Sample each channel on the shared 50 Hz absolute-time grid. Source
    // chunks may start late or contain acquisition gaps; slicing flattened
    // arrays by index silently shifts every event after a gap.
    std::map<std::string, std::vector<double>> resampled;
    std::map<std::string, const RawChannel*> resampledCh;
    for (auto& [field, idx] : mapping) {
        const RawChannel& channel = channels_[idx];
        if (channel.samples.empty()) continue;
        std::vector<double> values(size_t(nSamples), 0.0);
        bool sampled = false;
        for (int sample = 0; sample < nSamples; ++sample) {
            double value = 0.0;
            const double time =
                startTime + double(sample) / double(kDefaultSampleRate);
            if (sampleAt(size_t(idx), time, &value)) {
                values[size_t(sample)] = value;
                sampled = true;
            } else if (sample > 0) {
                values[size_t(sample)] = values[size_t(sample - 1)];
            }
        }
        if (!sampled) continue;
        resampled[field] = std::move(values);
        resampledCh[field] = &channel;
    }

    auto get = [&](const std::string& f, int i) -> double {
        auto it = resampled.find(f);
        if (it == resampled.end() || i >= (int)it->second.size()) return 0;
        return it->second[i];
    };
    auto unitOf = [&](const std::string& f) -> std::string {
        auto it = resampledCh.find(f);
        return it != resampledCh.end() ? lowerTrimmed(it->second->unit) : "";
    };

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
        if (has("speed")) {
            std::string u = unitOf("speed");
            auto su = speedUnits().find(u);
            double factor = su != speedUnits().end()
                                ? su->second
                                : (format_ == "aimd" ? 1.0 : 3.6);
            unified.speed.push_back(get("speed", i) * factor);
        } else {
            unified.speed.push_back(0);
        }

        // throttle → 0-1
        double th = get("throttle", i);
        std::string thu = unitOf("throttle");
        if (thu == "rad")
            th /= 1.7453292519943295;
        else if (thu == "deg")
            th /= 100;
        if (th > 1.5) th /= 100;
        unified.throttle.push_back(std::max(0.0, std::min(1.0, th)));

        // brake → bar
        if (has("brake")) {
            std::string u = unitOf("brake");
            auto bu = brakeUnits().find(u);
            double factor = bu != brakeUnits().end() ? bu->second : 1.0;
            unified.brake.push_back(std::max(0.0, get("brake", i) * factor));
        } else {
            double bp = get("brake_pos", i);
            if (bp > 1.5) bp /= 100;
            unified.brake.push_back(std::max(0.0, bp * 100));
        }

        // clutch → 0-1
        double clutch = get("clutch", i);
        std::string cu = unitOf("clutch");
        if (cu == "rad")
            clutch /= 1.7453292519943295;
        else if (cu == "deg")
            clutch /= 100;
        if (clutch > 1.5) clutch /= 100;
        unified.clutch.push_back(std::max(0.0, std::min(1.0, clutch)));

        // steering → deg
        double steer = get("steering", i);
        std::string su2 = unitOf("steering");
        if (su2 == "rad") steer *= 180.0 / kPi;
        unified.steering.push_back(steer);

        // gear
        unified.gear.push_back(
            std::max(0, int(std::llround(get("gear", i))) - gearOffset));

        // driver throttle
        double dth = get("driver_throttle", i);
        std::string dtu = unitOf("driver_throttle");
        if (dtu == "rad")
            dth /= 1.7453292519943295;
        else if (dtu == "deg")
            dth /= 100;
        if (dth > 1.5) dth /= 100;
        unified.driverThrottle.push_back(std::max(0.0, std::min(1.0, dth)));

        unified.gForceLong.push_back(get("g_long", i));
        unified.damperFL.push_back(get("damper_fl", i));
        unified.damperFR.push_back(get("damper_fr", i));
        unified.damperRL.push_back(get("damper_rl", i));
        unified.damperRR.push_back(get("damper_rr", i));

        // GPS: radians → degrees when unit says rad
        double rawLat = get("gps_lat", i);
        double rawLon = get("gps_lon", i);
        if (unitOf("gps_lat").find("rad") != std::string::npos) {
            rawLat *= 180.0 / kPi;
            rawLon *= 180.0 / kPi;
        }
        unified.gpsLat.push_back(rawLat);
        unified.gpsLon.push_back(rawLon);
        double positionAccuracy = get("gps_position_accuracy", i);
        const std::string positionAccuracyUnit =
            unitOf("gps_position_accuracy");
        if (positionAccuracyUnit == "cm")
            positionAccuracy *= 0.01;
        else if (positionAccuracyUnit == "mm")
            positionAccuracy *= 0.001;
        else if (positionAccuracyUnit == "ft")
            positionAccuracy *= 0.3048;
        unified.gpsPositionAccuracy.push_back(std::max(0.0, positionAccuracy));

        double speedAccuracy = get("gps_speed_accuracy", i);
        const auto speedAccuracyUnit =
            speedUnits().find(unitOf("gps_speed_accuracy"));
        if (speedAccuracyUnit != speedUnits().end())
            speedAccuracy *= speedAccuracyUnit->second / 3.6;
        unified.gpsSpeedAccuracy.push_back(std::max(0.0, speedAccuracy));

        double gpsSpeed = get("gps_speed", i);
        const auto gpsSpeedUnit = speedUnits().find(unitOf("gps_speed"));
        if (gpsSpeedUnit != speedUnits().end())
            gpsSpeed *= gpsSpeedUnit->second / 3.6;
        else if (format_ != "aimd")
            gpsSpeed /= 3.6;
        gpsSpeedMps.push_back(std::max(0.0, gpsSpeed));
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
            if (has("gps_speed_accuracy") && accuracy > 0.0) {
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
    if (values.empty()) return out;
    if (std::fabs(srcFreq - targetFreq) < 1e-9) return values;
    int nOut = int(duration * targetFreq) + 1;
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
