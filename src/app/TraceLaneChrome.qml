pragma ComponentBehavior: Bound
import Omatrack

import QtQuick
import QtQuick.Controls

Item {
    id: chrome

    required property TraceView trace

    function chromeText(items: var): string {
        const parts = [];
        for (const item of items) {
            if (String(item.kind) === "pill")
                parts.push(String(item.label) + " " + String(item.value));
            else if (String(item.text) !== "")
                parts.push(String(item.text));
        }
        return parts.join(" · ");
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
        model: chrome.trace.laneRows

        delegate: Item {
            id: lane

            readonly property real laneHeight: Number(lane.modelData.height)
            readonly property string laneKind: String(lane.modelData.kind)
            required property var modelData

            clip: true
            height: lane.laneHeight
            width: chrome.width
            y: Number(lane.modelData.y)

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
                    rightPadding: lane.laneHeight >= 12 && lane.laneHeight < 24 ? 48 : 6
                    text: String(lane.modelData.title)
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
                    text: String(lane.modelData.unit)
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
                    text: lane.modelData.expanded === true ? "▾" : "▸"
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
                    text: String(lane.modelData.title)
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
                    text: chrome.chromeText(lane.modelData.chrome || [])
                    verticalAlignment: Text.AlignVCenter
                    width: parent.width * 0.42
                    x: parent.width - width
                }
            }
        }
    }
}
