#pragma once

#include <QObject>
#include <QtQml/qqmlregistration.h>

#include <algorithm>
#include <cmath>

namespace omatrack {

struct FilmstripCells {
    double spacing = 0;
    double bookend = 0;
    double minimum = 0;
    double flexible = 0;
};

inline FilmstripCells filmstripCells(double width, int count, int fixed) {
    if (!std::isfinite(width) || width <= 0 || count <= 0) return {};
    fixed = std::clamp(fixed, 0, count);
    const int variable = count - fixed;
    const double selectable = std::min(1.0, width / count);
    // Keep bookends fixed before spending pixels on gaps. Dense sessions
    // tighten spacing rather than making their Out/In widths disagree.
    const double bookend =
        fixed > 0 ? std::min(72.0, (width - variable * selectable) / fixed) : 0;
    const double gap =
        count > 1 ? std::min(3.0, std::max(0.0, (width - fixed * bookend -
                                                 variable * selectable) /
                                                    (count - 1)))
                  : 0;
    const double usable = std::max(0.0, width - gap * (count - 1));
    const double remaining = std::max(0.0, usable - fixed * bookend);
    const double minimum =
        variable > 0 ? std::min(12.0, remaining / variable) : 0;
    return {gap, bookend, minimum,
            std::max(0.0, remaining - variable * minimum)};
}
inline double filmstripCellWidth(double width, int count, int fixed, int edge,
                                 double weight) {
    const auto cells = filmstripCells(width, count, fixed);
    return edge ? cells.bookend
                : cells.minimum + cells.flexible * std::clamp(weight, 0.0, 1.0);
}
inline double filmstripCellX(double width, int count, int fixed, bool leading,
                             int index, int edge, double offset) {
    const auto cells = filmstripCells(width, count, fixed);
    if (edge < 0) return 0;
    if (edge > 0) return std::max(0.0, width - cells.bookend);
    const int variableBefore = std::max(0, index - int(leading));
    return (leading ? cells.bookend : 0) + index * cells.spacing +
           variableBefore * cells.minimum +
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

    Q_INVOKABLE double cellWidth(double width, int count, int fixed, int edge,
                                 double weight) const {
        return omatrack::filmstripCellWidth(width, count, fixed, edge, weight);
    }
    Q_INVOKABLE double cellX(double width, int count, int fixed, bool leading,
                             int index, int edge, double offset) const {
        return omatrack::filmstripCellX(width, count, fixed, leading, index,
                                        edge, offset);
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
