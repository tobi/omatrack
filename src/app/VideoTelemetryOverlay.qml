pragma ComponentBehavior: Bound
import Omatrack

import QtQuick

Item {
    id: overlay

    readonly property real aspectRatio: 0.21
    property bool manuallyPositioned: false
    readonly property real minimumHudWidth: 480

    function clampPosition(): void {
        const availableWidth = Math.max(280, overlay.parent.width - 16);
        const availableHeight = Math.max(59, overlay.parent.height - 56);
        const maximumWidth = Math.min(availableWidth, availableHeight / overlay.aspectRatio);
        if (overlay.width > maximumWidth)
            overlay.width = maximumWidth;
        overlay.x = Math.max(8, Math.min(overlay.parent.width - overlay.width - 8, overlay.x));
        overlay.y = Math.max(8, Math.min(overlay.parent.height - overlay.height - 48, overlay.y));
    }
    function resizeTo(parentX: real): void {
        const maximumWidth = Math.max(280, overlay.parent.width - overlay.x - 8);
        const minimumWidth = Math.min(overlay.minimumHudWidth, maximumWidth);
        overlay.width = Math.max(minimumWidth, Math.min(maximumWidth, parentX - overlay.x));
        overlay.clampPosition();
    }
    function setInitialPosition(): void {
        overlay.x = (overlay.parent.width - overlay.width) / 2;
        overlay.y = overlay.parent.height * 0.8 - overlay.height;
        overlay.clampPosition();
    }

    height: overlay.width * overlay.aspectRatio
    objectName: "videoTelemetryOverlay"
    width: Math.min(overlay.parent.width - 16, 1000, Math.max(520, overlay.parent.width * 0.72))
    x: 0
    y: 0
    z: 8

    onHeightChanged: {
        if (!overlay.visible)
            return;
        if (overlay.manuallyPositioned)
            overlay.clampPosition();
        else
            overlay.setInitialPosition();
    }
    onVisibleChanged: {
        if (!overlay.visible)
            return;
        if (overlay.manuallyPositioned)
            overlay.clampPosition();
        else
            overlay.setInitialPosition();
    }
    onWidthChanged: {
        if (!overlay.visible)
            return;
        if (overlay.manuallyPositioned)
            overlay.clampPosition();
        else
            overlay.setInitialPosition();
    }

    Connections {
        function onHeightChanged(): void {
            if (!overlay.visible)
                return;
            if (overlay.manuallyPositioned)
                overlay.clampPosition();
            else
                overlay.setInitialPosition();
        }
        function onWidthChanged(): void {
            if (!overlay.visible)
                return;
            if (overlay.manuallyPositioned)
                overlay.clampPosition();
            else
                overlay.setInitialPosition();
        }

        target: overlay.parent
    }
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
    MouseArea {
        anchors.fill: parent
        cursorShape: drag.active ? Qt.ClosedHandCursor : Qt.OpenHandCursor
        drag.maximumX: overlay.parent.width - overlay.width - 8
        drag.maximumY: overlay.parent.height - overlay.height - 48
        drag.minimumX: 8
        drag.minimumY: 8
        drag.target: overlay

        onPressed: overlay.manuallyPositioned = true
        onReleased: {
            overlay.clampPosition();
        }
    }
    MouseArea {
        id: resizeHandle

        anchors.bottom: parent.bottom
        anchors.right: parent.right
        cursorShape: Qt.SizeFDiagCursor
        height: 28
        width: 28

        onPositionChanged: mouse => {
            if (!resizeHandle.pressed)
                return;
            const position = resizeHandle.mapToItem(overlay.parent, mouse.x, mouse.y);
            overlay.resizeTo(position.x);
        }
        onPressed: overlay.manuallyPositioned = true

        Rectangle {
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 7
            anchors.right: parent.right
            anchors.rightMargin: 3
            color: Style.foregroundColor
            height: 1
            opacity: 0.65
            rotation: -45
            width: 13
        }
        Rectangle {
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 5
            anchors.right: parent.right
            anchors.rightMargin: 2
            color: Style.foregroundColor
            height: 1
            opacity: 0.65
            rotation: -45
            width: 7
        }
    }
}
