pragma ComponentBehavior: Bound

// Trace channel configuration: per-channel visibility, color, and lane weight.
//
// Owns its own row cache and refreshes it from the store's channelConfigChanged
// signal, so nothing upstream has to remember to push data in.

import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import Racecraft

ApplicationWindow {
    id: channelsWindow

    // Standard channels are configured in the trace lanes themselves; this
    property var channelRows: []

    function refresh(): void {
        const settings = Store.channelSettings();
        let rows = [];
        for (let i = 0; i < settings.length; ++i)
            if (settings[i].key !== "delta")
                rows.push(settings[i]);
        channelsWindow.channelRows = rows;
    }

    Material.accent: Style.accentColor
    Material.background: Style.backgroundColor
    Material.foreground: Style.foregroundColor
    Material.primary: Style.surfaceColor
    Material.theme: Material.Dark
    color: Style.backgroundColor
    font.family: Style.uiFontFamily
    font.pixelSize: 11
    height: 480
    minimumHeight: 400
    minimumWidth: 520
    objectName: "channelsWindow"
    title: "Trace Channels"
    visible: false
    width: 640

    Component.onCompleted: channelsWindow.refresh()

    Connections {
        function onChannelConfigChanged(): void {
            channelsWindow.refresh();
        }

        target: Store
    }
    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 6

        Label {
            font.bold: true
            font.pixelSize: 16
            text: "Trace channels"
        }
        Label {
            color: Style.mutedTextColor
            font.pixelSize: 11
            text: "Source channels; visibility, color, and lane height"
        }
        ListView {
            id: channelListView

            Layout.fillHeight: true
            Layout.fillWidth: true
            clip: true
            model: channelsWindow.channelRows

            delegate: RowLayout {
                id: channelRow

                required property var modelData

                height: 36
                spacing: 8
                width: ListView.view.width

                Switch {
                    checked: channelRow.modelData.visible

                    onToggled: Store.setChannelVisible(channelRow.modelData.key, checked)
                }
                Label {
                    Layout.preferredWidth: 92
                    font.bold: true
                    text: channelRow.modelData.title
                }
                Label {
                    Layout.preferredWidth: 40
                    color: Style.mutedTextColor
                    font.family: Style.monoFontFamily
                    text: channelRow.modelData.unit
                }
                ComboBox {
                    id: colorBox

                    Layout.preferredWidth: 120
                    currentIndex: Math.max(0, Style.colorChoices.indexOf(channelRow.modelData.color))
                    model: Style.colorChoices

                    delegate: ItemDelegate {
                        id: colorChoice

                        required property string modelData

                        width: colorBox.width

                        contentItem: Rectangle {
                            border.color: Style.borderColor
                            color: colorChoice.modelData
                            radius: 3
                        }
                    }

                    onActivated: Store.setChannelColor(channelRow.modelData.key, currentText)
                }
                ComboBox {
                    Layout.preferredWidth: 88
                    currentIndex: Math.max(0, Math.round((channelRow.modelData.weight - 0.5) / 0.25))
                    model: [0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0]

                    onActivated: Store.setChannelWeight(channelRow.modelData.key, Number(currentText))
                }
            }
        }
        RowLayout {
            Layout.fillWidth: true

            Item {
                Layout.fillWidth: true
            }
            CompactButton {
                text: "Close"

                onClicked: channelsWindow.hide()
            }
        }
    }
}
