pragma ComponentBehavior: Bound
import Omatrack

// Dedicated corner-edit mode. Zone drag lives on the traces; this panel is
// the rest of the editor: rename, add, auto-generate, delete, and the single
// Save that writes the override. Cancel restores the snapshot taken on enter.
// Analysis (CornerFocusOverlay) is hidden while this is open.

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: overlay

    function addAtCursor(): void {
        const width = 0.04;
        const start = Math.max(0, Math.min(1 - width, Store.cursorFrac - width / 2));
        Store.addCorner(start, start + width);
    }
    function rangeText(index: int): string {
        const start = Store.cornerStart(index);
        const end = Store.cornerEnd(index);
        return (start * 100).toFixed(1) + "–" + (end * 100).toFixed(1) + "%";
    }

    border.color: Style.magentaColor
    border.width: 1
    clip: true
    color: Style.surfaceColor
    implicitHeight: headerColumn.implicitHeight + listColumn.implicitHeight + footerRow.implicitHeight + 28
    objectName: "cornerEditOverlay"
    radius: 6
    visible: Store.editingCorners

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 8
        spacing: 6

        ColumnLayout {
            id: headerColumn

            Layout.fillWidth: true
            spacing: 4

            RowLayout {
                Layout.fillWidth: true
                spacing: 4

                Label {
                    Layout.fillWidth: true
                    color: Style.magentaColor
                    elide: Text.ElideRight
                    font.bold: true
                    font.family: Style.monoFontFamily
                    font.letterSpacing: 0.8
                    font.pixelSize: Style.smallFontSize
                    text: "EDIT CORNERS"
                }
                Label {
                    Accessible.name: "Cancel corner edit"
                    Accessible.role: Accessible.Button
                    color: Style.mutedTextColor
                    font.pixelSize: Style.fontSize + 4
                    objectName: "cornerEditClose"
                    text: "×"

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor

                        onClicked: Store.cancelCornerEdit()
                    }
                }
            }
            Label {
                Layout.fillWidth: true
                color: Style.mutedTextColor
                font.pixelSize: Style.smallFontSize
                text: "Drag zone edges on the traces. Save writes a local override for this track."
                wrapMode: Text.Wrap
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 4

                CompactButton {
                    Layout.fillWidth: true
                    objectName: "cornerEditGenerate"
                    text: "Auto-generate"

                    onClicked: Store.autoGenerateCorners()
                }
                CompactButton {
                    Layout.fillWidth: true
                    objectName: "cornerEditAdd"
                    text: "Add at cursor"

                    onClicked: overlay.addAtCursor()
                }
            }
        }
        ScrollView {
            id: overlayScroll

            Layout.fillHeight: true
            Layout.fillWidth: true
            Layout.minimumHeight: 72
            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
            clip: true
            contentWidth: overlayScroll.availableWidth

            ColumnLayout {
                id: listColumn

                spacing: 2
                width: overlayScroll.availableWidth

                Repeater {
                    model: Store.cornerCount

                    delegate: Rectangle {
                        id: cornerRow

                        required property int index

                        Layout.fillWidth: true
                        color: Store.focusedCorner === cornerRow.index ? Style.selectionColor : "transparent"
                        implicitHeight: rowBody.implicitHeight + 4
                        radius: 3

                        RowLayout {
                            id: rowBody

                            anchors.fill: parent
                            anchors.margins: 2
                            spacing: 4

                            Label {
                                Layout.preferredWidth: 16
                                color: Style.dimTextColor
                                font.family: Style.monoFontFamily
                                font.pixelSize: Style.smallFontSize
                                text: String(cornerRow.index + 1)
                            }
                            CompactTextField {
                                Layout.fillWidth: true
                                Layout.preferredHeight: Style.smallControlHeight
                                font.pixelSize: Style.smallFontSize
                                objectName: "cornerEditName-" + cornerRow.index
                                text: Store.cornerName(cornerRow.index)

                                onEditingFinished: Store.setCornerName(cornerRow.index, text)
                            }
                            Label {
                                color: Style.mutedTextColor
                                font.family: Style.monoFontFamily
                                font.pixelSize: Style.smallFontSize
                                text: overlay.rangeText(cornerRow.index)

                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor

                                    onClicked: Store.focusCorner(cornerRow.index)
                                }
                            }
                            ToolButton {
                                Layout.preferredHeight: Style.smallControlHeight
                                Layout.preferredWidth: Style.smallControlHeight
                                font.pixelSize: Style.smallFontSize
                                objectName: "cornerEditDelete-" + cornerRow.index
                                text: "×"

                                onClicked: Store.deleteCorner(cornerRow.index)
                            }
                        }
                    }
                }
                Label {
                    Layout.fillWidth: true
                    color: Style.dimTextColor
                    font.pixelSize: Style.smallFontSize
                    text: "No zones. Auto-generate from brake, or add at the cursor."
                    visible: Store.cornerCount === 0
                    wrapMode: Text.Wrap
                }
            }
        }
        RowLayout {
            id: footerRow

            Layout.fillWidth: true
            spacing: 4

            CompactButton {
                Layout.fillWidth: true
                objectName: "cornerEditCancel"
                text: "Cancel"

                onClicked: Store.cancelCornerEdit()
            }
            CompactButton {
                Layout.fillWidth: true
                font.bold: true
                objectName: "cornerEditSave"
                text: "Save"

                onClicked: Store.commitCornerEdit()
            }
        }
    }
}
