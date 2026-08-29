pragma ComponentBehavior: Bound
import Omatrack

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Proportional lap filmstrip: one row per active/reference session, each row
// a proportional bar of its laps with the best lap highlighted. Extracted
// from Main.qml; the root passes the session strip array and receives
// signals for pointer tooltips and the right-click lap menu.

Rectangle {
    id: filmstrip

    property var sessions: []

    signal lapMenuRequested(string sessionKey, int lapId, real x, real y)
    signal pointerTooltipDismissed(string owner)
    signal pointerTooltipMoved(string owner, real x, real y)
    signal pointerTooltipRequested(string owner, string text, real x, real y)

    Layout.fillWidth: true
    Layout.preferredHeight: visible ? filmstrip.sessions.length * 33 + 9 : 0
    color: Style.traceBackgroundColor
    visible: filmstrip.sessions.length > 0

    Column {
        anchors.fill: parent
        anchors.margins: 6
        spacing: 3

        Repeater {
            model: filmstrip.sessions

            delegate: Rectangle {
                id: sessionStrip

                readonly property var laps: sessionStrip.strip.reference ? Store.compareLaps : Store.primaryLaps
                required property var modelData
                property string selectedLapTime: {
                    const key = strip.reference ? Store.compareSessionKey : Store.primarySessionKey;
                    const idx = strip.reference ? Store.compareLapIndex : Store.primaryLapIndex;
                    if (key === strip.sessionKey && idx >= 0)
                        return Store.lapTimeText(key, idx);
                    return strip.bestTime;
                }
                property var strip: modelData

                color: strip.reference ? Qt.tint(Style.surfaceColor, Qt.rgba(Style.orangeColor.r, Style.orangeColor.g, Style.orangeColor.b, 0.08)) : Style.surfaceColor
                height: 30
                radius: 4
                width: parent.width

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 6
                    anchors.rightMargin: 4
                    spacing: 5

                    DenseTwoLineRow {
                        Layout.maximumWidth: 190
                        Layout.minimumWidth: 190
                        Layout.preferredWidth: 190
                        detailVisible: false
                        rightColor: sessionStrip.strip.reference ? Style.orangeColor : Style.accentColor
                        rightFamily: Style.monoFontFamily
                        rightSize: 9
                        rightValue: sessionStrip.selectedLapTime
                        title: sessionStrip.strip.driverName !== "" && sessionStrip.strip.driverName !== "Unknown" ? sessionStrip.strip.driverName : "Unknown driver"
                        titleBold: true
                        titleColor: sessionStrip.strip.reference ? Style.orangeColor : Style.accentColor
                        titleFamily: Style.monoFontFamily
                        titleSize: 9
                        titleSpacing: 4
                    }
                    CompactToolButton {
                        Layout.preferredHeight: 24
                        Layout.preferredWidth: 24
                        text: "×"
                        tip: sessionStrip.strip.reference ? "Remove reference session" : "Clear active session"

                        onClicked: sessionStrip.strip.reference ? Store.clearCompare() : Store.clearPrimary()
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

                                    readonly property bool confidenceLap: !sessionStrip.strip.reference && Store.traceConfidenceMode && Store.traceConfidenceIncludesLap(sessionStrip.strip.sessionKey, proportionalLap.lapId)
                                    required property bool countsForBest
                                    readonly property real flexibleLaneWidth: Math.max(0, proportionalLapLane.width - Math.max(0, sessionStrip.laps.rowCount - 1) * proportionalLapRow.spacing - (sessionStrip.laps.fixedLapCount === sessionStrip.laps.rowCount ? 0 : sessionStrip.laps.fixedLapCount * 30))
                                    required property bool isFastest
                                    required property string label
                                    required property int lapId
                                    readonly property bool pinIncomplete: !proportionalLap.countsForBest && sessionStrip.laps.fixedLapCount !== sessionStrip.laps.rowCount
                                    property bool selectedLap: sessionStrip.strip.reference ? sessionStrip.strip.sessionKey === Store.compareSessionKey && proportionalLap.lapId === Store.compareLapIndex : sessionStrip.strip.sessionKey === Store.primarySessionKey && proportionalLap.lapId === Store.primaryLapIndex
                                    required property int timeMs
                                    required property string timeText
                                    readonly property string tooltipOwner: "lap:" + sessionStrip.strip.sessionKey + ":" + proportionalLap.lapId + ":" + sessionStrip.strip.reference

                                    // Bound to the Row, not `parent`:
                                    // a delegate evaluates its
                                    // bindings before it is reparented,
                                    // so `parent` is null on creation.
                                    anchors.verticalCenter: proportionalLapRow.verticalCenter
                                    border.color: sessionStrip.strip.reference ? Style.orangeColor : Style.accentColor
                                    border.width: proportionalLap.selectedLap || proportionalLap.confidenceLap ? 1 : 0
                                    color: proportionalLap.selectedLap ? Style.selectionColor : proportionalLap.confidenceLap ? Qt.tint(Style.traceBackgroundColor, Qt.rgba(Style.accentColor.r, Style.accentColor.g, Style.accentColor.b, 0.2)) : proportionalLapMouse.containsMouse ? Style.backgroundColor : Style.traceBackgroundColor
                                    height: proportionalLapRow.height - 8
                                    objectName: (sessionStrip.strip.reference ? "referenceFilmstripLap-" : "activeFilmstripLap-") + proportionalLap.lapId
                                    radius: 3
                                    width: proportionalLap.pinIncomplete ? 30 : Math.max(1, proportionalLap.flexibleLaneWidth * Math.max(1, proportionalLap.timeMs) / (sessionStrip.laps.fixedLapCount === sessionStrip.laps.rowCount ? sessionStrip.laps.totalTimeMs : sessionStrip.laps.flexibleTimeMs))

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
                                        color: proportionalLap.selectedLap ? (sessionStrip.strip.reference ? Style.orangeColor : Style.accentColor) : proportionalLap.isFastest ? Style.greenColor : Style.foregroundColor
                                        elide: Text.ElideRight
                                        font.bold: proportionalLap.selectedLap || proportionalLap.confidenceLap
                                        font.family: Style.monoFontFamily
                                        font.pixelSize: 9
                                        text: proportionalLap.pinIncomplete ? proportionalLap.label : proportionalLap.timeText
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
                                                filmstrip.lapMenuRequested(sessionStrip.strip.sessionKey, proportionalLap.lapId, mouse.x, mouse.y);
                                                return;
                                            }
                                            if (sessionStrip.strip.reference)
                                                Store.compareLap(sessionStrip.strip.sessionKey, proportionalLap.lapId);
                                            else
                                                Store.selectLap(sessionStrip.strip.sessionKey, proportionalLap.lapId);
                                        }
                                        onEntered: {
                                            const point = proportionalLapMouse.mapToItem(Overlay.overlay, proportionalLapMouse.mouseX, proportionalLapMouse.mouseY);
                                            filmstrip.pointerTooltipRequested(proportionalLap.tooltipOwner, proportionalLap.timeText + " · " + proportionalLap.label + (proportionalLap.confidenceLap ? " · Consistency cohort" : ""), point.x, point.y);
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
