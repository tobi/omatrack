pragma ComponentBehavior: Bound
import Omatrack

import QtQuick

Item {
    id: overlay

    readonly property real aspectRatio: 0.21
    readonly property real scaleFactor: 0.65
    readonly property real unscaledWidth: Math.min(overlay.parent.width - 16, 1000, Math.max(520, overlay.parent.width * 0.72))

    height: overlay.width * overlay.aspectRatio
    objectName: "videoTelemetryOverlay"
    width: Math.max(0, overlay.unscaledWidth * overlay.scaleFactor)
    x: (overlay.parent.width - overlay.width) * 0.5
    y: Math.max(0, Math.min(overlay.parent.height - overlay.height, overlay.parent.height * 0.9 - overlay.height * 0.5))
    z: 8

    VideoTelemetryHud {
        anchors.fill: parent
        backgroundColor: Qt.rgba(0, 0, 0, 0.84)
        brakeColor: Style.brakeTelemetryColor
        compareColor: Style.orangeColor
        foregroundColor: Style.foregroundColor
        monoFontFamily: Style.monoFontFamily
        mutedColor: Style.mutedTextColor
        steeringColor: Style.steeringTelemetryColor
        store: Store
        throttleColor: Style.throttleTelemetryColor
    }
}
