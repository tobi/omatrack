// Pure primary/reference lap alignment for the comparison views.
//
// TelemetryStore owns the QML-facing caches (comparisonAlignmentTime_,
// comparisonAlignmentFraction_, comparisonAlignmentBasis_, and
// comparisonGpsAnchors_) and rebuilds them when the selected lap pair changes.
// This helper owns only the calculation — lap-progress (distance) alignment
// at the 50 Hz unified grid, speed-landmark DTW when distance is missing,
// GPS position matching only when progress is unavailable, and the
// fraction remap —
// so it can be exercised headless with synthetic omatrack::UnifiedLap inputs.
// The store moves the result into its caches, preserving every threshold,
// basis string, anchor count, and monotonicity guarantee.

#pragma once

#include <QString>
#include <QVector>

namespace omatrack {
struct UnifiedLap;
}

// One pass of comparison alignment for a primary and a reference (compare) lap.
// Fields mirror TelemetryStore's caches verbatim.
struct ComparisonAlignmentResult {
    // Compare-lap time (seconds) for every primary sample, in primary order.
    QVector<double> time;
    // Compare-lap fraction (0-1) for every primary sample, in primary order.
    QVector<double> fraction;
    // Human-readable alignment basis (empty when alignment is impossible).
    QString basis;
    // Number of distributed GPS anchors applied (0 unless GPS refinement ran).
    int gpsAnchors = 0;
};

// Compute the full comparison alignment. Pure: no globals, no I/O, no Qt event
// loop. Returns an empty result (empty basis, zero anchors) when either lap is
// too short to align.
ComparisonAlignmentResult computeComparisonAlignment(
    const omatrack::UnifiedLap& primary, const omatrack::UnifiedLap& compare);

// Map a (basis, gpsAnchors) pair to the confidence label exposed to QML:
// "NONE" (no alignment), "HIGH" (GPS-anchored with enough anchors), "MED"
// (lap progress, validated lap distance, or speed landmarks), or "LOW"
// (sample-index fallback).
QString comparisonAlignmentConfidenceLabel(const QString& basis,
                                           int gpsAnchors);

// Look up the compare-lap fraction for a primary-lap fraction. An empty or
// degenerate map is identity: the caller must not treat "not yet aligned"
// as "the reference is at the start of the lap".
double interpolateAlignmentFraction(const QVector<double>& map,
                                    double primaryFraction);

// Inverse of interpolateAlignmentFraction. Same identity fallback.
double invertAlignmentFraction(const QVector<double>& map,
                               double compareFraction);
