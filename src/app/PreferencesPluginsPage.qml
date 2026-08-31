pragma ComponentBehavior: Bound
import Omatrack

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: pluginsPage

    property var pluginRows: Store.pluginLibrary()
    property string statusLine: ""

    objectName: "preferencesPluginsPage"

    Connections {
        function onPluginsChanged(): void {
            pluginsPage.pluginRows = Store.pluginLibrary();
        }

        target: Store
    }
    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 20
        spacing: 12

        Label {
            font.bold: true
            font.pixelSize: 16
            text: "Plugins"
        }
        Label {
            Layout.fillWidth: true
            Layout.preferredWidth: 1
            color: Style.mutedTextColor
            text: "Lua trace-group plugins add channels for the open session — weather, timing feeds, lab data. Each plugin is a folder with a plugin.lua; every call runs sandboxed on a worker, and an enabled plugin may make the HTTP requests it is written to make."
            wrapMode: Text.Wrap
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Label {
                Layout.fillWidth: true
                color: Style.mutedTextColor
                elide: Text.ElideMiddle
                font.family: Style.monoFontFamily
                font.pixelSize: Style.smallFontSize
                text: Store.pluginDirectory()
            }
            CompactButton {
                text: "Open folder"

                onClicked: Store.openPluginDirectory()
            }
            CompactButton {
                text: "Reload"

                onClicked: Store.reloadPlugins()
            }
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Repeater {
                model: Store.examplePlugins()

                delegate: CompactButton {
                    id: exampleButton

                    required property string modelData

                    objectName: "installExample-" + exampleButton.modelData
                    text: "Install " + exampleButton.modelData + " example"

                    onClicked: pluginsPage.statusLine = Store.installExamplePlugin(exampleButton.modelData)
                }
            }
            Label {
                Layout.fillWidth: true
                Layout.preferredWidth: 1  // wrap to the row, do not widen it
                color: Style.mutedTextColor
                font.pixelSize: Style.smallFontSize
                text: "The weather example asks open-meteo.com for the session's hour of temperature, precipitation, wind, humidity and pressure."
                wrapMode: Text.Wrap
            }
        }
        Label {
            Layout.fillWidth: true
            Layout.preferredWidth: 1
            color: Style.accentColor
            font.pixelSize: Style.smallFontSize
            objectName: "pluginStatusLine"
            text: pluginsPage.statusLine
            visible: pluginsPage.statusLine !== ""
            wrapMode: Text.Wrap
        }
        Rectangle {
            Layout.fillHeight: true
            Layout.fillWidth: true
            border.color: Style.borderColor
            color: Style.surfaceColor
            radius: 3

            Label {
                anchors.centerIn: parent
                color: Style.mutedTextColor
                text: "No plugins installed yet"
                visible: pluginsPage.pluginRows.length === 0
            }
            ListView {
                id: pluginList

                anchors.fill: parent
                anchors.margins: 6
                clip: true
                model: pluginsPage.pluginRows.length
                spacing: 4

                ScrollBar.vertical: ThinScrollBar {
                }
                delegate: Rectangle {
                    id: pluginRow

                    required property int index
                    readonly property var plugin: pluginsPage.pluginRows[pluginRow.index]

                    color: Style.backgroundColor
                    height: 54
                    radius: 3
                    width: pluginList.width

                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 8
                        spacing: 10

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 1

                            Label {
                                Layout.fillWidth: true
                                elide: Text.ElideRight
                                font.bold: true
                                text: pluginRow.plugin.name + (pluginRow.plugin.version > 0 ? "  v" + pluginRow.plugin.version : "")
                            }
                            Label {
                                Layout.fillWidth: true
                                color: pluginRow.plugin.error !== "" ? Style.orangeColor : Style.mutedTextColor
                                elide: Text.ElideRight
                                font.family: Style.monoFontFamily
                                font.pixelSize: Style.smallFontSize
                                text: pluginRow.plugin.error !== "" ? pluginRow.plugin.error : pluginRow.plugin.status
                            }
                            Label {
                                Layout.fillWidth: true
                                color: Style.dimTextColor
                                elide: Text.ElideMiddle
                                font.family: Style.monoFontFamily
                                font.pixelSize: Style.smallFontSize
                                text: pluginRow.plugin.directory
                            }
                        }
                        BusyIndicator {
                            Layout.preferredHeight: 18
                            Layout.preferredWidth: 18
                            running: pluginRow.plugin.loading
                            visible: pluginRow.plugin.loading
                        }
                        Switch {
                            checked: pluginRow.plugin.enabled
                            enabled: pluginRow.plugin.error === ""
                            text: pluginRow.plugin.enabled ? "Enabled" : "Off"

                            onToggled: Store.setPluginEnabled(pluginRow.plugin.id, checked)
                        }
                    }
                }
            }
        }
    }
}
