// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// The keyboard shortcuts, in an overlay: each key in a small box, the action next to it
Kirigami.Dialog {
    id: dialog

    readonly property var rows: [
        ["Ctrl+Q", i18nc("@item:intable shortcut action", "Quit Kinefin")],
        ["Escape", i18nc("@item:intable shortcut action", "Go Back")],
        ["Alt+Left", i18nc("@item:intable shortcut action", "Go Back")],
        ["Ctrl+F", i18nc("@item:intable shortcut action", "Search")],
        ["F5", i18nc("@item:intable shortcut action", "Refresh")],
        ["Ctrl+1", i18nc("@item:intable shortcut action", "Home")],
        ["Ctrl+2", i18nc("@item:intable shortcut action", "Movies")],
        ["Ctrl+3", i18nc("@item:intable shortcut action", "Shows")],
        ["Ctrl+PgUp", i18nc("@item:intable shortcut action", "Previous Tab")],
        ["Ctrl+PgDown", i18nc("@item:intable shortcut action", "Next Tab")],
        ["Ctrl+[", i18nc("@item:intable shortcut action", "Previous Tab")],
        ["Ctrl+]", i18nc("@item:intable shortcut action", "Next Tab")],
        ["Ctrl+Shift+Tab", i18nc("@item:intable shortcut action", "Previous Tab")],
        ["Ctrl+Tab", i18nc("@item:intable shortcut action", "Next Tab")],
        ["Menu", i18nc("@item:intable shortcut action", "Open Item Menu")],
        ["Shift+F10", i18nc("@item:intable shortcut action", "Open Item Menu")],
        ["Ctrl+Z", i18nc("@item:intable shortcut action", "Undo Last Queue Change")],
        ["Ctrl+Up", i18nc("@item:intable shortcut action", "Move Queue Item Up")],
        ["Ctrl+Down", i18nc("@item:intable shortcut action", "Move Queue Item Down")],
        ["Delete", i18nc("@item:intable shortcut action", "Remove Queue Item")],
        ["Ctrl+,", i18nc("@item:intable shortcut action", "Settings")],
        ["Ctrl+Shift+/", i18nc("@item:intable shortcut action", "Show Keyboard Shortcuts")]
    ]

    function keyParts(keys) {
        const parts = [];
        keys.split("+").forEach((key, i) => {
            if (i > 0) {
                parts.push("+");
            }
            parts.push(key);
        });
        return parts;
    }

    // The room around the header icon and the close button, the same on every side, and the room around the content
    readonly property real margin: Kirigami.Units.largeSpacing
    readonly property real contentMargin: Kirigami.Units.largeSpacing * 2
    readonly property real headerSize: Kirigami.Units.iconSizes.medium

    title: i18n("Keyboard Shortcuts")
    standardButtons: Kirigami.Dialog.NoButton
    footer: null
    preferredWidth: Kirigami.Units.gridUnit * 24
    // Room above and below for the opening animation, which starts the dialog two grid units lower: the dialog must not reach the window edge while it moves, or Qt reports a binding loop on y. A taller content scrolls
    maximumHeight: absoluteMaximumHeight - Kirigami.Units.gridUnit * 4

    header: QQC2.Control {
        // Every side is set on its own because the desktop style ignores `padding` for the top and bottom; the header spans the dialog border (1 px at the top and the sides) and the content sits inside it
        topPadding: dialog.margin + 1
        bottomPadding: dialog.margin
        leftPadding: dialog.margin + 1
        rightPadding: dialog.margin + 1
        // The icon and the close button are the same size, so the header has the same room above, below and at the sides of what you see: the icon, and the box of the button (it shows when focused)
        contentItem: RowLayout {
            spacing: Kirigami.Units.largeSpacing

            Kirigami.Icon {
                source: "input-keyboard-symbolic"
                Layout.preferredWidth: dialog.headerSize
                Layout.preferredHeight: dialog.headerSize
            }
            Kirigami.Heading {
                level: 3
                text: dialog.title
                Layout.fillWidth: true
            }
            QQC2.ToolButton {
                icon.name: "window-close"
                icon.width: Kirigami.Units.iconSizes.smallMedium
                icon.height: Kirigami.Units.iconSizes.smallMedium
                display: QQC2.AbstractButton.IconOnly
                text: i18nc("@action:button", "Close")
                Layout.preferredWidth: dialog.headerSize
                Layout.preferredHeight: dialog.headerSize
                onClicked: dialog.close()
            }
        }
    }

    // The dialog only sizes itself from an Item's implicitHeight; a layout alone gives a collapsed dialog
    Item {
        implicitHeight: column.implicitHeight + dialog.contentMargin * 2

        ColumnLayout {
            id: column

            anchors {
                left: parent.left
                right: parent.right
                top: parent.top
                leftMargin: dialog.contentMargin
                rightMargin: dialog.contentMargin
                topMargin: dialog.contentMargin
            }
            spacing: Kirigami.Units.largeSpacing

            Repeater {
                model: dialog.rows
                delegate: RowLayout {
                    id: row

                    required property var modelData

                    spacing: Kirigami.Units.largeSpacing * 2
                    Layout.fillWidth: true

                    RowLayout {
                        spacing: Kirigami.Units.smallSpacing
                        // One width for all the rows, so the keys end on the same line and descriptions start on the same line
                        Layout.minimumWidth: Kirigami.Units.gridUnit * 12
                        Layout.maximumWidth: Kirigami.Units.gridUnit * 12
                        Layout.preferredWidth: Kirigami.Units.gridUnit * 12
                        Layout.alignment: Qt.AlignVCenter

                        // The parts sit left-aligned at a constant gap; the spacer takes the rest of the fixed column, so the descriptions start at one x
                        Repeater {
                            model: dialog.keyParts(row.modelData[0])
                            delegate: Rectangle {
                                id: key
                                objectName: "shortcutsKeyPart"

                                required property string modelData
                                readonly property bool plus: modelData === "+"

                                radius: Kirigami.Units.cornerRadius
                                color: plus ? "transparent" : Qt.alpha(Kirigami.Theme.textColor, 0.06)
                                border.width: plus ? 0 : 1
                                border.color: Qt.alpha(Kirigami.Theme.textColor, 0.25)
                                implicitWidth: plus ? label.implicitWidth : Math.max(label.implicitWidth + Kirigami.Units.largeSpacing * 2, implicitHeight)
                                implicitHeight: label.implicitHeight + Kirigami.Units.smallSpacing * 2

                                QQC2.Label {
                                    id: label

                                    anchors.centerIn: parent
                                    text: key.modelData
                                    opacity: key.plus ? 0.7 : 1
                                }
                            }
                        }
                        Item {
                            Layout.fillWidth: true
                        }
                    }
                    QQC2.Label {
                        objectName: "shortcutsDescription"
                        text: row.modelData[1]
                        wrapMode: Text.Wrap
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                    }
                }
            }
        }
    }
}
