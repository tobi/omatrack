pragma ComponentBehavior: Bound
import Omatrack

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: toolbar

    required property bool deltaTraceVisible
    required property TraceView trace

    implicitHeight: Style.iconButtonSize

    RowLayout {
        anchors.fill: parent
        spacing: 2

        ToolButton {
            Layout.fillHeight: true
            Layout.preferredWidth: 58
            ToolTip.text: Store.comparing ? (toolbar.deltaTraceVisible ? "Hide comparison delta-time trace" : "Show comparison delta-time trace") : "Select a comparison lap to show delta time"
            ToolTip.visible: hovered
            checked: toolbar.deltaTraceVisible
            enabled: Store.comparing
            objectName: "deltaTraceButton"
            text: "Δt"

            onClicked: Store.setChannelVisible("delta", !toolbar.deltaTraceVisible)
        }
        ToolButton {
            Layout.fillHeight: true
            Layout.preferredWidth: 30
            ToolTip.text: "Zoom in"
            ToolTip.visible: hovered
            objectName: "zoomInButton"
            text: "+"

            onClicked: Store.zoomAt(Store.cursorFrac, 0.7)
        }
        ToolButton {
            Layout.fillHeight: true
            Layout.preferredWidth: 30
            ToolTip.text: "Zoom out"
            ToolTip.visible: hovered
            objectName: "zoomOutButton"
            text: "−"

            onClicked: Store.zoomAt(Store.cursorFrac, 1.4)
        }
        ToolButton {
            Layout.fillHeight: true
            Layout.preferredWidth: 30
            ToolTip.text: "Reset zoom"
            ToolTip.visible: hovered
            objectName: "zoomResetButton"
            text: "⤢"

            onClicked: Store.resetView()
        }
        ToolButton {
            id: confidenceButton

            Layout.fillHeight: true
            Layout.preferredWidth: 86
            ToolTip.text: "Show fastest-half session consistency heatmap (hold .)"
            ToolTip.visible: hovered
            checkable: true
            checked: Store.traceConfidenceMode
            objectName: "confidenceButton"
            text: "Consistency"

            onClicked: {
                Store.traceConfidenceMode = confidenceButton.checked;
                toolbar.trace.forceActiveFocus();
            }
        }
        Label {
            Layout.fillWidth: true
            Layout.leftMargin: 6
            Layout.rightMargin: 8
            color: Store.traceConfidenceLoading ? Style.orangeColor : Store.traceConfidenceLapCount >= 2 ? Style.accentColor : Style.mutedTextColor
            elide: Text.ElideRight
            font.family: Style.monoFontFamily
            font.pixelSize: Style.smallFontSize
            horizontalAlignment: Text.AlignRight
            text: Store.traceConfidenceLoading ? "FASTEST 50% · ALIGNING LAPS…" : Store.traceConfidenceLapCount >= 2 ? "FASTEST 50% · " + Store.traceConfidenceLapCount + " OTHER LAPS" : "FASTEST 50% · NEED 2 OTHER LAPS"
            verticalAlignment: Text.AlignVCenter
            visible: Store.traceConfidenceMode
        }
    }
}
