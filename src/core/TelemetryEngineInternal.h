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

/// Splits from a lap-number channel (detects increments).
std::vector<double> pdsLapNumberSplits(const std::vector<double>& values,
                                       int freq);

/// Splits from a cumulative lap-distance channel (detects resets > 300 m).
std::vector<double> pdsDistanceSplits(const std::vector<double>& values,
                                      int freq);

// ── lap construction ────────────────────────────────────────────────

/// Build laps from split times. Head/tail fragments are marked incomplete;
/// crossing pairs much shorter than the session median are filtered out.
std::vector<Lap> buildLapsFromSplits(const std::vector<double>& splitTimes,
                                     double duration);

/// Override lap times from a "previous lap time" channel when within 30 s
/// of the crossing-derived estimate.
std::vector<Lap> pdsApplyPreviousLapTimes(
    const std::vector<Lap>& laps,
    const std::vector<double>& previousLapTimeValues, int freq);

// ── channel matching ────────────────────────────────────────────────

/// Score how well a channel name matches an alias (higher = better).
/// Exact normalized match > substring containment > no match.
int scoreChannelMatch(const std::string& channelName,
                      const std::string& alias, int aliasPriority);

// ── driver ID ───────────────────────────────────────────────────────

/// Most frequent positive integer in a driver-ID series; ties go to the
/// earlier one. Returns 0 when no positive values exist.
int dominantDriverId(const std::vector<double>& values);

}  // namespace omatrack::detail
