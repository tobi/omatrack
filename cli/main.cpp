// racecraft-cli — headless verification tool for the racecraft-qt engine.
//
//   racecraft-cli parse <file>   → validates parsing: channels, mapping, laps.
//   racecraft-cli unify <file>   → builds 50 Hz UnifiedLap for fastest lap and
//                                  writes {file}.unified.csv.
//
// Exit code is the acceptance signal: 0 = success, non-zero = failure.

#include "core/TelemetryEngine.h"

#include <cstdio>
#include <string>

using namespace racecraft;

static int cmdParse(const std::string& path) {
    auto src = TelemetrySource::open(path);
    if (!src) return 1;

    printf("format: %s\n", src->formatName().c_str());
    if (src->formatName() == "aimd")
        printf("media time offset: %.6fs\n", src->mediaTimeOffsetSec());
    printf("channels: %zu\n", src->channels().size());
    for (size_t i = 0; i < src->channels().size(); ++i) {
        const auto& ch = src->channels()[i];
        printf("  [%zu] %-40s %-10s freq=%5.1fHz n=%zu dur=%.1fs\n", i, ch.name.c_str(),
               ch.unit.c_str(), ch.frequencyHz, ch.samples.size(), ch.durationSec);
    }

    auto mapping = src->mapChannels();
    printf("mapping:\n");
    for (auto& [field, idx] : mapping) {
        printf("  %-14s -> %s\n", field.c_str(), src->channels()[idx].name.c_str());
    }

    auto laps = src->detectLaps();
    printf("laps: %zu\n", laps.size());
    for (size_t i = 0; i < laps.size(); ++i) {
        printf("  #%d %s  %8.3fs -> %8.3fs  %s\n", laps[i].id,
               formatLapTime(laps[i].timeMs).c_str(), laps[i].startTime,
               laps[i].endTime,
               laps[i].complete ? "" : "(partial: out/in fragment)");
    }

    int failures = 0;
    if (src->channels().empty()) {
        printf("FAIL: no channels\n");
        failures++;
    }
    if (!mapping.count("speed")) {
        printf("FAIL: no speed channel mapped\n");
        failures++;
    }
    if (laps.size() < 2) {
        printf("FAIL: fewer than two laps detected (%zu)\n", laps.size());
        failures++;
    }
    // ever-present IMSA LMP2 channels we know must exist in this dataset family
    for (const char* required : {"speed", "gear", "throttle", "brake"}) {
        if (!mapping.count(required)) {
            printf("WARN: %s channel not mapped\n", required);
        }
    }
    if (failures) {
        printf("parse: FAILED (%d)\n", failures);
        return 1;
    }
    printf("parse: OK\n");
    return 0;
}

static int cmdUnify(const std::string& path) {
    auto src = TelemetrySource::open(path);
    if (!src) return 1;

    auto laps = src->detectLaps();
    if (laps.empty()) {
        printf("FAIL: no laps to unify\n");
        return 1;
    }
    // pick the fastest complete-ish lap (id != 0 preferred)
    int best = 0;
    double bestMs = 1e18;
    for (size_t i = 0; i < laps.size(); ++i) {
        if (laps[i].timeMs < bestMs && laps[i].timeMs > 30000) {
            bestMs = laps[i].timeMs;
            best = int(i);
        }
    }
    const Lap& lap = laps[best];
    printf("unify: lap %d  %s  [%.3f, %.3f]\n", lap.id, formatLapTime(lap.timeMs).c_str(),
           lap.startTime, lap.endTime);

    UnifiedLap u = src->unifyLap(lap.startTime, lap.endTime);
    printf("unified: %zu samples @ %d Hz\n", u.size(), u.sampleRate);

    int failures = 0;
    auto nonEmpty = [&](const char* label, size_t n) {
        printf("  %-14s n=%zu\n", label, n);
        if (n == 0) {
            printf("FAIL: %s empty\n", label);
            failures++;
        }
    };
    nonEmpty("speed", u.speed.size());
    nonEmpty("throttle", u.throttle.size());
    nonEmpty("brake", u.brake.size());
    nonEmpty("steering", u.steering.size());
    nonEmpty("gear", u.gear.size());
    nonEmpty("distance", u.distance.size());

    // sanity: sample speed should be plausible for a racing lap
    double peak = 0;
    for (double s : u.speed) peak = std::max(peak, s);
    printf("  peak speed: %.1f km/h\n", peak);
    if (peak < 40 || peak > 400) {
        printf("FAIL: implausible peak speed %.1f km/h\n", peak);
        failures++;
    }

    // CSV dump
    std::string csvPath = path + ".unified.csv";
    FILE* f = fopen(csvPath.c_str(), "w");
    if (!f) {
        printf("FAIL: cannot write %s\n", csvPath.c_str());
        return 1;
    }
    fprintf(f, "time,speed,throttle,driverThrottle,brake,clutch,steering,gear,distance,gForceLong\n");
    for (size_t i = 0; i < u.size(); ++i) {
        fprintf(f, "%.3f,%.3f,%.4f,%.4f,%.3f,%.4f,%.3f,%d,%.2f,%.4f\n", u.time[i], u.speed[i],
                u.throttle[i], u.driverThrottle[i], u.brake[i], u.clutch[i], u.steering[i],
                u.gear[i], u.distance[i], u.gForceLong[i]);
    }
    fclose(f);
    printf("wrote %s\n", csvPath.c_str());

    if (failures) {
        printf("unify: FAILED (%d)\n", failures);
        return 1;
    }
    printf("unify: OK\n");
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr,
                "usage: %s parse|unify <file.pds|file.ld|file.vbo>\n"
                "  parse — validate channel parsing + lap detection (exit 0 on success)\n"
                "  unify — build 50 Hz UnifiedLap for fastest lap, dump CSV\n",
                argv[0]);
        return 2;
    }
    std::string cmd = argv[1];
    std::string path = argv[2];
    if (cmd == "parse") return cmdParse(path);
    if (cmd == "unify") return cmdUnify(path);
    fprintf(stderr, "unknown command: %s\n", cmd.c_str());
    return 2;
}
