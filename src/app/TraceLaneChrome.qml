pragma ComponentBehavior: Bound
import Omatrack

import QtQuick
import QtQuick.Controls

Item {
    id: chrome

    property list<traceLaneRow> rows: []
    required property TraceView trace

    function refresh(): void {
        if (chrome.trace)
            chrome.rows = chrome.trace.laneRows;
    }

    Component.onCompleted: chrome.refresh()
    onTraceChanged: chrome.refresh()

    Connections {
        function onLaneLayoutChanged(): void {
            chrome.refresh();
        }

        target: chrome.trace
    }
    Label {
        color: Style.mutedTextColor
        font.bold: true
        font.family: Style.monoFontFamily
        font.pixelSize: Style.smallFontSize
        height: 14
        horizontalAlignment: Text.AlignRight
        leftPadding: 2
        rightPadding: 6
        text: "Heat"
        verticalAlignment: Text.AlignVCenter
        visible: Store.traceConfidenceMode
        width: chrome.trace.labelWidth
        y: chrome.trace.rulerHeight
    }
    Repeater {
        model: chrome.rows.length

        delegate: Item {
            id: lane

            required property int index
            readonly property real laneHeight: lane.row.height
            readonly property string laneKind: lane.row.kind
            readonly property traceLaneRow row: chrome.rows[lane.index]

            clip: true
            height: lane.laneHeight
            objectName: "traceLane-" + lane.row.key
            width: chrome.width
            y: lane.row.y

            Item {
                id: channelLabel

                height: parent.height
                visible: lane.laneKind !== "group" && lane.laneHeight >= 7
                width: chrome.trace.labelWidth

                Label {
                    color: Style.mutedTextColor
                    elide: Text.ElideRight
                    font.bold: true
                    font.family: Style.monoFontFamily
                    font.pixelSize: Math.max(6, Math.min(Style.smallFontSize, lane.laneHeight - 2))
                    height: lane.laneHeight >= 24 ? 14 : lane.laneHeight
                    horizontalAlignment: Text.AlignRight
                    leftPadding: 2
                    rightPadding: !Store.resizingTraces && lane.laneHeight >= 12 && lane.laneHeight < 24 ? 48 : 6
                    text: lane.row.title
                    verticalAlignment: Text.AlignVCenter
                    width: parent.width
                    y: lane.laneHeight >= 24 ? 2 : 0
                }
                Label {
                    color: Style.dimTextColor
                    font.family: Style.monoFontFamily
                    font.pixelSize: Math.max(6, Style.smallFontSize - 2)
                    height: 12
                    horizontalAlignment: Text.AlignRight
                    leftPadding: 2
                    rightPadding: 6
                    text: lane.row.unit
                    verticalAlignment: Text.AlignVCenter
                    visible: lane.laneHeight >= 24 && text !== ""
                    width: parent.width
                    y: 15
                }
            }
            Item {
                id: groupLabel

                height: parent.height
                visible: lane.laneKind === "group" && lane.laneHeight >= 10
                width: parent.width

                Label {
                    color: Style.foregroundColor
                    font.bold: true
                    font.family: Style.monoFontFamily
                    font.pixelSize: Math.min(Style.smallFontSize, Math.max(7, lane.laneHeight - 5))
                    height: parent.height
                    horizontalAlignment: Text.AlignLeft
                    text: lane.row.expanded ? "▾" : "▸"
                    verticalAlignment: Text.AlignVCenter
                    width: 14
                    x: 6
                }
                Label {
                    color: Style.foregroundColor
                    elide: Text.ElideRight
                    font.bold: true
                    font.family: Style.monoFontFamily
                    font.pixelSize: Math.min(Style.smallFontSize, Math.max(7, lane.laneHeight - 5))
                    height: parent.height
                    horizontalAlignment: Text.AlignLeft
                    text: lane.row.title
                    verticalAlignment: Text.AlignVCenter
                    width: Math.max(0, parent.width * 0.56 - 28)
                    x: 20
                }
                Label {
                    color: Style.mutedTextColor
                    elide: Text.ElideRight
                    font.family: Style.monoFontFamily
                    font.pixelSize: Math.min(Style.smallFontSize, Math.max(7, lane.laneHeight - 6))
                    height: parent.height
                    horizontalAlignment: Text.AlignRight
                    rightPadding: 8
                    text: lane.row.chromeText
                    verticalAlignment: Text.AlignVCenter
                    width: parent.width * 0.42
                    x: parent.width - width
                }
            }
        }
    }
}
