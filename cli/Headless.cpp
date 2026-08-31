// Headless commands shared by `omatrack` and the test-only `omatrack-cli`.
// See Headless.h for the command surface.

#include "Headless.h"

#include "core/ComparisonAlignment.h"
#include "core/CornerAnalysis.h"
#include "core/TelemetryEngine.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace omatrack;

namespace {

// Representative racing lap: complete, not a pit outlier. Uses the shared
// core classifyLaps() so the CLI and GUI agree on what counts for best.
static int fastestLapIndex(std::vector<Lap>& laps) {
    classifyLaps(laps);
    int best = -1;
    double bestMs = 1e18;
    for (size_t i = 0; i < laps.size(); ++i) {
        if (!laps[i].complete || laps[i].isPitLap || !(laps[i].timeMs > 0.0))
            continue;
        if (laps[i].timeMs < bestMs) {
            bestMs = laps[i].timeMs;
            best = int(i);
        }
    }
    if (best >= 0) return best;
    for (size_t i = 0; i < laps.size(); ++i) {
        if (!laps[i].complete || !(laps[i].timeMs > 0.0)) continue;
        if (laps[i].timeMs < bestMs) {
            bestMs = laps[i].timeMs;
            best = int(i);
        }
    }
    return best >= 0 ? best : 0;
}

static int cmdParse(const std::string& path) {
    std::string error;
    auto src = TelemetrySource::open(path, &error);
    if (!src) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }

    printf("format: %s\n", src->formatName().c_str());
    if (src->utcStartNs() >= 0)
        printf("utc start: %lld ns (Unix epoch)\n",
               static_cast<long long>(src->utcStartNs()));
    else
        printf("utc start: unknown (no wall clock in this recording)\n");
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
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }

    auto laps = src->detectLaps();
    if (laps.empty()) {
        printf("FAIL: no laps to unify\n");
        return 1;
    }
    const int best = fastestLapIndex(laps);
    const Lap& lap = laps[size_t(best)];
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

// Runs the same corner analyzers the GUI runs, on the same unified laps, so a
// check can be developed and regression-tested without a display.
// Lap `id` when requested (-1 picks the fastest), or -1 when absent.
static int lapIndexById(const std::vector<Lap>& laps, int id) {
    for (size_t i = 0; i < laps.size(); ++i)
        if (laps[i].id == id) return int(i);
    return -1;
}

