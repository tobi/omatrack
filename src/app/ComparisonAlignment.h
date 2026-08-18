// Thin Qt adapter over the Qt-free core comparison alignment
// (src/core/ComparisonAlignment.h). Exposes the same QVector/QString API the
// store and tests already consume; all computation lives in the core.

#pragma once

#include <QString>
#include <QVector>

#include "core/ComparisonAlignment.h"  // ComparisonAlignmentStrategy

namespace omatrack {
struct UnifiedLap;
}

struct ComparisonAlignmentOptions {
    ComparisonAlignmentStrategy strategy =
        ComparisonAlignmentStrategy::GpsContinuous;
    // Primary-lap sample fractions at the starts of configured corners.
    QVector<double> cornerStarts;
};

// Qt-facing result mirroring the core result with QVector/QString fields.
struct ComparisonAlignmentResult {
    QVector<double> time;
    QVector<double> fraction;
    QString basis;
    int gpsAnchors = 0;
    QString rejectionReason;
};

ComparisonAlignmentResult computeComparisonAlignment(
    const omatrack::UnifiedLap& primary, const omatrack::UnifiedLap& compare,
    const ComparisonAlignmentOptions& options = {});

bool comparisonGpsAlignmentAvailable(const omatrack::UnifiedLap& primary,
                                     const omatrack::UnifiedLap& compare);
bool comparisonDamperAlignmentAvailable(const omatrack::UnifiedLap& primary,
                                        const omatrack::UnifiedLap& compare);

QString comparisonAlignmentConfidenceLabel(const QString& basis,
                                           int gpsAnchors);

double interpolateAlignmentFraction(const QVector<double>& map,
                                    double primaryFraction);
double invertAlignmentFraction(const QVector<double>& map,
                               double compareFraction);
