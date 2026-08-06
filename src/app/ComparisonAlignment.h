// Pure primary/reference lap alignment for the comparison views.
//
// TelemetryStore owns the QML-facing caches (comparisonAlignmentTime_,
// comparisonAlignmentFraction_, comparisonAlignmentBasis_, and
// comparisonGpsAnchors_) plus the lazy ensure/invalidate lifecycle. This
// helper owns only the calculation — speed-landmark DTW, the distance/progress
// fallback, GPS anchor refinement, and the fraction remap — so it can be
// exercised headless with synthetic omatrack::UnifiedLap inputs. The store
// delegates here and copies the result into its caches, preserving every
// threshold, basis string, anchor count, and monotonicity guarantee.

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
// (speed landmarks or validated lap distance), or "LOW" (everything else).
QString comparisonAlignmentConfidenceLabel(const QString& basis,
                                           int gpsAnchors);
