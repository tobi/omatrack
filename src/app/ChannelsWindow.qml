pragma ComponentBehavior: Bound
import Omatrack

import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

ApplicationWindow {
    id: channelsWindow

    property int cursorTick: 0
    property string filterText: ""
    property var sidecarRows: []

    function refresh(): void {
        channelsWindow.sidecarRows = Store.sidecarLibrary();
    }

    Material.accent: Style.accentColor
    Material.background: Style.backgroundColor
    Material.foreground: Style.foregroundColor
    Material.primary: Style.surfaceColor
    Material.theme: Material.Dark
    color: Style.backgroundColor
    font.family: Style.uiFontFamily
    font.pixelSize: 11
    height: 620
    minimumHeight: 420
    minimumWidth: 660
    objectName: "channelsWindow"
    title: "Trace Channels"
    visible: false
    width: 780

    RowFilterModel {
        id: channelFilter

        filterText: channelsWindow.filterText
        sourceModel: Store.channels
    }
    Connections {
        function onCursorFracChanged(): void {
            ++channelsWindow.cursorTick;
        }
        function onSelectionChanged(): void {
            channelsWindow.refresh();
        }
        function onSidecarLibraryChanged(): void {
            channelsWindow.sidecarRows = Store.sidecarLibrary();
        }

        target: Store
    }
    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 10
        spacing: 6

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Label {
                Layout.fillWidth: true
                font.bold: true
                font.pixelSize: 15
                text: "Trace channels"
            }
            TextField {
                id: channelSearch

                Layout.preferredHeight: 32
                Layout.preferredWidth: 220
                placeholderText: "Type to filter channels"
                selectByMouse: true
                text: channelsWindow.filterText

                onTextChanged: channelsWindow.filterText = text
            }
        }
        Label {
            Layout.fillWidth: true
            color: Style.mutedTextColor
            elide: Text.ElideRight
            text: "Visibility, color, lane size, and the value at the current playhead"
        }
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: sidecarList.visible ? Math.min(112, 34 + channelsWindow.sidecarRows.length * 38) : 0
            border.color: Style.borderColor
            color: Style.surfaceColor
            radius: 3
            visible: channelsWindow.sidecarRows.length > 0

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 6
                spacing: 2

                Label {
                    Layout.fillWidth: true
                    color: Style.accentColor
                    font.bold: true
                    font.family: Style.monoFontFamily
                    font.pixelSize: 9
                    text: "AVAILABLE SIDECARS · MATCHING THIS SESSION"
                }
                ListView {
                    id: sidecarList

                    Layout.fillHeight: true
                    Layout.fillWidth: true
                    clip: true
                    interactive: channelsWindow.sidecarRows.length > 3
                    model: channelsWindow.sidecarRows.length

                    delegate: RowLayout {
                        id: sidecarRow

                        required property int index
                        readonly property var sidecar: channelsWindow.sidecarRows[sidecarRow.index]

                        height: 34
                        spacing: 7
                        width: ListView.view.width

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0

                            Label {
                                Layout.fillWidth: true
                                color: Style.foregroundColor
                                elide: Text.ElideRight
                                font.bold: true
                                text: sidecarRow.sidecar.name || sidecarRow.sidecar.path
                            }
                            Label {
                                Layout.fillWidth: true
                                color: Style.mutedTextColor
                                elide: Text.ElideRight
                                font.family: Style.monoFontFamily
                                font.pixelSize: 8
                                text: sidecarRow.sidecar.window + " · " + sidecarRow.sidecar.channelCount + " ch · " + sidecarRow.sidecar.spanCount + " spans"
                            }
                        }
                        CompactButton {
                            Layout.preferredWidth: 76
                            text: sidecarRow.sidecar.attached ? "Added" : "Add"

                            onClicked: Store.attachSidecar(sidecarRow.sidecar.path)
                        }
                    }
                }
            }
        }
        ListView {
            id: channelListView

            Layout.fillHeight: true
            Layout.fillWidth: true
            clip: true
            highlightFollowsCurrentItem: false
            model: channelFilter
            reuseItems: true
            spacing: 2

            delegate: Rectangle {
                id: channelRow

                required property string channelColor
                required property bool channelVisible
                required property int index
                required property string key
                required property string title
                required property string unit
                required property double weight

                color: channelRow.index % 2 === 0 ? Style.surfaceColor : Style.backgroundColor
                height: 34
                radius: 2
                width: ListView.view.width

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 7
                    anchors.rightMargin: 7
                    spacing: 7

                    Switch {
                        Layout.preferredWidth: 42
                        checked: channelRow.channelVisible
                        scale: 0.78

                        onToggled: Store.setChannelVisible(channelRow.key, checked)
                    }
                    Label {
                        Layout.preferredWidth: 156
                        color: Style.foregroundColor
                        elide: Text.ElideRight
                        font.bold: true
                        text: channelRow.title
                    }
                    Label {
                        Layout.preferredWidth: 52
                        color: Style.mutedTextColor
                        elide: Text.ElideRight
                        font.family: Style.monoFontFamily
                        font.pixelSize: 9
                        text: channelRow.unit
                    }
                    Label {
                        Layout.preferredWidth: 86
                        color: Style.accentColor
                        elide: Text.ElideRight
                        font.family: Style.monoFontFamily
                        font.pixelSize: 9
                        horizontalAlignment: Text.AlignRight
                        text: channelsWindow.cursorTick >= 0 ? Store.channelExample(channelRow.key) : "—"
                    }
                    ComboBox {
                        id: colorBox

                        currentIndex: Math.max(0, Style.colorChoices.indexOf(channelRow.channelColor))
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

                        onActivated: Store.setChannelColor(channelRow.key, currentText)
                    }
                    ComboBox {
                        Layout.preferredWidth: 76
                        currentIndex: Math.max(0, Math.round((channelRow.weight - 0.5) / 0.25))
                        model: [0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0]

                        onActivated: Store.setChannelWeight(channelRow.key, Number(currentText))
                    }
                }
            }

            ScrollAnchor {
                role: "key"
                view: channelListView
            }
        }
        RowLayout {
            Layout.fillWidth: true

            Label {
                Layout.fillWidth: true
                color: Style.mutedTextColor
                font.family: Style.monoFontFamily
                font.pixelSize: 9
                text: channelFilter.rowCount + " channels"
            }
            CompactButton {
                text: "Close"

                onClicked: channelsWindow.hide()
            }
        }
    }
}
