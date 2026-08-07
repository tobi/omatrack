pragma ComponentBehavior: Bound
import Omatrack

import QtQuick

Item {
    id: overlay

    function clampPosition(): void {
        overlay.x = Math.max(8, Math.min(overlay.parent.width - overlay.width - 8, overlay.x));
        overlay.y = Math.max(8, Math.min(overlay.parent.height - overlay.height - 48, overlay.y));
    }

    height: width * 0.26
    objectName: "videoTelemetryOverlay"
    width: Math.min(parent.width - 16, 1000, Math.max(520, parent.width * 0.72))
    x: 24
    y: 64
    z: 8

    onHeightChanged: {
        if (overlay.visible)
            overlay.clampPosition();
    }
    onVisibleChanged: {
        if (overlay.visible)
            overlay.clampPosition();
    }
    onWidthChanged: {
        if (overlay.visible)
            overlay.clampPosition();
    }

    Connections {
        function onHeightChanged(): void {
            if (overlay.visible)
                overlay.clampPosition();
        }
        function onWidthChanged(): void {
            if (overlay.visible)
                overlay.clampPosition();
        }

        target: overlay.parent
    }
    VideoTelemetryHud {
        anchors.fill: parent
        backgroundColor: Qt.rgba(0, 0, 0, 0.84)
        brakeColor: Style.redColor
        compareColor: Style.orangeColor
        foregroundColor: Style.foregroundColor
        monoFontFamily: Style.monoFontFamily
        mutedColor: Style.mutedTextColor
        primaryColor: Style.greenColor
        store: Store
    }
    MouseArea {
        anchors.fill: parent
        cursorShape: drag.active ? Qt.ClosedHandCursor : Qt.OpenHandCursor
        drag.maximumX: overlay.parent.width - overlay.width - 8
        drag.maximumY: overlay.parent.height - overlay.height - 48
        drag.minimumX: 8
        drag.minimumY: 8
        drag.target: overlay

        onReleased: {
            overlay.clampPosition();
        }
    }
}
