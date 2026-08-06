// omatrack-cli — headless inspection tool for the Omatrack engine.
//
//   omatrack-cli parse <file>
//   omatrack-cli unify <file> --output <csv>
//
// Exit code is the acceptance signal: 0 = success, non-zero = failure.

#include "core/TelemetryEngine.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>

using namespace omatrack;

static int cmdParse(const std::string& path) {
    auto src = TelemetrySource::open(path);
    if (!src) return 1;

    printf("format: %s\n", src->formatName().c_str());
    if (src->formatName() == "aimd")
        printf("media time offset: %.6fs\n", src->mediaTimeOffsetSec());
    printf("channels: %zu\n", src->channels().size());
    for (size_t i = 0; i < src->channels().size(); ++i) {
        const auto& ch = src->channels()[i];
        printf("  [%zu] %-40s %-10s freq=%5.1fHz n=%zu dur=%.1fs\n", i,
               ch.name.c_str(), ch.unit.c_str(), ch.frequencyHz,
               ch.samples.size(), ch.durationSec);
    }

    auto mapping = src->mapChannels();
    printf("mapping:\n");
    for (auto& [field, idx] : mapping) {
        printf("  %-14s -> %s\n", field.c_str(),
               src->channels()[idx].name.c_str());
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
    if (laps.empty()) {
        printf("FAIL: no laps detected\n");
        failures++;
    }
    for (const char* conceptName : {"speed", "gear", "throttle", "brake"}) {
        if (!mapping.count(conceptName))
            printf("INFO: optional %s concept not mapped\n", conceptName);
    }
    if (failures) {
        printf("parse: FAILED (%d)\n", failures);
        return 1;
    }
    printf("parse: OK\n");
    return 0;
}

static int cmdUnify(const std::string& path, const std::string& outputPath) {
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
    printf("unify: lap %d  %s  [%.3f, %.3f]\n", lap.id,
           formatLapTime(lap.timeMs).c_str(), lap.startTime, lap.endTime);

    UnifiedLap u = src->unifyLap(lap.startTime, lap.endTime);
    printf("unified: %zu samples @ %d Hz\n", u.size(), u.sampleRate);

    int failures = 0;
    auto report = [](const char* label, size_t n) {
        printf("  %-14s n=%zu\n", label, n);
    };
    report("speed", u.speed.size());
    report("throttle", u.throttle.size());
    report("brake", u.brake.size());
    report("steering", u.steering.size());
    report("gear", u.gear.size());
    report("distance", u.distance.size());
    printf("  distance source: %s\n", u.distanceSource == DistanceSource::Native
                                          ? "native"
                                          : "speed-fused");
    if (u.size() == 0 || u.speed.size() != u.size() ||
        u.distance.size() != u.size()) {
        printf("FAIL: required unified arrays are missing or misaligned\n");
        failures++;
    }

    double peak = 0.0;
    for (double speed : u.speed) peak = std::max(peak, speed);
    printf("  peak speed: %.1f km/h\n", peak);
    if (failures) {
        printf("unify: FAILED (%d)\n", failures);
        return 1;
    }

    if (std::filesystem::exists(outputPath)) {
        printf("FAIL: refusing to overwrite %s\n", outputPath.c_str());
        return 1;
    }
    FILE* f = fopen(outputPath.c_str(), "w");
    if (!f) {
        printf("FAIL: cannot write %s\n", outputPath.c_str());
        return 1;
    }
    fprintf(f,
            "time,speed,throttle,driverThrottle,brake,clutch,steering,gear,"
            "distance,gForceLong,gpsLat,gpsLon,gpsPositionAccuracy,"
            "gpsSpeedAccuracy\n");
    for (size_t i = 0; i < u.size(); ++i) {
        auto writeDouble = [&](const std::vector<double>& values,
                               const char* format) {
            fputc(',', f);
            if (i < values.size()) fprintf(f, format, values[i]);
        };
        fprintf(f, "%.3f", u.time[i]);
        writeDouble(u.speed, "%.3f");
        writeDouble(u.throttle, "%.4f");
        writeDouble(u.driverThrottle, "%.4f");
        writeDouble(u.brake, "%.3f");
        writeDouble(u.clutch, "%.4f");
        writeDouble(u.steering, "%.3f");
        fputc(',', f);
        if (i < u.gear.size()) fprintf(f, "%d", u.gear[i]);
        writeDouble(u.distance, "%.2f");
        writeDouble(u.gForceLong, "%.4f");
        writeDouble(u.gpsLat, "%.8f");
        writeDouble(u.gpsLon, "%.8f");
        writeDouble(u.gpsPositionAccuracy, "%.3f");
        writeDouble(u.gpsSpeedAccuracy, "%.3f");
        fputc('\n', f);
    }
    bool writeFailed = ferror(f) != 0;
    if (fclose(f) != 0) writeFailed = true;
    if (writeFailed) {
        std::filesystem::remove(outputPath);
        printf("FAIL: incomplete export removed: %s\n", outputPath.c_str());
        return 1;
    }
    printf("wrote %s\n", outputPath.c_str());

    printf("unify: OK\n");
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr,
                "usage:\n"
                "  %s parse <file.pds|file.ld|file.vbo|file.mp4>\n"
                "  %s unify <file> --output <csv>\n\n"
                "unify exports location-bearing GPS fields when available; "
                "choose an explicit output path and handle it as sensitive "
                "data.\n",
                argv[0], argv[0]);
        return 2;
    }
    const std::string cmd = argv[1];
    if (cmd == "parse" && argc == 3) return cmdParse(argv[2]);
    if (cmd == "unify" && argc == 5 && std::string(argv[3]) == "--output")
        return cmdUnify(argv[2], argv[4]);
    fprintf(stderr, "invalid arguments; run %s without arguments for usage\n",
            argv[0]);
    return 2;
}