static int cmdCorners(const std::string& path, int lapId,
                      const std::string& referencePath, int referenceLapId,
                      const std::vector<std::pair<double, double>>& zones) {
    std::string error;
    auto src = TelemetrySource::open(path, &error);
    if (!src) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }
    auto laps = src->detectLaps();
    if (laps.empty()) {
        printf("FAIL: no laps\n");
        return 1;
    }
    const int lapIndex =
        lapId < 0 ? fastestLapIndex(laps) : lapIndexById(laps, lapId);
    if (lapIndex < 0) {
        printf("FAIL: no lap %d in active recording\n", lapId);
        return 1;
    }
    const Lap& lap = laps[size_t(lapIndex)];
    const UnifiedLap primary = src->unifyLap(lap.startTime, lap.endTime);
    printf("active: lap %d %s\n", lap.id, formatLapTime(lap.timeMs).c_str());

    std::unique_ptr<TelemetrySource> referenceSource;
    UnifiedLap reference;
    if (!referencePath.empty()) {
        referenceSource = TelemetrySource::open(referencePath, &error);
        if (!referenceSource) {
            std::fprintf(stderr, "error: %s\n", error.c_str());
            return 1;
        }
        auto referenceLaps = referenceSource->detectLaps();
        if (referenceLaps.empty()) {
            printf("FAIL: reference has no laps\n");
            return 1;
        }
        const int pickIndex = referenceLapId < 0
                                  ? fastestLapIndex(referenceLaps)
                                  : lapIndexById(referenceLaps, referenceLapId);
        if (pickIndex < 0) {
            printf("FAIL: no lap %d in reference recording\n", referenceLapId);
            return 1;
        }
        const Lap& pick = referenceLaps[size_t(pickIndex)];
        reference = referenceSource->unifyLap(pick.startTime, pick.endTime);
        printf("reference: lap %d %s\n", pick.id,
               formatLapTime(pick.timeMs).c_str());
    }

    // Map reference zones through the same comparison alignment the GUI uses
    // (GPS-continuous when both laps carry usable fixes, else lap percentage)
    // so the reference corner range is aligned, not assumed identical.
    omatrack::alignment::Result alignment;
    if (!reference.time.empty()) {
        alignment = omatrack::alignment::compute(primary, reference);
        printf("alignment: %s  anchors=%d  confidence=%s%s%s\n",
               alignment.basis.empty() ? "none" : alignment.basis.c_str(),
               alignment.gpsAnchors,
               omatrack::alignment::confidenceLabel(alignment.basis,
                                                    alignment.gpsAnchors)
                   .c_str(),
               alignment.rejectionReason.empty() ? "" : "  rejected: ",
               alignment.rejectionReason.c_str());
    }

    for (const auto& zone : zones) {
        CornerContext context;
        context.primary = &primary;
        context.primaryMetrics =
            measureCorner(primary, zone.first, zone.second);
        if (!reference.time.empty()) {
            context.reference = &reference;
            double refStart = zone.first;
            double refEnd = zone.second;
            if (alignment.fraction.size() == primary.time.size()) {
                refStart = omatrack::alignment::interpolateFraction(
                    alignment.fraction, zone.first);
                refEnd = omatrack::alignment::interpolateFraction(
                    alignment.fraction, zone.second);
            }
            context.referenceMetrics =
                measureCorner(reference, refStart, refEnd);
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

static int cmdCompare(const std::string& leftPath,
                      const std::string& rightPath) {
    std::string error;
    auto left = TelemetrySource::open(leftPath, &error);
    if (!left) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }
    error.clear();
    auto right = TelemetrySource::open(rightPath, &error);
    if (!right) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }
    const std::string report =
        compareTelemetrySources(*left, *right, "aimd", "telemetry");
    std::fputs(report.c_str(), stdout);
    return 0;
}

}  // namespace

namespace omatrack::headless {

bool isCommand(const char* argument) {
    if (!argument) return false;
    static const char* const kCommands[] = {"parse", "unify", "corners",
                                            "compare"};
    for (const char* command : kCommands)
        if (std::strcmp(argument, command) == 0) return true;
    return false;
}

void printUsage(const char* program) {
    fprintf(
        stderr,
        "usage:\n"
        "  %s parse <file.pds|file.ld|file.vbo|file.mp4|file.telemetry>\n"
        "  %s unify <file> --output <csv>\n"
        "  %s corners <file> [--lap N] [--reference <file>] "
        "[--reference-lap N] --zone <start:end> [--zone ...]\n"
        "  %s compare <aimd.mp4> <file.telemetry>\n"
        "  %s --version\n\n"
        "unify exports location-bearing GPS fields when available; "
        "choose an explicit output path and handle it as sensitive "
        "data.\n"
        "corners runs the corner analyzers on the fastest lap (or the "
        "lap ids given); zones are lap fractions.\n"
        "compare dumps GPS, main channels, laps, and video-frame "
        "sync from an AiM extract against its .telemetry companion.\n",
        program, program, program, program, program);
}

int run(int argc, char** argv, const char* program) {
    const std::string cmd = argv[1];
    if (cmd == "parse" && argc == 3) return cmdParse(argv[2]);
    if (cmd == "unify" && argc == 5 && std::string(argv[3]) == "--output")
        return cmdUnify(argv[2], argv[4]);
    if (cmd == "compare" && argc == 4) return cmdCompare(argv[2], argv[3]);
    if (cmd == "corners" && argc >= 3) {
        std::string reference;
        int lapId = -1;
        int referenceLapId = -1;
        std::vector<std::pair<double, double>> zones;
        for (int i = 3; i < argc; ++i) {
            const std::string option = argv[i];
            if (option == "--reference" && i + 1 < argc) {
                reference = argv[++i];
            } else if (option == "--lap" && i + 1 < argc) {
                lapId = std::stoi(argv[++i]);
            } else if (option == "--reference-lap" && i + 1 < argc) {
                referenceLapId = std::stoi(argv[++i]);
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
        return cmdCorners(argv[2], lapId, reference, referenceLapId, zones);
    }
    printUsage(program);
    return 2;
}

}  // namespace omatrack::headless
