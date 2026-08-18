// Qt-free Track Atlas spatial station mapping. The algorithmic core
// (projection, isotonic regression, station interpolation) lives here;
// the app layer wraps it in a thin QVector/QByteArray adapter for QJson
// parsing and QPointF conversion (src/app/TrackAtlasSpatial.h).

#pragma once

#include <utility>
#include <vector>

namespace omatrack {
struct UnifiedLap;
}

namespace omatrack::trackatlas {

/// A 2D point in (longitude, latitude) or local-metre space.
struct Point {
    double x = 0.0;
    double y = 0.0;
};

bool hasPositionalGps(const UnifiedLap& lap);

/// Build a station-to-lap-fraction map by projecting GPS fixes onto a
/// centerline. Returns an empty vector when the lap or centerline is
/// unsuitable. Each point is (station, fraction).
std::vector<Point> spatialStationMap(const UnifiedLap& lap,
                                     const std::vector<Point>& centerline);

/// Look up the lap fraction at a given station [0,1] from a mapping
/// produced by spatialStationMap. Returns -1.0 for an empty mapping.
double lapFractionAtStation(const std::vector<Point>& mapping, double station);

}  // namespace omatrack::trackatlas
