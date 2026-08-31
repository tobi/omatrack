#pragma once

#include <QObject>
#include <QtQml/qqmlregistration.h>

#include <algorithm>
#include <cmath>

namespace omatrack {

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
