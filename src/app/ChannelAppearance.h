#pragma once

#include <QColor>
#include <QString>
#include <QVariantMap>

#include <algorithm>
#include <cmath>

struct ChannelAppearance {
    double strokeWidth = 1.25;  // logical screen pixels, independent of zoom
    double fillOpacity = 0.0;   // peak alpha; fades to zero at the baseline
    QColor referenceColor = QColor(QStringLiteral("#e09d7f"));

    static ChannelAppearance defaults(const QString& key) {
        ChannelAppearance style;
        if (key == QStringLiteral("throttle") ||
            key == QStringLiteral("brake") || key == QStringLiteral("clutch"))
            style.fillOpacity = 0.28;
        if (key == QStringLiteral("delta")) style.fillOpacity = 0.20;
        if (key == QStringLiteral("g_long") || key == QStringLiteral("gps_lon"))
            style.referenceColor = QColor(QStringLiteral("#d3c6aa"));
        return style;
    }

    static ChannelAppearance fromMap(const QString& key,
                                     const QVariantMap& map) {
        ChannelAppearance style = defaults(key);
        bool ok = false;
        const double width =
            map.value(QStringLiteral("stroke_width")).toDouble(&ok);
        if (ok && std::isfinite(width))
            style.strokeWidth = std::clamp(width, 0.5, 4.0);
        const double fill =
            map.value(QStringLiteral("fill_opacity")).toDouble(&ok);
        if (ok && std::isfinite(fill))
            style.fillOpacity = std::clamp(fill, 0.0, 1.0);
        const QColor reference(
            map.value(QStringLiteral("reference_color")).toString());
        if (reference.isValid()) style.referenceColor = reference;
        return style;
    }

    void writeTo(QVariantMap& map) const {
        map.insert(QStringLiteral("stroke_width"), strokeWidth);
        map.insert(QStringLiteral("fill_opacity"), fillOpacity);
        map.insert(QStringLiteral("reference_color"),
                   referenceColor.name(QColor::HexRgb));
    }

    bool operator==(const ChannelAppearance& other) const {
        return strokeWidth == other.strokeWidth &&
               fillOpacity == other.fillOpacity &&
               referenceColor == other.referenceColor;
    }
};
