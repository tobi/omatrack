#pragma once

#include <QObject>
#include <QtQml/qqmlregistration.h>

#include <algorithm>
#include <cmath>

namespace omatrack {

// A pit stop gets one fixed cell however long the car stood; every driven
// interval (out, in, flying, fragment) shares the rest by driving time.
constexpr double kFilmstripPitStopCell = 36.0;

struct FilmstripCells {
    double spacing = 0;
    double fixed = 0;
    double minimum = 0;
    double flexible = 0;
};

inline FilmstripCells filmstripCells(double width, int count, int fixed) {
    if (!std::isfinite(width) || width <= 0 || count <= 0) return {};
    fixed = std::clamp(fixed, 0, count);
    const int variable = count - fixed;
    const double selectable = std::min(1.0, width / count);
    // Keep fixed cells at their size before spending pixels on gaps. Dense
    // sessions tighten spacing rather than shrinking the pit-stop cells.
    const double fixedWidth =
        fixed > 0
            ? std::max(0.0, std::min(kFilmstripPitStopCell,
                                     (width - variable * selectable) / fixed))
            : 0;
    const double gap =
        count > 1 ? std::min(3.0, std::max(0.0, (width - fixed * fixedWidth -
                                                 variable * selectable) /
                                                    (count - 1)))
                  : 0;
    const double usable = std::max(0.0, width - gap * (count - 1));
    const double remaining = std::max(0.0, usable - fixed * fixedWidth);
    const double minimum =
        variable > 0 ? std::min(12.0, remaining / variable) : 0;
    return {gap, fixedWidth, minimum,
            std::max(0.0, remaining - variable * minimum)};
}
inline double filmstripCellWidth(double width, int count, int fixed,
                                 bool isFixed, double weight) {
    const auto cells = filmstripCells(width, count, fixed);
    return isFixed
               ? cells.fixed
               : cells.minimum + cells.flexible * std::clamp(weight, 0.0, 1.0);
}
/// `fixedBefore` counts fixed cells left of `index`; `offset` is the share
/// of flexible width taken by the variable cells before it.
inline double filmstripCellX(double width, int count, int fixed, int index,
                             int fixedBefore, double offset) {
    const auto cells = filmstripCells(width, count, fixed);
    fixedBefore = std::clamp(fixedBefore, 0, std::max(0, index));
    return fixedBefore * cells.fixed + index * cells.spacing +
           (index - fixedBefore) * cells.minimum +
           cells.flexible * std::clamp(offset, 0.0, 1.0);
}

// The fullscreen strip sits above a stable controls lane. Reuse existing
// bottom letterboxing when it fits; otherwise reserve space by reducing the
// video viewport rather than covering driving footage. PiP needs reservation
// because its small player occupies the main player's lower letterbox.
inline double filmstripReservedHeight(double width, double height,
                                      double primaryAspect,
                                      double referenceAspect, int mode,
                                      double stripHeight,
                                      double controlsHeight) {
    if (width <= 0 || height <= 0 || stripHeight <= 0) return 0;
    const double needed = std::min(height, stripHeight + controlsHeight + 16.0);
    const auto letterbox = [height](double paneWidth, double aspect) {
        if (!std::isfinite(aspect) || aspect <= 0) return 0.0;
        return std::max(0.0,
                        (height - std::min(height, paneWidth / aspect)) / 2.0);
    };
    double space = 0.0;
    if (mode == 1) {
        const double paneWidth = std::max(0.0, (width - 2.0) / 2.0);
        space = std::min(letterbox(paneWidth, primaryAspect),
                         letterbox(paneWidth, referenceAspect));
    } else if (mode == 4) {
        space = letterbox(width, primaryAspect);
    } else if (mode == 5) {
        space = letterbox(width, referenceAspect);
    }
    return space >= needed ? 0.0 : needed;
}

}  // namespace omatrack

class FilmstripLayout : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
public:
    explicit FilmstripLayout(QObject* parent = nullptr) : QObject(parent) {}

    Q_INVOKABLE double cellWidth(double width, int count, int fixed,
                                 bool isFixed, double weight) const {
        return omatrack::filmstripCellWidth(width, count, fixed, isFixed,
                                            weight);
    }
    Q_INVOKABLE double cellX(double width, int count, int fixed, int index,
                             int fixedBefore, double offset) const {
        return omatrack::filmstripCellX(width, count, fixed, index, fixedBefore,
                                        offset);
    }
    Q_INVOKABLE double reservedHeight(double width, double height,
                                      double primaryAspect,
                                      double referenceAspect, int mode,
                                      double stripHeight,
                                      double controlsHeight) const {
        return omatrack::filmstripReservedHeight(width, height, primaryAspect,
                                                 referenceAspect, mode,
                                                 stripHeight, controlsHeight);
    }
};
