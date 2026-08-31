pragma ComponentBehavior: Bound
import Omatrack

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Proportional lap filmstrip: one row per active/reference session. Laps fill
// the lane by time, including Out/In/Frag. Left click selects the current lap
// for that session; right click on a bar sets comparison. Label right-click
// stays "Swap with reference  X".

Rectangle {
    id: filmstrip

    signal pointerTooltipDismissed(string owner)
    signal pointerTooltipMoved(string owner, real x, real y)
    signal pointerTooltipRequested(string owner, string text, real x, real y)

    clip: true
    color: Style.traceBackgroundColor
    implicitHeight: Store.filmstripSessions.count > 0 ? Store.filmstripSessions.count * 33 + 9 : 0
    visible: Store.filmstripSessions.count > 0

    Menu {
        id: swapReferenceMenu

        objectName: "filmstripSwapMenu"

        MenuItem {
            enabled: Store.comparing
            text: "Swap with reference  X"

            onTriggered: Store.swapPrimaryWithReference()
        }
    }
    Column {
        anchors.fill: parent
        anchors.margins: 6
        spacing: 3

        Repeater {
            model: Store.filmstripSessions

            delegate: Rectangle {
                id: sessionStrip

                required property string bestTime
                required property string driverName
                readonly property var laps: sessionStrip.reference ? Store.compareLaps : Store.primaryLaps
                required property bool reference
                property string selectedLapTime: {
                    const key = sessionStrip.reference ? Store.compareSessionKey : Store.primarySessionKey;
                    const idx = sessionStrip.reference ? Store.compareLapIndex : Store.primaryLapIndex;
                    if (key === sessionStrip.sessionKey && idx >= 0)
                        return Store.lapTimeText(key, idx);
                    return sessionStrip.bestTime;
                }
                readonly property int selectedOrdinal: sessionStrip.reference ? Store.compareLapOrdinal : Store.primaryLapOrdinal
                required property string sessionKey

                color: sessionStrip.reference ? Qt.tint(Style.surfaceColor, Qt.rgba(Style.orangeColor.r, Style.orangeColor.g, Style.orangeColor.b, 0.08)) : Style.surfaceColor
                height: 30
                radius: 4
                width: parent.width

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 6
                    anchors.rightMargin: 4
                    spacing: 5

                    Item {
                        Layout.fillHeight: true
                        Layout.maximumWidth: 190
                        Layout.minimumWidth: 190
                        Layout.preferredWidth: 190
                        objectName: sessionStrip.reference ? "referenceFilmstripLabel" : "activeFilmstripLabel"

                        DenseTwoLineRow {
                            anchors.fill: parent
                            detailVisible: false
                            rightColor: sessionStrip.reference ? Style.orangeColor : Style.accentColor
                            rightFamily: Style.monoFontFamily
                            rightSize: 9
                            rightValue: (sessionStrip.selectedOrdinal > 0 ? "L" + sessionStrip.selectedOrdinal + " · " : "") + sessionStrip.selectedLapTime
                            title: (sessionStrip.reference ? "REF · " : "ACTIVE · ") + (sessionStrip.driverName !== "" && sessionStrip.driverName !== "Unknown" ? sessionStrip.driverName : "Unknown driver")
                            titleBold: true
                            titleColor: sessionStrip.reference ? Style.orangeColor : Style.accentColor
                            titleFamily: Style.monoFontFamily
                            titleSize: 9
                            titleSpacing: 4
                        }
                        // DenseTwoLineRow's default children go into its
                        // optional detail layout, which is hidden here. The
                        // hit target must be a sibling, not one of those rows.
                        MouseArea {
                            acceptedButtons: Qt.RightButton
                            anchors.fill: parent

                            onClicked: swapReferenceMenu.popup()
                        }
                    }
                    CompactToolButton {
                        Layout.preferredHeight: 24
                        Layout.preferredWidth: 24
                        text: "×"
                        tip: sessionStrip.reference ? "Remove reference session" : "Clear active session"

                        onClicked: sessionStrip.reference ? Store.clearCompare() : Store.clearPrimary()
                    }
                    Item {
                        Layout.preferredWidth: 7
                    }
                    Item {
                        id: proportionalLapLane

                        Layout.fillHeight: true
                        Layout.fillWidth: true

                        Row {
                            id: proportionalLapRow

                            anchors.fill: parent
                            spacing: 3

                            Repeater {
                                model: sessionStrip.laps

                                delegate: Rectangle {
                                    id: proportionalLap

                                    readonly property bool confidenceLap: !sessionStrip.reference && Store.traceConfidenceMode && Store.traceConfidenceIncludesLap(sessionStrip.sessionKey, proportionalLap.lapId)
                                    required property bool countsForBest
                                    required property string hoverText
                                    required property bool isComplete
                                    required property bool isFastest
                                    required property string label
                                    required property int lapId
                                    property bool selectedLap: sessionStrip.reference ? sessionStrip.sessionKey === Store.compareSessionKey && proportionalLap.lapId === Store.compareLapIndex : sessionStrip.sessionKey === Store.primarySessionKey && proportionalLap.lapId === Store.primaryLapIndex
                                    required property int timeMs
                                    required property string timeText
                                    readonly property string tooltipOwner: "lap:" + sessionStrip.sessionKey + ":" + proportionalLap.lapId + ":" + sessionStrip.reference

                                    // Bound to the Row, not `parent`:
                                    // a delegate evaluates its
                                    // bindings before it is reparented,
                                    // so `parent` is null on creation.
                                    anchors.verticalCenter: proportionalLapRow.verticalCenter
                                    border.color: sessionStrip.reference ? Style.orangeColor : Style.accentColor
                                    border.width: proportionalLap.selectedLap || proportionalLap.confidenceLap ? 1 : 0
                                    color: proportionalLap.selectedLap ? Style.selectionColor : proportionalLap.confidenceLap ? Qt.tint(Style.traceBackgroundColor, Qt.rgba(Style.accentColor.r, Style.accentColor.g, Style.accentColor.b, 0.2)) : proportionalLapMouse.containsMouse ? Style.backgroundColor : Style.traceBackgroundColor
                                    height: proportionalLapRow.height - 8
                                    objectName: (sessionStrip.reference ? "referenceFilmstripLap-" : "activeFilmstripLap-") + proportionalLap.lapId
                                    radius: 3
                                    width: Math.max(1, (proportionalLapLane.width - Math.max(0, sessionStrip.laps.count - 1) * proportionalLapRow.spacing) * Math.max(1, proportionalLap.timeMs) / Math.max(1, sessionStrip.laps.totalTimeMs))

                                    Rectangle {
                                        anchors.bottom: parent.bottom
                                        anchors.left: parent.left
                                        anchors.right: parent.right
                                        color: Style.greenColor
                                        height: proportionalLap.isFastest ? 2 : 0
                                    }
                                    Label {
                                        id: proportionalLapLabel

                                        anchors.fill: parent
                                        anchors.leftMargin: 5
                                        anchors.rightMargin: 5
                                        color: proportionalLap.selectedLap ? (sessionStrip.reference ? Style.orangeColor : Style.accentColor) : proportionalLap.isFastest ? Style.greenColor : Style.foregroundColor
                                        elide: Text.ElideRight
                                        font.bold: proportionalLap.selectedLap || proportionalLap.confidenceLap
                                        font.family: Style.monoFontFamily
                                        font.pixelSize: 9
                                        text: proportionalLap.isComplete ? proportionalLap.timeText : proportionalLap.label
                                        verticalAlignment: Text.AlignVCenter
                                    }
                                    MouseArea {
                                        id: proportionalLapMouse

                                        acceptedButtons: Qt.LeftButton | Qt.RightButton
                                        anchors.fill: parent
                                        hoverEnabled: true

                                        onClicked: mouse => {
                                            if (mouse.button === Qt.RightButton) {
                                                filmstrip.pointerTooltipDismissed(proportionalLap.tooltipOwner);
                                                Store.compareLap(sessionStrip.sessionKey, proportionalLap.lapId);
                                                return;
                                            }
                                            if (sessionStrip.reference)
                                                Store.compareLap(sessionStrip.sessionKey, proportionalLap.lapId);
                                            else
                                                Store.selectLap(sessionStrip.sessionKey, proportionalLap.lapId);
                                        }
                                        onEntered: {
                                            const point = proportionalLapMouse.mapToItem(Overlay.overlay, proportionalLapMouse.mouseX, proportionalLapMouse.mouseY);
                                            filmstrip.pointerTooltipRequested(proportionalLap.tooltipOwner, proportionalLap.hoverText + (proportionalLap.confidenceLap ? " · Consistency cohort" : ""), point.x, point.y);
                                        }
                                        onExited: filmstrip.pointerTooltipDismissed(proportionalLap.tooltipOwner)
                                        onPositionChanged: mouse => {
                                            const point = proportionalLapMouse.mapToItem(Overlay.overlay, mouse.x, mouse.y);
                                            filmstrip.pointerTooltipMoved(proportionalLap.tooltipOwner, point.x, point.y);
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
