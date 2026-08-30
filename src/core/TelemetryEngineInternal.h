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

/// Splits from a last/previous-lap-time channel (detects a new posted time).
std::vector<double> pdsLastLapTimeSplits(const std::vector<double>& values,
                                         int freq);

/// Splits from a lap-number channel (detects positive-to-next-positive
/// increments; zero/dropout recovery only re-establishes counter state).
std::vector<double> pdsLapNumberSplits(const std::vector<double>& values,
                                       int freq);

/// Whether a lap-number signal carries authoritative non-zero state.
bool lapNumberCarriesState(const std::vector<double>& values);

/// Select the most authoritative boundary source. An active lap-number
/// signal with at least two crossings wins over beacon/timer/distance.
/// A counter that never increments is not authoritative.
std::vector<double> selectLapSplits(const std::vector<double>& beaconSplits,
                                    const std::vector<double>& lapNumberSplits,
                                    bool lapNumberActive,
                                    const std::vector<double>& lapTimeSplits,
                                    const std::vector<double>& distanceSplits);

/// Splits from a wrapping lap-distance channel. A reset is a drop of more
/// than half the observed peak, so metre, kilometre, percent, and 0-1
/// fraction channels all work (a 300 m floor misses a 4 km oval in km).
std::vector<double> pdsDistanceSplits(const std::vector<double>& values,
                                      int freq);

// ── lap construction ────────────────────────────────────────────────

/// Build laps from split times. Head/tail fragments are marked incomplete;
/// heuristic crossings much shorter than the session median can be rejected.
std::vector<Lap> buildLapsFromSplits(const std::vector<double>& splitTimes,
                                     double duration,
                                     bool rejectShortCrossings = true);

/// Mark complete crossings much shorter than the session median incomplete.
/// Used for both heuristic splits and vendor-supplied lap lists so an out-lap
/// cannot win fastest-lap selection.
void markShortCrossingsIncomplete(std::vector<Lap>& laps);

/// Promote vendor-incomplete laps whose time matches the session median.
/// AiM/Pi often flag oval crossings incomplete when the logger's track map
/// is the road course; upstream still found the laps, so classification
/// (not a second detector) decides complete vs Out/In/Frag.
void restoreRepresentativeCrossings(std::vector<Lap>& laps);

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

/// Most frequent positive numeric code in a driver-ID series; ties go to the
/// earlier one. Float32-backed values are reduced to their meaningful decimal
/// precision so codes such as 2.1 do not expose binary storage noise. Returns
/// 0 when no positive finite values exist.
double dominantDriverId(const std::vector<double>& values,
                        uint32_t sampleTypeCode = 0);

}  // namespace omatrack::detail
