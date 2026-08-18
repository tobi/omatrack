#include "ComparisonAlignment.h"

#include "core/TelemetryEngine.h"

#include <vector>

ComparisonAlignmentResult computeComparisonAlignment(
    const omatrack::UnifiedLap& primary, const omatrack::UnifiedLap& compare,
    const ComparisonAlignmentOptions& options) {
    omatrack::alignment::Options coreOptions;
    coreOptions.strategy = options.strategy;
    coreOptions.cornerStarts.reserve(size_t(options.cornerStarts.size()));
    for (double start : options.cornerStarts)
        coreOptions.cornerStarts.push_back(start);

    const omatrack::alignment::Result core =
        omatrack::alignment::compute(primary, compare, coreOptions);

    ComparisonAlignmentResult result;
    result.time = QVector<double>(core.time.begin(), core.time.end());
    result.fraction =
        QVector<double>(core.fraction.begin(), core.fraction.end());
    result.basis = QString::fromStdString(core.basis);
    result.gpsAnchors = core.gpsAnchors;
    result.rejectionReason = QString::fromStdString(core.rejectionReason);
    return result;
}

bool comparisonGpsAlignmentAvailable(const omatrack::UnifiedLap& primary,
                                     const omatrack::UnifiedLap& compare) {
    return omatrack::alignment::gpsAvailable(primary, compare);
}

bool comparisonDamperAlignmentAvailable(const omatrack::UnifiedLap& primary,
                                        const omatrack::UnifiedLap& compare) {
    return omatrack::alignment::damperAvailable(primary, compare);
}

QString comparisonAlignmentConfidenceLabel(const QString& basis,
                                           int gpsAnchors) {
    return QString::fromStdString(
        omatrack::alignment::confidenceLabel(basis.toStdString(), gpsAnchors));
}

double interpolateAlignmentFraction(const QVector<double>& map,
                                    double primaryFraction) {
    return omatrack::alignment::interpolateFraction(
        std::vector<double>(map.begin(), map.end()), primaryFraction);
}

double invertAlignmentFraction(const QVector<double>& map,
                               double compareFraction) {
    return omatrack::alignment::invertFraction(
        std::vector<double>(map.begin(), map.end()), compareFraction);
}
