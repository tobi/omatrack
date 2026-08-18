pragma ComponentBehavior: Bound
import Omatrack

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: driversPage

    property string driverFilter: ""
    property string mappingEditKey: ""
    property string mappingEditName: ""

    function beginEdit(key, displayName): void {
        driversPage.mappingEditKey = key;
        driversPage.mappingEditName = displayName;
    }
    function cancelEdit(): void {
        driversPage.mappingEditKey = "";
        driversPage.mappingEditName = "";
    }
    function refresh(): void {
        // The model auto-refreshes on driverMappingsChanged; this just
        // re-triggers the filter in case the source model replaced rows.
        mappingFilter.invalidate();
    }
    function saveEdit(): void {
        if (driversPage.mappingEditKey === "" || driversPage.mappingEditName.trim() === "")
            return;
        Store.setDriverMapping(driversPage.mappingEditKey, driversPage.mappingEditName);
        driversPage.cancelEdit();
    }

    objectName: "preferencesDriversPage"

    Component.onCompleted: driversPage.refresh()
    onVisibleChanged: {
        if (visible)
            driversPage.refresh();
        else
            driversPage.cancelEdit();
    }

    RowFilterModel {
        id: mappingFilter

        filterText: driversPage.driverFilter
        sourceModel: Store.driverMappings
    }
    Connections {
        function onDriverMappingsChanged(): void {
        }

        target: Store
    }
    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 20
        spacing: 12

        RowLayout {
            Layout.fillWidth: true
            spacing: 12

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2

                Label {
                    font.bold: true
                    font.pixelSize: 16
                    text: "Driver identity"
                }
                Label {
                    color: Style.mutedTextColor
                    text: "One name follows the same car number, class, and logger ID across sessions."
                }
            }
            Label {
                color: Style.mutedTextColor
                font.family: Style.monoFontFamily
                font.pixelSize: Style.smallFontSize
                text: Store.driverMappings.rowCount + (Store.driverMappings.rowCount === 1 ? " driver" : " drivers")
            }
        }
        CompactTextField {
            Layout.fillWidth: true
            placeholderText: "Filter by driver, car, class, or logger ID"

            onTextEdited: driversPage.driverFilter = text
        }
        Rectangle {
            Layout.fillHeight: true
            Layout.fillWidth: true
            border.color: Style.borderColor
            border.width: 1
            color: Style.traceBackgroundColor
            radius: 6

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 1
                spacing: 0

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 34
                    color: Style.surfaceColor

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 12
                        anchors.rightMargin: 12

                        Label {
                            Layout.fillWidth: true
                            font.bold: true
                            text: "Display name and identity key"
                        }
                        Label {
                            color: Style.dimTextColor
                            font.family: Style.monoFontFamily
                            font.pixelSize: Style.smallFontSize
                            text: "Changes apply immediately"
                        }
                    }
                }
                Item {
                    Layout.fillHeight: true
                    Layout.fillWidth: true

                    Label {
                        anchors.centerIn: parent
                        color: Style.mutedTextColor
                        horizontalAlignment: Text.AlignHCenter
                        text: Store.driverMappings.rowCount === 0 ? "No drivers indexed yet\nAdd a telemetry folder and scan the library first." : "No drivers match this filter."
                        visible: mappingList.count === 0
                    }
                    ListView {
                        id: mappingList

                        anchors.fill: parent
                        clip: true
                        model: mappingFilter

                        ScrollBar.vertical: ThinScrollBar {
                        }
                        delegate: Rectangle {
                            id: mappingRow

                            required property string carClass
                            required property string carNumber
                            required property string display
                            required property string driverId
                            readonly property bool editing: driversPage.mappingEditKey === mappingRow.key
                            required property int index
                            required property string key

                            color: mappingRow.index % 2 === 0 ? "transparent" : Qt.rgba(1, 1, 1, 0.025)
                            height: 58
                            width: ListView.view.width

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 12
                                anchors.rightMargin: 8
                                spacing: 8

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 1

                                    Label {
                                        Layout.fillWidth: true
                                        color: Style.accentColor
                                        elide: Text.ElideRight
                                        font.bold: true
                                        text: mappingRow.display
                                        visible: !mappingRow.editing
                                    }
                                    CompactTextField {
                                        Layout.fillWidth: true
                                        objectName: "driverRenameEditor" + mappingRow.index
                                        text: driversPage.mappingEditName
                                        visible: mappingRow.editing

                                        Keys.onEscapePressed: driversPage.cancelEdit()
                                        onAccepted: driversPage.saveEdit()
                                        onTextEdited: driversPage.mappingEditName = text
                                        onVisibleChanged: {
                                            if (visible)
                                                forceActiveFocus();
                                        }
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        color: Style.mutedTextColor
                                        elide: Text.ElideRight
                                        font.family: Style.monoFontFamily
                                        font.pixelSize: Style.smallFontSize
                                        text: "Car " + (mappingRow.carNumber || "—") + "  ·  " + (mappingRow.carClass || "Unknown class") + "  ·  Logger ID " + (mappingRow.driverId || "—")
                                    }
                                }
                                CompactButton {
                                    enabled: !mappingRow.editing || driversPage.mappingEditName.trim() !== ""
                                    text: mappingRow.editing ? "Save" : "Rename"

                                    onClicked: {
                                        if (mappingRow.editing)
                                            driversPage.saveEdit();
                                        else
                                            driversPage.beginEdit(mappingRow.key, mappingRow.display);
                                    }
                                }
                                CompactButton {
                                    text: mappingRow.editing ? "Cancel" : "Reset"

                                    onClicked: {
                                        if (mappingRow.editing) {
                                            driversPage.cancelEdit();
                                        } else {
                                            Store.setDriverMapping(mappingRow.key, "");
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        Label {
            Layout.fillWidth: true
            color: Style.dimTextColor
            font.pixelSize: Style.smallFontSize
            text: "Reset removes the saved override and restores the name detected from telemetry."
            wrapMode: Text.Wrap
        }
    }
}
