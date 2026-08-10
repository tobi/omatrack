// omatrack-cli — headless inspection tool for the Omatrack engine.
//
//   omatrack-cli parse <file>
//   omatrack-cli unify <file> --output <csv>
//   omatrack-cli corners <file> [--reference <file>] --zone <start:end> ...
//
// Exit code is the acceptance signal: 0 = success, non-zero = failure.

#include "core/CornerAnalysis.h"
#include "core/TelemetryEngine.h"

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <filesystem>
#include <string>

using namespace omatrack;

static int cmdParse(const std::string& path) {
    std::string error;
    auto src = TelemetrySource::open(path, &error);
    if (!src) {
        std::fprintf(stderr, "omatrack-cli: %s\n", error.c_str());
        return 1;
    }

    printf("format: %s\n", src->formatName().c_str());
    if (const auto offset = src->videoPresentationOffsetSec())
        printf("video presentation offset: %.6f s\n", *offset);
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
    std::string error;
    auto src = TelemetrySource::open(path, &error);
    if (!src) {
        std::fprintf(stderr, "omatrack-cli: %s\n", error.c_str());
        return 1;
    }

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
            "distance,gForceLong,gForceLat,gpsLat,gpsLon,gpsPositionAccuracy,"
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
        writeDouble(u.gForceLat, "%.4f");
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

// Fastest lap that is long enough to be a real timed lap, mirroring how the
// GUI picks a default.
static int fastestLapIndex(const std::vector<Lap>& laps) {
    int best = 0;
    double bestMs = 1e18;
    for (size_t i = 0; i < laps.size(); ++i) {
        if (laps[i].timeMs < bestMs && laps[i].timeMs > 30000) {
            bestMs = laps[i].timeMs;
            best = int(i);
        }
    }
    return best;
}

// Runs the same corner analyzers the GUI runs, on the same unified laps, so a
// check can be developed and regression-tested without a display.
static int cmdCorners(const std::string& path, const std::string& referencePath,
                      const std::vector<std::pair<double, double>>& zones) {
    std::string error;
    auto src = TelemetrySource::open(path, &error);
    if (!src) {
        std::fprintf(stderr, "omatrack-cli: %s\n", error.c_str());
        return 1;
    }
    auto laps = src->detectLaps();
    if (laps.empty()) {
        printf("FAIL: no laps\n");
        return 1;
    }
    const Lap& lap = laps[size_t(fastestLapIndex(laps))];
    const UnifiedLap primary = src->unifyLap(lap.startTime, lap.endTime);
    printf("active: lap %d %s\n", lap.id, formatLapTime(lap.timeMs).c_str());

    std::unique_ptr<TelemetrySource> referenceSource;
    UnifiedLap reference;
    if (!referencePath.empty()) {
        referenceSource = TelemetrySource::open(referencePath, &error);
        if (!referenceSource) {
            std::fprintf(stderr, "omatrack-cli: %s\n", error.c_str());
            return 1;
        }
        auto referenceLaps = referenceSource->detectLaps();
        if (referenceLaps.empty()) {
            printf("FAIL: reference has no laps\n");
            return 1;
        }
        const Lap& pick = referenceLaps[size_t(fastestLapIndex(referenceLaps))];
        reference = referenceSource->unifyLap(pick.startTime, pick.endTime);
        printf("reference: lap %d %s\n", pick.id,
               formatLapTime(pick.timeMs).c_str());
    }

    for (const auto& zone : zones) {
        CornerContext context;
        context.primary = &primary;
        context.primaryMetrics =
            measureCorner(primary, zone.first, zone.second);
        if (!reference.time.empty()) {
            // Map the zone onto the reference lap by distance, the way the
            // store does, so both metrics describe the same piece of track.
            const auto fractionAtDistance = [](const UnifiedLap& target,
                                               double metres) {
                if (target.distance.size() < 2) return 0.0;
                const auto it = std::lower_bound(target.distance.begin(),
                                                 target.distance.end(), metres);
                if (it == target.distance.end()) return 1.0;
                return double(it - target.distance.begin()) /
                       double(target.distance.size() - 1);
            };
            const double startMetres =
                primary.distance[size_t(context.primaryMetrics.firstIndex)];
            const double endMetres =
                primary.distance[size_t(context.primaryMetrics.lastIndex)];
            context.reference = &reference;
            context.referenceMetrics = measureCorner(
                reference, fractionAtDistance(reference, startMetres),
                fractionAtDistance(reference, endMetres));
        }

        const CornerMetrics& metrics = context.primaryMetrics;
        printf("\nzone %.4f-%.4f  %.1fm  %.3fs\n", zone.first, zone.second,
               metrics.lengthMeters, metrics.time);
        if (!metrics.valid) {
            printf("  (no samples)\n");
            continue;
        }
        printf("  speed   entry %.1f  apex %.1f  exit %.1f km/h\n",
               metrics.entrySpeed, metrics.apexSpeed, metrics.exitSpeed);
        // A point the lap never reached is missing, not zero.
        const auto metres = [](double value) {
            static char text[6][24];
            static int slot = 0;
            char* buffer = text[slot++ % 6];
            if (std::isfinite(value))
                std::snprintf(buffer, sizeof(text[0]), "%.0fm", value);
            else
                std::snprintf(buffer, sizeof(text[0]), "-");
            return buffer;
        };
        printf("  points  brake %-7s turn-in %-7s apex %-7s throttle %s\n",
               metres(metrics.brakePoint), metres(metrics.turnInPoint),
               metres(metrics.apexPoint), metres(metrics.throttlePoint));
        printf(
            "  control gear %d  steer %.0f°  brake %.0f bar  trail %.2fs  "
            "lateral-g %s\n",
            metrics.minGear, metrics.maxSteering, metrics.maxBrake,
            metrics.trailBrakeSeconds, metrics.hasLateralG ? "yes" : "no");

        const std::vector<CornerNote> notes =
            CornerAnalysisRegistry::instance().run(context);
        if (notes.empty()) {
            printf("  notes   (none)\n");
            continue;
        }
        for (const CornerNote& note : notes)
            printf("  %-7s %s [%s]\n", severityName(note.severity),
                   note.text.c_str(), note.id.c_str());
    }
    printf("\ncorners: OK\n");
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr,
                "usage:\n"
                "  %s parse <file.pds|file.ld|file.vbo|file.mp4>\n"
                "  %s unify <file> --output <csv>\n"
                "  %s corners <file> [--reference <file>] "
                "--zone <start:end> [--zone ...]\n\n"
                "unify exports location-bearing GPS fields when available; "
                "choose an explicit output path and handle it as sensitive "
                "data.\n"
                "corners runs the corner analyzers on the fastest lap; zones "
                "are lap fractions.\n",
                argv[0], argv[0], argv[0]);
        return 2;
    }
    const std::string cmd = argv[1];
    if (cmd == "parse" && argc == 3) return cmdParse(argv[2]);
    if (cmd == "unify" && argc == 5 && std::string(argv[3]) == "--output")
        return cmdUnify(argv[2], argv[4]);
    if (cmd == "corners" && argc >= 3) {
        std::string reference;
        std::vector<std::pair<double, double>> zones;
        for (int i = 3; i < argc; ++i) {
            const std::string option = argv[i];
            if (option == "--reference" && i + 1 < argc) {
                reference = argv[++i];
            } else if (option == "--zone" && i + 1 < argc) {
                const std::string value = argv[++i];
                const size_t colon = value.find(':');
                if (colon == std::string::npos) {
                    fprintf(stderr, "--zone wants start:end lap fractions\n");
                    return 2;
                }
                zones.emplace_back(std::stod(value.substr(0, colon)),
                                   std::stod(value.substr(colon + 1)));
            } else {
                fprintf(stderr, "unknown option %s\n", option.c_str());
                return 2;
            }
        }
        if (zones.empty()) {
            fprintf(stderr, "corners needs at least one --zone start:end\n");
            return 2;
        }
        return cmdCorners(argv[2], reference, zones);
    }
    fprintf(stderr, "invalid arguments; run %s without arguments for usage\n",
            argv[0]);
    return 2;
}
