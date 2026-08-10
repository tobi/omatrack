pragma ComponentBehavior: Bound
import Omatrack

// Corner focus information overlay, anchored to the right side of the trace
// pane by its parent (Main.qml). Fades in when Store.focusedCorner >= 0 and
// shows a dense primary/reference corner summary pulled from
// Store.cornerFocusSummary(). Owns its own cache: `summary` is refreshed from a
// Connections { target: Store } block whenever the focus, selection, corners,
// or reference alignment change.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: overlay

    property var summary: ({})

    // Plain (unsigned) number with `decimals` and a trailing `suffix`. Returns
    // "—" for missing/non-finite values so every cell degrades gracefully.
    function fmtPlain(value, decimals, suffix): string {
        if (value === undefined || value === null || !isFinite(Number(value)))
            return "—";
        return Number(value).toFixed(decimals) + suffix;
    }

    // Signed number: prepends "+" to positive values so gains/losses read at a
    // glance. Returns "—" for missing/non-finite values.
    function fmtSigned(value, decimals, suffix): string {
        if (value === undefined || value === null || !isFinite(Number(value)))
            return "—";
        const n = Number(value);
        return (n > 0 ? "+" : "") + n.toFixed(decimals) + suffix;
    }
    function refresh(): void {
        overlay.summary = Store.cornerFocusSummary();
    }

    // Speed deltas: +ve = primary faster (green), -ve = primary slower (red).
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

    // Time deltas: +ve = primary loses (red), -ve = primary gains (green).
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
    color: Style.darkBackgroundColor
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

    Component.onCompleted: overlay.refresh()

    Connections {
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

    // The trace pane can be short (a video session halves it), so the panel
    // scrolls its own content rather than clipping numbers away.
    ScrollView {
        id: overlayScroll

        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
        anchors.fill: parent
        anchors.margins: 10
        clip: true
        contentWidth: overlayScroll.availableWidth

        ColumnLayout {
            spacing: 8
            width: overlayScroll.availableWidth

            // ── header ───────────────────────────────────────────────────
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
                ToolButton {
                    objectName: "cornerFocusClose"
                    text: "×"

                    onClicked: Store.clearCornerFocus()
                }
            }
            Label {
                Layout.fillWidth: true
                color: Style.mutedTextColor
                font.pixelSize: Style.smallFontSize
                text: "Select a reference lap to compare"
                visible: overlay.summary.hasCompare !== true
            }

            // ── time ─────────────────────────────────────────────────────
            Label {
                color: Style.dimTextColor
                font.bold: true
                font.pixelSize: Style.smallFontSize
                text: "TIME"
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 6

                Label {
                    Layout.preferredWidth: 56
                    color: Style.mutedTextColor
                    font.pixelSize: Style.smallFontSize
                    text: "Corner"
                }
                Label {
                    Layout.preferredWidth: 64
                    color: Style.foregroundColor
                    font.family: Style.monoFontFamily
                    font.pixelSize: Style.smallFontSize
                    horizontalAlignment: Text.AlignRight
                    text: overlay.fmtPlain(overlay.summary.time, 3, "s")
                }
                Label {
                    Layout.preferredWidth: 64
                    color: Style.mutedTextColor
                    font.family: Style.monoFontFamily
                    font.pixelSize: Style.smallFontSize
                    horizontalAlignment: Text.AlignRight
                    text: overlay.fmtPlain(overlay.summary.compareTime, 3, "s")
                    visible: overlay.summary.hasCompare === true
                }
                Label {
                    Layout.preferredWidth: 64
                    color: overlay.timeDeltaColor(overlay.summary.delta)
                    font.family: Style.monoFontFamily
                    font.pixelSize: Style.smallFontSize
                    horizontalAlignment: Text.AlignRight
                    text: overlay.fmtSigned(overlay.summary.delta, 3, "s")
                    visible: overlay.summary.hasCompare === true
                }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 6
                visible: overlay.summary.hasCompare === true

                Label {
                    Layout.fillWidth: true
                    color: Style.mutedTextColor
                    font.pixelSize: Style.smallFontSize
                    text: "On entry"
                }
                Label {
                    Layout.preferredWidth: 64
                    color: overlay.timeDeltaColor(overlay.summary.entryTimeDelta)
                    font.family: Style.monoFontFamily
                    font.pixelSize: Style.smallFontSize
                    horizontalAlignment: Text.AlignRight
                    text: overlay.fmtSigned(overlay.summary.entryTimeDelta, 3, "s")
                }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 6
                visible: overlay.summary.hasCompare === true

                Label {
                    Layout.fillWidth: true
                    color: Style.mutedTextColor
                    font.pixelSize: Style.smallFontSize
                    text: "On exit"
                }
                Label {
                    Layout.preferredWidth: 64
                    color: overlay.timeDeltaColor(overlay.summary.exitTimeDelta)
                    font.family: Style.monoFontFamily
                    font.pixelSize: Style.smallFontSize
                    horizontalAlignment: Text.AlignRight
                    text: overlay.fmtSigned(overlay.summary.exitTimeDelta, 3, "s")
                }
            }

            // ── speed ────────────────────────────────────────────────────
            Label {
                color: Style.dimTextColor
                font.bold: true
                font.pixelSize: Style.smallFontSize
                text: "SPEED · km/h"
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 6
                visible: overlay.summary.hasCompare === true

                Item {
                    Layout.preferredWidth: 56
                }
                Label {
                    Layout.preferredWidth: 64
                    color: Style.mutedTextColor
                    font.pixelSize: Style.smallFontSize
                    horizontalAlignment: Text.AlignRight
                    text: "Prim"
                }
                Label {
                    Layout.preferredWidth: 64
                    color: Style.mutedTextColor
                    font.pixelSize: Style.smallFontSize
                    horizontalAlignment: Text.AlignRight
                    text: "Ref"
                }
                Label {
                    Layout.preferredWidth: 64
                    color: Style.mutedTextColor
                    font.pixelSize: Style.smallFontSize
                    horizontalAlignment: Text.AlignRight
                    text: "Δ"
                }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 6

                Label {
                    Layout.preferredWidth: 56
                    color: Style.mutedTextColor
                    font.pixelSize: Style.smallFontSize
                    text: "Entry"
                }
                Label {
                    Layout.preferredWidth: 64
                    color: Style.foregroundColor
                    font.family: Style.monoFontFamily
                    font.pixelSize: Style.smallFontSize
                    horizontalAlignment: Text.AlignRight
                    text: overlay.fmtPlain(overlay.summary.entrySpeed, 1, "")
                }
                Label {
                    Layout.preferredWidth: 64
                    color: Style.mutedTextColor
                    font.family: Style.monoFontFamily
                    font.pixelSize: Style.smallFontSize
                    horizontalAlignment: Text.AlignRight
                    text: overlay.fmtPlain(overlay.summary.compareEntrySpeed, 1, "")
                    visible: overlay.summary.hasCompare === true
                }
                Label {
                    Layout.preferredWidth: 64
                    color: overlay.speedDeltaColor(overlay.summary.entryDelta)
                    font.family: Style.monoFontFamily
                    font.pixelSize: Style.smallFontSize
                    horizontalAlignment: Text.AlignRight
                    text: overlay.fmtSigned(overlay.summary.entryDelta, 1, "")
                    visible: overlay.summary.hasCompare === true
                }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 6

                Label {
                    Layout.preferredWidth: 56
                    color: Style.mutedTextColor
                    font.pixelSize: Style.smallFontSize
                    text: "Apex"
                }
                Label {
                    Layout.preferredWidth: 64
                    color: Style.foregroundColor
                    font.family: Style.monoFontFamily
                    font.pixelSize: Style.smallFontSize
                    horizontalAlignment: Text.AlignRight
                    text: overlay.fmtPlain(overlay.summary.apexSpeed, 1, "")
                }
                Label {
                    Layout.preferredWidth: 64
                    color: Style.mutedTextColor
                    font.family: Style.monoFontFamily
                    font.pixelSize: Style.smallFontSize
                    horizontalAlignment: Text.AlignRight
                    text: overlay.fmtPlain(overlay.summary.compareApexSpeed, 1, "")
                    visible: overlay.summary.hasCompare === true
                }
                Label {
                    Layout.preferredWidth: 64
                    color: overlay.speedDeltaColor(overlay.summary.apexDelta)
                    font.family: Style.monoFontFamily
                    font.pixelSize: Style.smallFontSize
                    horizontalAlignment: Text.AlignRight
                    text: overlay.fmtSigned(overlay.summary.apexDelta, 1, "")
                    visible: overlay.summary.hasCompare === true
                }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 6

                Label {
                    Layout.preferredWidth: 56
                    color: Style.mutedTextColor
                    font.pixelSize: Style.smallFontSize
                    text: "Exit"
                }
                Label {
                    Layout.preferredWidth: 64
                    color: Style.foregroundColor
                    font.family: Style.monoFontFamily
                    font.pixelSize: Style.smallFontSize
                    horizontalAlignment: Text.AlignRight
                    text: overlay.fmtPlain(overlay.summary.exitSpeed, 1, "")
                }
                Label {
                    Layout.preferredWidth: 64
                    color: Style.mutedTextColor
                    font.family: Style.monoFontFamily
                    font.pixelSize: Style.smallFontSize
                    horizontalAlignment: Text.AlignRight
                    text: overlay.fmtPlain(overlay.summary.compareExitSpeed, 1, "")
                    visible: overlay.summary.hasCompare === true
                }
                Label {
                    Layout.preferredWidth: 64
                    color: overlay.speedDeltaColor(overlay.summary.exitDelta)
                    font.family: Style.monoFontFamily
                    font.pixelSize: Style.smallFontSize
                    horizontalAlignment: Text.AlignRight
                    text: overlay.fmtSigned(overlay.summary.exitDelta, 1, "")
                    visible: overlay.summary.hasCompare === true
                }
            }

            // ── points ───────────────────────────────────────────────────
            Label {
                color: Style.dimTextColor
                font.bold: true
                font.pixelSize: Style.smallFontSize
                text: "POINTS"
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 6
                visible: overlay.summary.hasCompare === true

                Label {
                    Layout.fillWidth: true
                    color: Style.mutedTextColor
                    font.pixelSize: Style.smallFontSize
                    text: "Brake point"
                }
                Label {
                    Layout.preferredWidth: 64
                    color: Style.foregroundColor
                    font.family: Style.monoFontFamily
                    font.pixelSize: Style.smallFontSize
                    horizontalAlignment: Text.AlignRight
                    text: overlay.fmtSigned(overlay.summary.brakePointDelta, 0, "m")
                }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 6
                visible: overlay.summary.hasCompare === true

                Label {
                    Layout.fillWidth: true
                    color: Style.mutedTextColor
                    font.pixelSize: Style.smallFontSize
                    text: "Turn-in"
                }
                Label {
                    Layout.preferredWidth: 64
                    color: Style.foregroundColor
                    font.family: Style.monoFontFamily
                    font.pixelSize: Style.smallFontSize
                    horizontalAlignment: Text.AlignRight
                    text: overlay.fmtSigned(overlay.summary.turnInDelta, 0, "m")
                }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 6
                visible: overlay.summary.hasCompare === true

                Label {
                    Layout.fillWidth: true
                    color: Style.mutedTextColor
                    font.pixelSize: Style.smallFontSize
                    text: "Throttle"
                }
                Label {
                    Layout.preferredWidth: 64
                    color: Style.foregroundColor
                    font.family: Style.monoFontFamily
                    font.pixelSize: Style.smallFontSize
                    horizontalAlignment: Text.AlignRight
                    text: overlay.fmtSigned(overlay.summary.throttlePointDelta, 0, "m")
                }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 6

                Label {
                    Layout.fillWidth: true
                    color: Style.mutedTextColor
                    font.pixelSize: Style.smallFontSize
                    text: "Min gear"
                }
                Label {
                    Layout.preferredWidth: 64
                    color: Style.foregroundColor
                    font.family: Style.monoFontFamily
                    font.pixelSize: Style.smallFontSize
                    horizontalAlignment: Text.AlignRight
                    text: overlay.fmtPlain(overlay.summary.minGear, 0, "")
                }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 6

                Label {
                    Layout.fillWidth: true
                    color: Style.mutedTextColor
                    font.pixelSize: Style.smallFontSize
                    text: "Max steering"
                }
                Label {
                    Layout.preferredWidth: 64
                    color: Style.foregroundColor
                    font.family: Style.monoFontFamily
                    font.pixelSize: Style.smallFontSize
                    horizontalAlignment: Text.AlignRight
                    text: overlay.fmtPlain(overlay.summary.maxSteering, 0, "°")
                }
            }

            // ── checks ───────────────────────────────────────────────────
            Label {
                color: Style.dimTextColor
                font.bold: true
                font.pixelSize: Style.smallFontSize
                text: "CHECKS"
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
                        color: noteRow.severity === "error" ? Style.redColor : noteRow.severity === "warning" ? Style.orangeColor : Style.mutedTextColor
                        radius: 2.5
                    }
                    Label {
                        Layout.fillWidth: true
                        color: Style.mutedTextColor
                        font.family: Style.monoFontFamily
                        font.pixelSize: Style.smallFontSize
                        text: noteRow.text
                        wrapMode: Text.Wrap
                    }
                }
            }
        }
    }
}
