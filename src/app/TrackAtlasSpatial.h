#pragma once

#include <QByteArray>
#include <QPointF>
#include <QVector>

namespace omatrack {
struct UnifiedLap;

namespace trackatlas {

bool hasPositionalGps(const UnifiedLap& lap);
QVector<QPointF> parseCenterline(const QByteArray& payload);
QVector<QPointF> spatialStationMap(const UnifiedLap& lap,
                                   const QVector<QPointF>& centerline);
double lapFractionAtStation(const QVector<QPointF>& mapping, double station);

}  // namespace trackatlas
}  // namespace omatrack
