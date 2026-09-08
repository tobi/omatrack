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
    required property TraceView trace

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
    minimumWidth: 720
    objectName: "channelsWindow"
    title: "Channels"
    visible: false
    width: 820

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
                text: "Channels"
            }
            CompactButton {
                enabled: Store.ready
                text: "Resize lanes…"

                onClicked: {
                    Store.beginTraceResize();
                    channelsWindow.hide();
                }
            }
            TextField {
                id: channelSearch

                Layout.preferredHeight: 32
                Layout.preferredWidth: 220
                placeholderText: "Search channels"
                selectByMouse: true
                text: channelsWindow.filterText

                onTextChanged: channelsWindow.filterText = text
            }
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

                required property bool canCombine
                required property string channelColor
                required property bool channelVisible
                required property bool combineWithPrevious
                required property double fillOpacity
                required property double heightPercent
                required property int index
                required property string key
                required property string referenceColor
                required property bool span
                required property double strokeWidth
                required property string title
                required property string unit

                color: channelsWindow.appearanceKey === channelRow.key ? Style.surfaceColor : channelRow.index % 2 === 0 ? Style.surfaceColor : Style.backgroundColor
                height: channelsWindow.appearanceKey === channelRow.key ? (channelRow.span ? 82 : 126) : 40
                radius: 2
                width: ListView.view.width

                Rectangle {
                    anchors.bottom: parent.bottom
                    anchors.left: parent.left
                    anchors.leftMargin: 6
                    anchors.right: parent.right
                    anchors.rightMargin: 6
                    color: Style.darkBackgroundColor
                    height: channelRow.span ? 36 : 80
                    radius: 2
                    visible: channelsWindow.appearanceKey === channelRow.key
                }
                RowLayout {
                    anchors.left: parent.left
                    anchors.leftMargin: 7
                    anchors.right: parent.right
                    anchors.rightMargin: 7
                    anchors.top: parent.top
                    height: 40
                    spacing: 7

                    Switch {
                        Layout.preferredWidth: 42
                        checked: channelRow.channelVisible
                        scale: 0.78

                        onToggled: Store.setChannelVisible(channelRow.key, checked)
                    }
                    Rectangle {
                        Accessible.ignored: true
                        Layout.preferredHeight: 20
                        Layout.preferredWidth: 4
                        color: channelRow.channelColor
                        radius: 2
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
                    CompactButton {
                        ToolTip.text: "Colors and line appearance"
                        ToolTip.visible: hovered
                        text: "Style"

                        onClicked: channelsWindow.appearanceKey = channelsWindow.appearanceKey === channelRow.key ? "" : channelRow.key
                    }
                    CheckBox {
                        Layout.preferredWidth: 82
                        checked: channelRow.combineWithPrevious
                        enabled: channelRow.canCombine && !channelRow.span
                        text: "Overlay"
                        visible: channelRow.canCombine && !channelRow.span

                        onToggled: Store.setChannelCombined(channelRow.key, checked)
                    }
                    Label {
                        color: Style.mutedTextColor
                        text: "Height"
                    }
                    SpinBox {
                        id: laneHeightSpin

                        Accessible.name: channelRow.title + " lane height"
                        Layout.preferredHeight: 26
                        Layout.preferredWidth: 78
                        ToolTip.text: channelRow.combineWithPrevious ? "Height of this shared line" : "Lane height"
                        ToolTip.visible: hovered
                        editable: true
                        enabled: !channelRow.span
                        from: 1
                        stepSize: 1
                        textFromValue: function (value, locale) {
                            return Number(value).toLocaleString(locale, 'f', 0) + "%";
                        }
                        to: 100
                        value: Math.round(channelRow.heightPercent)
                        valueFromText: function (text, locale) {
                            return Number.fromLocaleString(locale, text.replace("%", ""));
                        }

                        onValueModified: {
                            Store.setChannelLaneHeightPercent(channelRow.key, value);
                            channelsWindow.trace.fitChannels = false;
                        }
                    }
                }
                ColumnLayout {
                    anchors.left: parent.left
                    anchors.leftMargin: 16
                    anchors.right: parent.right
                    anchors.rightMargin: 16
                    anchors.top: parent.top
                    anchors.topMargin: 44
                    spacing: 4
                    visible: channelsWindow.appearanceKey === channelRow.key

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 32
                        spacing: 8

                        Label {
                            Layout.preferredWidth: 72
                            color: Style.mutedTextColor
                            text: "Colors"
                        }
                        TraceColorButton {
                            text: "Active"
                            traceColor: channelRow.channelColor

                            onColorSelected: value => Store.setChannelTraceColors(channelRow.key, value, channelRow.referenceColor)
                        }
                        TraceColorButton {
                            enabled: !channelRow.span
                            text: "Reference"
                            traceColor: channelRow.referenceColor

                            onColorSelected: value => Store.setChannelTraceColors(channelRow.key, channelRow.channelColor, value)
                        }
                        Item {
                            Layout.fillWidth: true
                        }
                        CompactButton {
                            text: "Reset"

                            onClicked: Store.resetChannelAppearance(channelRow.key)
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 36
                        spacing: 8
                        visible: !channelRow.span

                        Label {
                            Layout.preferredWidth: 72
                            color: Style.mutedTextColor
                            text: "Line width"
                        }
                        Slider {
                            id: strokeSlider

                            Accessible.name: channelRow.title + " stroke width"
                            Layout.preferredWidth: 110
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
                            Layout.preferredWidth: 24
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
