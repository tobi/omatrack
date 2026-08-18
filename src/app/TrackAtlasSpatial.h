#pragma once

#include <QByteArray>
#include <QPointF>
#include <QVector>

namespace omatrack {
struct UnifiedLap;
}

namespace omatrack::trackatlas {

bool hasPositionalGps(const UnifiedLap& lap);

// QJson centerline parsing stays in the app layer (depends on QtJson).
QVector<QPointF> parseCenterline(const QByteArray& payload);

// Thin adapters over the Qt-free core; convert QVector<QPointF> <-> Point.
QVector<QPointF> spatialStationMap(const UnifiedLap& lap,
                                   const QVector<QPointF>& centerline);
double lapFractionAtStation(const QVector<QPointF>& mapping, double station);

}  // namespace omatrack::trackatlas
