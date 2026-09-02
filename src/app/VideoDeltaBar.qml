pragma ComponentBehavior: Bound
import Omatrack

// iRacing-style live delta: the number is accumulated time versus the
// reference at this station; the bar colour is whether that gap is
// improving right now (relative speed). Green is gaining, red is losing.
// A car can be behind on the number and still show green. Drag to move,
// bottom-right or the wheel to scale; default is centred 15 % from the
// top of the fullscreen video.

import QtQuick

OverlayCard {
    id: deltaBar

    readonly property real barHeight: 18 * deltaBar.userScale
    readonly property real barWidth: 260 * deltaBar.userScale
    readonly property real baseBarWidth: 260
    readonly property color dimRateColor: Qt.rgba(deltaBar.rateColor.r, deltaBar.rateColor.g, deltaBar.rateColor.b, 0.38)
    property real dragOriginX: 0
    property real dragOriginY: 0
    readonly property real fillRatio: Number.isFinite(deltaBar.timeDelta) ? Math.min(1, Math.abs(deltaBar.timeDelta) / deltaBar.fullScale) : 0
    readonly property real fillWidth: track.width * 0.5 * deltaBar.fillRatio
    readonly property real fullScale: 1
    readonly property bool gaining: Number.isFinite(deltaBar.speedDelta) ? deltaBar.speedDelta >= 0 : Number.isFinite(deltaBar.timeDelta) && deltaBar.timeDelta <= 0
    readonly property real maxScale: 2.8
    readonly property real minScale: 0.55
    readonly property color rateColor: {
        const base = deltaBar.gaining ? Style.throttleTelemetryColor : Style.brakeTelemetryColor;
        if (!Number.isFinite(deltaBar.speedDelta))
            return base;
        const mix = Math.min(1, Math.abs(deltaBar.speedDelta) / 5);
        const floor = 0.55;
        const t = floor + (1 - floor) * mix;
        return Qt.rgba(Style.mutedTextColor.r + (base.r - Style.mutedTextColor.r) * t, Style.mutedTextColor.g + (base.g - Style.mutedTextColor.g) * t, Style.mutedTextColor.b + (base.b - Style.mutedTextColor.b) * t, 1);
    }
    property real resizeOriginScale: 1
    property real speedDelta: Number.NaN
    property real timeDelta: Number.NaN
    readonly property string timeText: {
        if (!Number.isFinite(deltaBar.timeDelta))
            return "";
        if (deltaBar.timeDelta > 0)
            return "+" + deltaBar.timeDelta.toFixed(2);
        return deltaBar.timeDelta.toFixed(2);
    }
    property bool userPositioned: false
    property real userScale: 1

    function clampScale(value: real): real {
        return Math.max(deltaBar.minScale, Math.min(deltaBar.maxScale, value));
    }
    function clampX(value: real): real {
        if (!deltaBar.parent)
            return 0;
        return Math.max(0, Math.min(deltaBar.parent.width - deltaBar.width, value));
    }
    function clampY(value: real): real {
        if (!deltaBar.parent)
            return 0;
        return Math.max(0, Math.min(deltaBar.parent.height - deltaBar.height, value));
    }
    function refresh(): void {
        deltaBar.timeDelta = Store.cursorTimeDelta();
        deltaBar.speedDelta = Store.cursorSpeedDelta();
    }

    border.width: 0
    clip: false
    color: Qt.rgba(0, 0, 0, 0)
    dragEnabled: !resizeHover.hovered
    height: track.height + 4 * deltaBar.userScale + readout.implicitHeight
    objectName: "videoDeltaBar"
    opacity: Number.isFinite(deltaBar.timeDelta) ? 1 : 0
    radius: 0
    width: deltaBar.barWidth
    z: 9

    Component.onCompleted: deltaBar.refresh()
    onDragBegun: {
        deltaBar.dragOriginX = deltaBar.x;
        deltaBar.dragOriginY = deltaBar.y;
        deltaBar.userPositioned = true;
    }
    onDragMoved: (x, y) => {
        deltaBar.x = deltaBar.clampX(deltaBar.dragOriginX + x);
        deltaBar.y = deltaBar.clampY(deltaBar.dragOriginY + y);
    }

    Binding {
        property: "x"
        restoreMode: Binding.RestoreNone
        target: deltaBar
        value: deltaBar.parent ? (deltaBar.parent.width - deltaBar.width) * 0.5 : 0
        when: !deltaBar.userPositioned
    }
    Binding {
        property: "y"
        restoreMode: Binding.RestoreNone
        target: deltaBar
        value: deltaBar.parent ? deltaBar.parent.height * 0.15 : 0
        when: !deltaBar.userPositioned
    }
    Connections {
        function onCursorReadoutChanged(): void {
            deltaBar.refresh();
        }
        function onReferenceAlignmentChanged(): void {
            deltaBar.refresh();
        }
        function onSelectionChanged(): void {
            deltaBar.refresh();
        }

        target: Store
    }
    Connections {
        function onHeightChanged(): void {
            if (deltaBar.userPositioned)
                deltaBar.y = deltaBar.clampY(deltaBar.y);
        }
        function onWidthChanged(): void {
            if (deltaBar.userPositioned)
                deltaBar.x = deltaBar.clampX(deltaBar.x);
        }

        target: deltaBar.parent
    }
    Rectangle {
        id: track

        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        color: Qt.rgba(0, 0, 0, 0.78)
        height: deltaBar.barHeight
        radius: height / 2
        width: deltaBar.barWidth

        Rectangle {
            clip: true
            color: Qt.rgba(0, 0, 0, 0)
            height: track.height
            radius: track.radius
            width: track.width

            Rectangle {
                id: fill

                height: parent.height
                visible: deltaBar.fillWidth > 0.5
                width: deltaBar.fillWidth
                x: deltaBar.timeDelta < 0 ? parent.width * 0.5 - deltaBar.fillWidth : parent.width * 0.5

                gradient: Gradient {
                    orientation: Gradient.Horizontal

                    GradientStop {
                        color: deltaBar.timeDelta < 0 ? deltaBar.rateColor : deltaBar.dimRateColor
                        position: 0
                    }
                    GradientStop {
                        color: deltaBar.timeDelta < 0 ? deltaBar.dimRateColor : deltaBar.rateColor
                        position: 1
                    }
                }
            }
        }
        Rectangle {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.verticalCenter: parent.verticalCenter
            color: Qt.rgba(0, 0, 0, 1)
            height: track.height
            width: Math.max(2, Math.round(2 * deltaBar.userScale))
        }
    }
    Text {
        id: readout

        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: track.bottom
        anchors.topMargin: 4 * deltaBar.userScale
        color: deltaBar.rateColor
        font.bold: true
        font.family: Style.monoFontFamily
        font.pixelSize: Math.max(12, Math.round(22 * deltaBar.userScale))
        text: deltaBar.timeText
    }
    WheelHandler {
        onWheel: event => {
            deltaBar.userScale = deltaBar.clampScale(deltaBar.userScale * (event.angleDelta.y > 0 ? 1.08 : 1 / 1.08));
            if (deltaBar.userPositioned) {
                deltaBar.x = deltaBar.clampX(deltaBar.x);
                deltaBar.y = deltaBar.clampY(deltaBar.y);
            }
            event.accepted = true;
        }
    }
    Item {
        id: resizeHandle

        anchors.bottom: parent.bottom
        anchors.right: parent.right
        height: 18
        width: 18

        HoverHandler {
            id: resizeHover

            cursorShape: Qt.SizeFDiagCursor
        }
        DragHandler {
            target: null

            onActiveChanged: {
                if (active)
                    deltaBar.resizeOriginScale = deltaBar.userScale;
            }
            onTranslationChanged: {
                deltaBar.userScale = deltaBar.clampScale(deltaBar.resizeOriginScale + translation.x / deltaBar.baseBarWidth);
                if (deltaBar.userPositioned) {
                    deltaBar.x = deltaBar.clampX(deltaBar.x);
                    deltaBar.y = deltaBar.clampY(deltaBar.y);
                }
            }
        }
    }
}
