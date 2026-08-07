// Internal helpers extracted from TelemetryEngine.cpp for unit testing.
//
// These are the pure-logic functions that implement lap detection, channel
// matching, and driver-ID resolution. They live in omatrack::detail so tests
// can call them directly without going through a file-backed TelemetrySource.
// Production code should use the public TelemetrySource API instead.

#pragma once

#include <string>
#include <vector>

#include "TelemetryEngine.h"  // Lap

namespace omatrack::detail {

// ── lap split detection ─────────────────────────────────────────────

/// Rising-edge splits from a beacon/trigger channel (1 sample per pulse).
std::vector<double> pdsBeaconSplits(const std::vector<double>& values,
                                    int freq);

/// Splits from a cumulative lap-time channel (detects backward jumps > 5 s).
std::vector<double> pdsLapTimeSplits(const std::vector<double>& values,
                                     int freq);

/// Splits from a lap-number channel (detects positive-to-next-positive
/// increments; zero/dropout recovery only re-establishes counter state).
std::vector<double> pdsLapNumberSplits(const std::vector<double>& values,
                                       int freq);

/// Whether a lap-number signal carries authoritative non-zero state.
bool lapNumberCarriesState(const std::vector<double>& values);

/// Select the most authoritative boundary source. An active lap-number signal
/// wins over beacon/timer/distance heuristics; with fewer than two crossings,
/// no completed lap is fabricated from a weaker signal.
std::vector<double> selectLapSplits(const std::vector<double>& beaconSplits,
                                    const std::vector<double>& lapNumberSplits,
                                    bool lapNumberActive,
                                    const std::vector<double>& lapTimeSplits,
                                    const std::vector<double>& distanceSplits);

/// Splits from a cumulative lap-distance channel (detects resets > 300 m).
std::vector<double> pdsDistanceSplits(const std::vector<double>& values,
                                      int freq);

// ── lap construction ────────────────────────────────────────────────

/// Build laps from split times. Head/tail fragments are marked incomplete;
/// heuristic crossings much shorter than the session median can be rejected.
std::vector<Lap> buildLapsFromSplits(const std::vector<double>& splitTimes,
                                     double duration,
                                     bool rejectShortCrossings = true);

/// Override lap times from a "previous lap time" channel when it agrees with
/// the crossing-derived estimate; heuristic contradictory crossings can be
/// rejected without downgrading authoritative lap-counter boundaries.
std::vector<Lap> pdsApplyPreviousLapTimes(
    const std::vector<Lap>& laps,
    const std::vector<double>& previousLapTimeValues, int freq,
    bool rejectMismatches = true);

/// Reject crossing pairs that cover substantially less of a lap-position
/// signal than the best-supported crossing pair in the same recording.
std::vector<Lap> pdsApplyLapDistanceCoverage(
    const std::vector<Lap>& laps, const std::vector<double>& lapDistanceValues,
    int freq);

// ── channel matching ────────────────────────────────────────────────

/// Score how well a channel name matches an alias (higher = better).
/// Exact normalized match > substring containment > no match.
int scoreChannelMatch(const std::string& channelName, const std::string& alias,
                      int aliasPriority);

// ── driver ID ───────────────────────────────────────────────────────

/// Most frequent positive integer in a driver-ID series; ties go to the
/// earlier one. Returns 0 when no positive values exist.
int dominantDriverId(const std::vector<double>& values);

}  // namespace omatrack::detail
