pragma ComponentBehavior: Bound
import Omatrack

// Corner focus information overlay, anchored to the right side of the trace
// pane by its parent (Main.qml). Fades in when Store.focusedCorner >= 0.
//
// Layout is a game-style report card, not a four-column table: a hero time
// chip, then fixed three-column rows (label · graphic · signed value). Speed
// uses magnitude bars; brake / turn-in / throttle use a centre-zero gauge.
// Owns its own cache: `summary` is refreshed from a Connections block.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

OverlayCard {
    id: overlay

    readonly property bool comparing: overlay.summary.hasCompare === true
    property real dragOriginX: 0
    property real dragOriginY: 0
    property real dragX: 0
    property real dragY: 0
    readonly property real pointScale: overlay.maxAbs([overlay.summary.brakePointDelta, overlay.summary.turnInDelta, overlay.summary.throttlePointDelta], 40)
    readonly property real soloSpeedScale: overlay.maxAbs([overlay.summary.entrySpeed, overlay.summary.apexSpeed, overlay.summary.exitSpeed], 1)
    readonly property real speedScale: overlay.maxAbs([overlay.summary.entryDelta, overlay.summary.apexDelta, overlay.summary.exitDelta], 12)
    property var summary: ({})

    function brakePointDeltaText(value): string {
        if (value === undefined || value === null || !isFinite(Number(value)))
            return "—";
        const metres = Number(value);
        if (Math.abs(metres) < 0.5)
            return "matched";
        return Math.abs(metres).toFixed(0) + "m " + (metres > 0 ? "later" : "earlier");
    }
    function clampDragX(value): real {
        const pane = overlay.parent;
        if (!pane)
            return value;
        const minX = 8 - Math.max(0, pane.width - overlay.width - 8);
        return Math.max(minX, Math.min(8, value));
    }
    function clampDragY(value): real {
        const pane = overlay.parent;
        if (!pane)
            return value;
        const maxY = Math.max(-8, pane.height - overlay.height - 8);
        return Math.max(-8, Math.min(maxY, value));
    }
    function clearMarkerHighlight(key): void {
        if (Store.highlightedCornerMarker === key)
            Store.setHighlightedCornerMarker("");
    }
    function fmtPlain(value, decimals, suffix): string {
        if (value === undefined || value === null || !isFinite(Number(value)))
            return "—";
        return Number(value).toFixed(decimals) + suffix;
    }
    function fmtSigned(value, decimals, suffix): string {
        if (value === undefined || value === null || !isFinite(Number(value)))
            return "—";
        const n = Number(value);
        return (n > 0 ? "+" : "") + n.toFixed(decimals) + suffix;
    }
    function maxAbs(values, floor): real {
        let peak = floor;
        for (let i = 0; i < values.length; ++i) {
            const n = Number(values[i]);
            if (isFinite(n))
                peak = Math.max(peak, Math.abs(n));
        }
        return peak;
    }
    function refresh(): void {
        overlay.summary = Store.cornerFocusSummary();
    }
    function setMarkerHighlight(key): void {
        Store.setHighlightedCornerMarker(key);
    }
    function speedDeltaColor(value): color {
        if (value === undefined || value === null || !isFinite(Number(value)))
            return Style.mutedTextColor;
        const n = Number(value);
        if (n > 0)
            return Style.greenColor;
        if (n < 0)
            return Style.redColor;
        return Style.mutedTextColor;
    }
    function timeDeltaColor(value): color {
        if (value === undefined || value === null || !isFinite(Number(value)))
            return Style.mutedTextColor;
        const n = Number(value);
        if (n < 0)
            return Style.greenColor;
        if (n > 0)
            return Style.redColor;
        return Style.mutedTextColor;
    }

    border.color: Style.borderColor
    border.width: 1
    clip: true
    color: Style.surfaceColor
    implicitHeight: overlayBody.implicitHeight + 20
    objectName: "cornerFocusOverlay"
    opacity: Store.focusedCorner >= 0 ? 1 : 0
    radius: 6
    visible: overlay.opacity > 0

    Behavior on opacity {
        NumberAnimation {
            duration: 180
            easing.type: Easing.OutCubic
        }
    }
    transform: Translate {
        x: overlay.dragX
        y: overlay.dragY
    }

    Component.onCompleted: overlay.refresh()
    onDragBegun: {
        overlay.dragOriginX = overlay.dragX;
        overlay.dragOriginY = overlay.dragY;
    }
    onDragMoved: (x, y) => {
        overlay.dragX = overlay.clampDragX(overlay.dragOriginX + x);
        overlay.dragY = overlay.clampDragY(overlay.dragOriginY + y);
    }

    Connections {
        function onCornerConsistencyChanged(): void {
            overlay.refresh();
        }
        function onCornerFocusChanged(): void {
            overlay.refresh();
        }
        function onCornersChanged(): void {
            overlay.refresh();
        }
        function onReferenceAlignmentChanged(): void {
            overlay.refresh();
        }
        function onSelectionChanged(): void {
            overlay.refresh();
        }

        target: Store
    }
    ScrollView {
        id: overlayScroll

        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
        anchors.fill: parent
        anchors.margins: 10
        clip: true
        contentWidth: overlayScroll.availableWidth

        ColumnLayout {
            id: overlayBody

            spacing: 6
            width: overlayScroll.availableWidth

            RowLayout {
                Layout.fillWidth: true
                spacing: 4

                Label {
                    Layout.fillWidth: true
                    color: Style.foregroundColor
                    elide: Text.ElideRight
                    font.bold: true
                    font.pixelSize: Style.fontSize
                    text: overlay.summary.name || "Corner"
                }
                Label {
                    Accessible.name: "Close corner"
                    Accessible.role: Accessible.Button
                    color: Style.mutedTextColor
                    font.pixelSize: Style.fontSize + 4
                    objectName: "cornerFocusClose"
                    text: "×"

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor

                        onClicked: Store.clearCornerFocus()
                    }
                }
            }
            Rectangle {
                id: hero

                Layout.fillWidth: true
                border.color: overlay.comparing ? overlay.timeDeltaColor(overlay.summary.delta) : Style.borderColor
                border.width: 1
                color: {
                    if (!overlay.comparing)
                        return Style.traceBackgroundColor;
                    const n = Number(overlay.summary.delta);
                    if (!isFinite(n) || n === 0)
                        return Style.traceBackgroundColor;
                    const tint = n < 0 ? Style.greenColor : Style.redColor;
                    return Qt.rgba(tint.r, tint.g, tint.b, 0.14);
                }
                implicitHeight: heroColumn.implicitHeight + 12
                radius: 4

                ColumnLayout {
                    id: heroColumn

                    anchors.left: parent.left
                    anchors.margins: 6
                    anchors.right: parent.right
                    anchors.top: parent.top
                    spacing: 2

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        Label {
                            Layout.fillWidth: true
                            color: Style.mutedTextColor
                            font.bold: !overlay.comparing
                            font.family: Style.monoFontFamily
                            font.pixelSize: overlay.comparing ? Style.smallFontSize : Style.fontSize + 4
                            text: overlay.fmtPlain(overlay.summary.time, 3, "s")
                        }
                        Label {
                            color: overlay.timeDeltaColor(overlay.summary.delta)
                            font.bold: true
                            font.family: Style.monoFontFamily
                            font.pixelSize: Style.fontSize + 4
                            text: overlay.fmtSigned(overlay.summary.delta, 3, "s")
                            visible: overlay.comparing
                        }
                    }
                    Label {
                        Layout.fillWidth: true
                        color: Style.mutedTextColor
                        font.pixelSize: Style.smallFontSize
                        text: overlay.fmtSigned(overlay.summary.entryTimeDelta, 3, "s") + " entry  ·  " + overlay.fmtSigned(overlay.summary.exitTimeDelta, 3, "s") + " exit"
                        visible: overlay.comparing
                    }
                    Label {
                        Layout.fillWidth: true
                        color: Style.mutedTextColor
                        font.pixelSize: Style.smallFontSize
                        text: "Select a reference lap to compare"
                        visible: !overlay.comparing
                    }
                }
            }
            SectionLabel {
                text: "SPEED"
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                Label {
                    Layout.preferredWidth: 54
                    color: Style.mutedTextColor
                    font.pixelSize: Style.smallFontSize
                    text: "Entry"
                }
                MagnitudeBar {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 12
                    barScale: overlay.comparing ? overlay.speedScale : overlay.soloSpeedScale
                    signedColors: overlay.comparing
                    value: overlay.comparing ? Number(overlay.summary.entryDelta) : Number(overlay.summary.entrySpeed)
                }
                Label {
                    Layout.preferredWidth: 48
                    color: overlay.comparing ? overlay.speedDeltaColor(overlay.summary.entryDelta) : Style.foregroundColor
                    font.family: Style.monoFontFamily
                    font.pixelSize: Style.smallFontSize
                    horizontalAlignment: Text.AlignRight
                    text: overlay.comparing ? overlay.fmtSigned(overlay.summary.entryDelta, 1, "") : overlay.fmtPlain(overlay.summary.entrySpeed, 1, "")
                }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                Label {
                    Layout.preferredWidth: 54
                    color: Style.mutedTextColor
                    font.pixelSize: Style.smallFontSize
                    text: "Apex"
                }
                MagnitudeBar {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 12
                    barScale: overlay.comparing ? overlay.speedScale : overlay.soloSpeedScale
                    signedColors: overlay.comparing
                    value: overlay.comparing ? Number(overlay.summary.apexDelta) : Number(overlay.summary.apexSpeed)
                }
                Label {
                    Layout.preferredWidth: 48
                    color: overlay.comparing ? overlay.speedDeltaColor(overlay.summary.apexDelta) : Style.foregroundColor
                    font.family: Style.monoFontFamily
                    font.pixelSize: Style.smallFontSize
                    horizontalAlignment: Text.AlignRight
                    text: overlay.comparing ? overlay.fmtSigned(overlay.summary.apexDelta, 1, "") : overlay.fmtPlain(overlay.summary.apexSpeed, 1, "")
                }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                Label {
                    Layout.preferredWidth: 54
                    color: Style.mutedTextColor
                    font.pixelSize: Style.smallFontSize
                    text: "Exit"
                }
                MagnitudeBar {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 12
                    barScale: overlay.comparing ? overlay.speedScale : overlay.soloSpeedScale
                    signedColors: overlay.comparing
                    value: overlay.comparing ? Number(overlay.summary.exitDelta) : Number(overlay.summary.exitSpeed)
                }
                Label {
                    Layout.preferredWidth: 48
                    color: overlay.comparing ? overlay.speedDeltaColor(overlay.summary.exitDelta) : Style.foregroundColor
                    font.family: Style.monoFontFamily
                    font.pixelSize: Style.smallFontSize
                    horizontalAlignment: Text.AlignRight
                    text: overlay.comparing ? overlay.fmtSigned(overlay.summary.exitDelta, 1, "") : overlay.fmtPlain(overlay.summary.exitSpeed, 1, "")
                }
            }
            SectionLabel {
                text: "POINTS"
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                HoverHandler {
                    onHoveredChanged: {
                        if (hovered)
                            overlay.setMarkerHighlight("brake");
                        else
                            overlay.clearMarkerHighlight("brake");
                    }
                }
                Label {
                    Layout.preferredWidth: 54
                    color: Style.mutedTextColor
                    font.pixelSize: Style.smallFontSize
                    text: "Brake"
                }
                OffsetGauge {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 12
                    barScale: overlay.pointScale
                    value: overlay.comparing ? Number(overlay.summary.brakePointDelta) : 0
                    visible: overlay.comparing
                }
                Label {
                    Layout.preferredWidth: 52
                    color: Style.foregroundColor
                    font.family: Style.monoFontFamily
                    font.pixelSize: Style.smallFontSize
                    horizontalAlignment: Text.AlignRight
                    text: overlay.comparing ? overlay.fmtSigned(overlay.summary.brakePointDelta, 0, "m") : overlay.fmtPlain(overlay.summary.brakePoint, 0, "m")
                }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                HoverHandler {
                    onHoveredChanged: {
                        if (hovered)
                            overlay.setMarkerHighlight("turnin");
                        else
                            overlay.clearMarkerHighlight("turnin");
                    }
                }
                Label {
                    Layout.preferredWidth: 54
                    color: Style.mutedTextColor
                    font.pixelSize: Style.smallFontSize
                    text: "Turn-in"
                }
                OffsetGauge {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 12
                    barScale: overlay.pointScale
                    value: overlay.comparing ? Number(overlay.summary.turnInDelta) : 0
                    visible: overlay.comparing
                }
                Label {
                    Layout.preferredWidth: 52
                    color: Style.foregroundColor
                    font.family: Style.monoFontFamily
                    font.pixelSize: Style.smallFontSize
                    horizontalAlignment: Text.AlignRight
                    text: overlay.comparing ? overlay.fmtSigned(overlay.summary.turnInDelta, 0, "m") : overlay.fmtPlain(overlay.summary.turnInPoint, 0, "m")
                }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                HoverHandler {
                    onHoveredChanged: {
                        if (hovered)
                            overlay.setMarkerHighlight("pickup");
                        else
                            overlay.clearMarkerHighlight("pickup");
                    }
                }
                Label {
                    Layout.preferredWidth: 54
                    color: Style.mutedTextColor
                    font.pixelSize: Style.smallFontSize
                    text: "Throttle"
                }
                OffsetGauge {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 12
                    barScale: overlay.pointScale
                    value: overlay.comparing ? Number(overlay.summary.throttlePointDelta) : 0
                    visible: overlay.comparing
                }
                Label {
                    Layout.preferredWidth: 52
                    color: Style.foregroundColor
                    font.family: Style.monoFontFamily
                    font.pixelSize: Style.smallFontSize
                    horizontalAlignment: Text.AlignRight
                    text: overlay.comparing ? overlay.fmtSigned(overlay.summary.throttlePointDelta, 0, "m") : overlay.fmtPlain(overlay.summary.throttlePoint, 0, "m")
                }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                Label {
                    Layout.preferredWidth: 54
                    color: Style.mutedTextColor
                    font.pixelSize: Style.smallFontSize
                    text: "Gear"
                }
                Row {
                    Layout.fillWidth: true
                    spacing: 4

                    Rectangle {
                        border.color: Style.accentColor
                        border.width: 1
                        color: Style.traceBackgroundColor
                        height: 22
                        radius: 3
                        width: 22

                        Label {
                            anchors.centerIn: parent
                            color: Style.accentColor
                            font.bold: true
                            font.family: Style.monoFontFamily
                            font.pixelSize: Style.fontSize
                            text: overlay.fmtPlain(overlay.summary.minGear, 0, "")
                        }
                    }
                    Rectangle {
                        border.color: Style.orangeColor
                        border.width: 1
                        color: Style.traceBackgroundColor
                        height: 22
                        radius: 3
                        visible: overlay.comparing
                        width: 22

                        Label {
                            anchors.centerIn: parent
                            color: Style.orangeColor
                            font.bold: true
                            font.family: Style.monoFontFamily
                            font.pixelSize: Style.fontSize
                            text: overlay.fmtPlain(overlay.summary.compareMinGear, 0, "")
                        }
                    }
                }
                Label {
                    Layout.preferredWidth: 52
                    color: Style.foregroundColor
                    font.family: Style.monoFontFamily
                    font.pixelSize: Style.smallFontSize
                    horizontalAlignment: Text.AlignRight
                    text: overlay.comparing ? overlay.fmtSigned(Number(overlay.summary.minGear) - Number(overlay.summary.compareMinGear), 0, "") : ""
                }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                HoverHandler {
                    onHoveredChanged: {
                        if (hovered)
                            overlay.setMarkerHighlight("turnin");
                        else
                            overlay.clearMarkerHighlight("turnin");
                    }
                }
                Label {
                    Layout.preferredWidth: 54
                    color: Style.mutedTextColor
                    font.pixelSize: Style.smallFontSize
                    text: "Steer"
                }
                MagnitudeBar {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 12
                    barScale: 90
                    signedColors: false
                    value: Number(overlay.summary.maxSteering)
                }
                Label {
                    Layout.preferredWidth: 48
                    color: Style.foregroundColor
                    font.family: Style.monoFontFamily
                    font.pixelSize: Style.smallFontSize
                    horizontalAlignment: Text.AlignRight
                    text: overlay.fmtPlain(overlay.summary.maxSteering, 0, "°")
                }
            }
            SectionLabel {
                text: "CONSISTENCY"
            }
            Label {
                Layout.fillWidth: true
                color: Style.mutedTextColor
                font.family: Style.monoFontFamily
                font.pixelSize: Style.smallFontSize
                text: "Reading " + overlay.summary.consistencyLapCount + (overlay.summary.consistencyLapCount === 1 ? " lap…" : " laps…")
                visible: overlay.summary.consistencyLoading === true
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                visible: overlay.summary.brakeConsistencyAvailable === true

                HoverHandler {
                    onHoveredChanged: {
                        if (hovered)
                            overlay.setMarkerHighlight("brake");
                        else
                            overlay.clearMarkerHighlight("brake");
                    }
                }
                Label {
                    Layout.preferredWidth: 54
                    color: Style.mutedTextColor
                    font.pixelSize: Style.smallFontSize
                    text: "Brake"
                }
                OffsetGauge {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 12
                    barScale: Math.max(Number(overlay.summary.brakePointRange) / 2, Number(overlay.summary.brakePointStdDev) * 2, 5)
                    value: Number(overlay.summary.brakePointVsMedian)
                }
                Label {
                    Layout.preferredWidth: 48
                    color: Style.foregroundColor
                    font.family: Style.monoFontFamily
                    font.pixelSize: Style.smallFontSize
                    horizontalAlignment: Text.AlignRight
                    text: overlay.fmtPlain(overlay.summary.brakePointStdDev, 1, "m σ")
                }
            }
            Label {
                Layout.fillWidth: true
                color: Style.dimTextColor
                font.family: Style.monoFontFamily
                font.pixelSize: Style.smallFontSize
                text: overlay.brakePointDeltaText(overlay.summary.brakePointVsMedian) + " vs median · " + overlay.fmtPlain(overlay.summary.brakePointRange, 0, "m") + " range"
                visible: overlay.summary.brakeConsistencyAvailable === true && isFinite(Number(overlay.summary.brakePointVsMedian))
            }
            Label {
                Layout.fillWidth: true
                color: Style.mutedTextColor
                font.family: Style.monoFontFamily
                font.pixelSize: Style.smallFontSize
                text: overlay.summary.consistencyLapCount > 0 ? "Need at least two top-quartile braking laps" : "No representative timed laps"
                visible: overlay.summary.consistencyLoading !== true && overlay.summary.brakeConsistencyAvailable !== true
                wrapMode: Text.Wrap
            }
            SectionLabel {
                text: "NOTES"
                visible: (overlay.summary.notes ?? []).length > 0
            }
            Repeater {
                model: overlay.summary.notes ?? []

                RowLayout {
                    id: noteRow

                    required property string severity
                    required property string text

                    Layout.fillWidth: true
                    spacing: 6

                    Rectangle {
                        Layout.alignment: Qt.AlignTop
                        Layout.preferredHeight: 5
                        Layout.preferredWidth: 5
                        Layout.topMargin: 4
                        color: noteRow.severity === "error" ? Style.redColor : noteRow.severity === "warning" ? Style.yellowColor : Style.mutedTextColor
                        radius: 2.5
                    }
                    Label {
                        Layout.fillWidth: true
                        color: noteRow.severity === "error" ? Style.redColor : noteRow.severity === "warning" ? Style.yellowColor : Style.mutedTextColor
                        font.pixelSize: Style.smallFontSize
                        text: noteRow.text
                        wrapMode: Text.Wrap
                    }
                }
            }
        }
    }

    component MagnitudeBar: Item {
        id: bar

        required property real barScale
        readonly property color fillColor: {
            const n = Number(bar.value);
            if (!isFinite(n) || n === 0)
                return Style.mutedTextColor;
            if (!bar.signedColors)
                return Style.foregroundColor;
            return n > 0 ? bar.positiveColor : bar.negativeColor;
        }
        readonly property real magnitude: {
            const span = Math.max(Number(bar.barScale), 0.001);
            const n = Number(bar.value);
            if (!isFinite(n))
                return 0;
            return Math.min(1, Math.abs(n) / span);
        }
        property color negativeColor: Style.redColor
        property color positiveColor: Style.greenColor
        property bool signedColors: true
        required property real value

        implicitHeight: 12
        implicitWidth: 80

        Rectangle {
            anchors.fill: parent
            color: Style.traceBackgroundColor
            radius: 2
        }
        Rectangle {
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            color: bar.fillColor
            height: parent.height
            radius: 2
            width: Math.max(parent.width * bar.magnitude, bar.magnitude > 0 ? 2 : 0)
        }
    }
    component OffsetGauge: Item {
        id: gauge

        required property real barScale
        readonly property real mark: {
            const span = Math.max(Number(gauge.barScale), 0.001);
            const n = Number(gauge.value);
            if (!isFinite(n))
                return 0.5;
            return Math.max(0, Math.min(1, 0.5 + 0.5 * n / span));
        }
        required property real value

        implicitHeight: 12
        implicitWidth: 80

        Rectangle {
            anchors.fill: parent
            color: Style.traceBackgroundColor
            radius: 2
        }
        Rectangle {
            anchors.horizontalCenter: parent.horizontalCenter
            color: Style.borderColor
            height: parent.height
            width: 1
        }
        Rectangle {
            color: Style.foregroundColor
            height: parent.height
            radius: 1
            width: 3
            x: gauge.width * gauge.mark - width / 2
        }
    }
    component SectionLabel: Label {
        color: Style.dimTextColor
        font.bold: true
        font.letterSpacing: 0.6
        font.pixelSize: Style.smallFontSize
    }
}
