pragma ComponentBehavior: Bound
import Omatrack

import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

ApplicationWindow {
    id: channelsWindow

    property string appearanceKey: ""
    property int cursorTick: 0
    property string filterText: ""
    property var pluginRows: []
    property var sidecarRows: []

    function refresh(): void {
        channelsWindow.sidecarRows = Store.sidecarLibrary();
        channelsWindow.pluginRows = Store.pluginLibrary();
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
    minimumWidth: 740
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
        function onChannelsCursorTick(): void {
            if (!channelsWindow.visible)
                return;
            ++channelsWindow.cursorTick;
        }
        function onCursorFracChanged(): void {
            if (!channelsWindow.visible)
                return;
        }
        function onPluginsChanged(): void {
            channelsWindow.pluginRows = Store.pluginLibrary();
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
            text: "Active / reference colors · Style: line width and area fill · Lane size · Live values"
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
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: pluginList.visible ? Math.min(112, 34 + channelsWindow.pluginRows.length * 38) : 0
            border.color: Style.borderColor
            color: Style.surfaceColor
            radius: 3
            visible: channelsWindow.pluginRows.length > 0

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 6
                spacing: 2

                RowLayout {
                    Layout.fillWidth: true

                    Label {
                        Layout.fillWidth: true
                        color: Style.accentColor
                        elide: Text.ElideMiddle
                        font.bold: true
                        font.family: Style.monoFontFamily
                        font.pixelSize: 9
                        text: "PLUGINS · " + Store.pluginDirectory()
                    }
                    CompactButton {
                        text: "Reload"

                        onClicked: Store.reloadPlugins()
                    }
                }
                ListView {
                    id: pluginList

                    Layout.fillHeight: true
                    Layout.fillWidth: true
                    clip: true
                    interactive: channelsWindow.pluginRows.length > 3
                    model: channelsWindow.pluginRows.length

                    delegate: RowLayout {
                        id: pluginRow

                        required property int index
                        readonly property var plugin: channelsWindow.pluginRows[pluginRow.index]

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
                                text: pluginRow.plugin.name + (pluginRow.plugin.version > 0 ? "  v" + pluginRow.plugin.version : "")
                            }
                            Label {
                                Layout.fillWidth: true
                                color: pluginRow.plugin.error !== "" ? Style.orangeColor : Style.mutedTextColor
                                elide: Text.ElideRight
                                font.family: Style.monoFontFamily
                                font.pixelSize: 8
                                text: pluginRow.plugin.error !== "" ? pluginRow.plugin.error : pluginRow.plugin.status
                            }
                        }
                        BusyIndicator {
                            Layout.preferredHeight: 18
                            Layout.preferredWidth: 18
                            running: pluginRow.plugin.loading
                            visible: pluginRow.plugin.loading
                        }
                        CompactButton {
                            Layout.preferredWidth: 76
                            enabled: pluginRow.plugin.error === "" && (pluginRow.plugin.enabled || pluginRow.plugin.channelCount > 0)
                            text: pluginRow.plugin.enabled ? "Remove" : "Add"

                            onClicked: Store.setPluginEnabled(pluginRow.plugin.id, !pluginRow.plugin.enabled)
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
                required property double fillOpacity
                required property int index
                required property string key
                required property string referenceColor
                required property bool span
                required property double strokeWidth
                required property string title
                required property string unit
                required property double weight

                color: channelRow.index % 2 === 0 ? Style.surfaceColor : Style.backgroundColor
                height: channelsWindow.appearanceKey === channelRow.key ? 76 : 34
                radius: 2
                width: ListView.view.width

                RowLayout {
                    anchors.left: parent.left
                    anchors.leftMargin: 7
                    anchors.right: parent.right
                    anchors.rightMargin: 7
                    anchors.top: parent.top
                    height: 34
                    spacing: 7

                    Switch {
                        Layout.preferredWidth: 42
                        checked: channelRow.channelVisible
                        scale: 0.78

                        onToggled: Store.setChannelVisible(channelRow.key, checked)
                    }
                    Label {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 70
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
                    TraceColorButton {
                        text: "Active"
                        traceColor: channelRow.channelColor

                        onColorSelected: value => Store.setChannelColor(channelRow.key, value)
                    }
                    TraceColorButton {
                        enabled: !channelRow.span
                        text: "Ref"
                        traceColor: channelRow.referenceColor

                        onColorSelected: value => Store.setChannelAppearance(channelRow.key, channelRow.strokeWidth, channelRow.fillOpacity, value)
                    }
                    CompactButton {
                        enabled: !channelRow.span
                        text: "Style…"

                        onClicked: channelsWindow.appearanceKey = channelsWindow.appearanceKey === channelRow.key ? "" : channelRow.key
                    }
                    ComboBox {
                        Layout.preferredHeight: 26
                        Layout.preferredWidth: 76
                        ToolTip.text: "Lane height"
                        ToolTip.visible: hovered
                        currentIndex: Math.max(0, Math.round((channelRow.weight - 0.5) / 0.25))
                        model: [0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0]

                        onActivated: Store.setChannelWeight(channelRow.key, Number(currentText))
                    }
                }
                RowLayout {
                    anchors.bottom: parent.bottom
                    anchors.left: parent.left
                    anchors.leftMargin: 52
                    anchors.right: parent.right
                    anchors.rightMargin: 10
                    height: 38
                    spacing: 8
                    visible: channelsWindow.appearanceKey === channelRow.key

                    Label {
                        color: Style.mutedTextColor
                        text: "Stroke"
                    }
                    Slider {
                        id: strokeSlider

                        Accessible.name: channelRow.title + " stroke width"
                        Layout.preferredWidth: 120
                        from: 0.5
                        stepSize: 0.25
                        to: 4.0
                        value: channelRow.strokeWidth

                        onMoved: Store.setChannelAppearance(channelRow.key, strokeSlider.value, channelRow.fillOpacity, channelRow.referenceColor)
                    }
                    Label {
                        Layout.preferredWidth: 48
                        font.family: Style.monoFontFamily
                        text: channelRow.strokeWidth.toFixed(2) + " px"
                    }
                    Label {
                        color: Style.mutedTextColor
                        text: "Fill"
                    }
                    Slider {
                        id: fillSlider

                        Accessible.name: channelRow.title + " area opacity"
                        Layout.fillWidth: true
                        from: 0.0
                        stepSize: 0.01
                        to: 1.0
                        value: channelRow.fillOpacity

                        onMoved: Store.setChannelAppearance(channelRow.key, channelRow.strokeWidth, fillSlider.value, channelRow.referenceColor)
                    }
                    Label {
                        Layout.preferredWidth: 34
                        font.family: Style.monoFontFamily
                        text: Math.round(channelRow.fillOpacity * 100) + "%"
                    }
                    CompactButton {
                        text: "Reset style"

                        onClicked: Store.resetChannelAppearance(channelRow.key)
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
                text: channelListView.count + " channels"
            }
            CompactButton {
                text: "Close"

                onClicked: channelsWindow.hide()
            }
        }
    }
}
