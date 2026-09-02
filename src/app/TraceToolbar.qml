pragma ComponentBehavior: Bound
import Omatrack

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: toolbar

    required property bool deltaTraceVisible
    required property TraceView trace

    signal channelsRequested

    implicitHeight: Style.iconButtonSize

    RowLayout {
        anchors.fill: parent
        spacing: 2

        CompactToolButton {
            Layout.fillHeight: true
            Layout.maximumWidth: 58
            Layout.minimumWidth: 58
            Layout.preferredWidth: 58
            checked: toolbar.deltaTraceVisible
            enabled: Store.comparing
            objectName: "deltaTraceButton"
            text: "Δt"
            tip: Store.comparing ? (toolbar.deltaTraceVisible ? "Hide comparison delta-time trace" : "Show comparison delta-time trace") : "Select a comparison lap to show delta time"

            onClicked: Store.setChannelVisible("delta", !toolbar.deltaTraceVisible)
        }
        CompactToolButton {
            Layout.fillHeight: true
            Layout.maximumWidth: 30
            Layout.minimumWidth: 30
            Layout.preferredWidth: 30
            objectName: "zoomInButton"
            text: "+"
            tip: "Zoom in"

            onClicked: Store.zoomAt(Store.cursorFrac, 0.7)
        }
        CompactToolButton {
            Layout.fillHeight: true
            Layout.maximumWidth: 30
            Layout.minimumWidth: 30
            Layout.preferredWidth: 30
            objectName: "zoomOutButton"
            text: "−"
            tip: "Zoom out"

            onClicked: Store.zoomAt(Store.cursorFrac, 1.4)
        }
        CompactToolButton {
            Layout.fillHeight: true
            Layout.maximumWidth: 30
            Layout.minimumWidth: 30
            Layout.preferredWidth: 30
            objectName: "zoomResetButton"
            text: "⤢"
            tip: "Reset zoom"

            onClicked: Store.resetView()
        }
        CompactToolButton {
            Layout.fillHeight: true
            Layout.maximumWidth: 76
            Layout.minimumWidth: 76
            Layout.preferredWidth: 76
            objectName: "channelsButton"
            text: "Channels…"
            tip: "Configure trace channels"

            onClicked: toolbar.channelsRequested()
        }
        CompactToolButton {
            Layout.fillHeight: true
            Layout.maximumWidth: 72
            Layout.minimumWidth: 72
            Layout.preferredWidth: 72
            enabled: Store.primaryLabel !== ""
            objectName: "editCornersButton"
            text: Store.editingCorners ? "Editing" : "Corners"
            tip: "Edit corner zones"

            onClicked: Store.beginCornerEdit()
        }
        CompactToolButton {
            Layout.fillHeight: true
            Layout.maximumWidth: 48
            Layout.minimumWidth: 48
            Layout.preferredWidth: 48
            checkable: true
            checked: toolbar.trace.fitChannels
            objectName: "fitChannelsButton"
            text: "FIT"
            tip: toolbar.trace.fitChannels ? "Show all traces at once" : "Use standard lane sizes and scroll vertically"

            onClicked: toolbar.trace.fitChannels = checked
        }
        CompactToolButton {
            id: eventModeButton

            Layout.fillHeight: true
            Layout.maximumWidth: 56
            Layout.minimumWidth: 56
            Layout.preferredWidth: 56
            checkable: true
            checked: Store.eventMode
            objectName: "eventModeButton"
            text: "Event"
            tip: Store.eventMode ? "Show the full library" : "Filter the library to this track and day"

            onClicked: Store.eventMode = eventModeButton.checked
        }
        Label {
            color: Style.accentColor
            font.family: Style.monoFontFamily
            font.pixelSize: Style.smallFontSize
            text: Store.eventSession
            visible: Store.eventMode && Store.eventSession !== ""
        }
        CompactTextField {
            Layout.maximumWidth: 64
            Layout.preferredWidth: 64
            placeholderText: "c1"
            text: Store.eventSession
            visible: Store.eventMode

            onEditingFinished: Store.eventSession = text.trim()
        }
        CompactToolButton {
            id: confidenceButton

            Layout.fillHeight: true
            Layout.maximumWidth: 86
            Layout.minimumWidth: 86
            Layout.preferredWidth: 86
            checkable: true
            checked: Store.traceConfidenceMode
            objectName: "confidenceButton"
            text: "Consistency"
            tip: "Show fastest-half session consistency heatmap (hold .)"

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
            text: !Store.traceConfidenceMode ? "" : Store.traceConfidenceLoading ? "FASTEST 50% · ALIGNING LAPS…" : Store.traceConfidenceLapCount >= 2 ? "FASTEST 50% · " + Store.traceConfidenceLapCount + " OTHER LAPS" : "FASTEST 50% · NEED 2 OTHER LAPS"
            verticalAlignment: Text.AlignVCenter
        }
    }
}
