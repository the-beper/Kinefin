// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// The keyboard shortcuts, in an overlay: each key in a small box, the action next to it
Kirigami.Dialog {
    id: dialog

    // The tab actions of the shell: Ctrl+N is the position among the visible tabs, so the rows follow the same list
    property var tabActions: []
    // The SDL3 gamepads of the shell; null in shell tests and when SDL is off
    property var gamepad: null
    readonly property var tabRows: tabActions.filter(action => action.visible).map((action, index) => ["Ctrl+" + (index + 1), action.text])
    readonly property var faceLabels: gamepad && gamepad.connectedCount > 0 ? gamepad.faceLabels : ["A", "B", "X", "Y"]

    readonly property var rows: [
        ["Ctrl+Q", i18nc("@item:intable shortcut action", "Quit Kinefin")],
        ["Escape", i18nc("@item:intable shortcut action", "Go Back")],
        ["Alt+Left", i18nc("@item:intable shortcut action", "Go Back")],
        ["Ctrl+F", i18nc("@item:intable shortcut action", "Search")],
        ["Ctrl+R", i18nc("@item:intable shortcut action", "Refresh")],
        ...dialog.tabRows,
        ["Ctrl+PgUp", i18nc("@item:intable shortcut action", "Previous Tab")],
        ["Ctrl+PgDown", i18nc("@item:intable shortcut action", "Next Tab")],
        ["Ctrl+[", i18nc("@item:intable shortcut action", "Previous Tab")],
        ["Ctrl+]", i18nc("@item:intable shortcut action", "Next Tab")],
        ["Ctrl+Shift+Tab", i18nc("@item:intable shortcut action", "Previous Tab")],
        ["Ctrl+Tab", i18nc("@item:intable shortcut action", "Next Tab")],
        ["Menu", i18nc("@item:intable shortcut action", "Open Item Menu")],
        ["Shift+F10", i18nc("@item:intable shortcut action", "Open Item Menu")],
        ["Ctrl+Z", i18nc("@item:intable shortcut action", "Undo Last Queue Change")],
        ["Ctrl+Up", i18nc("@item:intable shortcut action", "Move Queue or Playlist Item Up")],
        ["Ctrl+Down", i18nc("@item:intable shortcut action", "Move Queue or Playlist Item Down")],
        ["Delete", i18nc("@item:intable shortcut action", "Remove Queue or Playlist Item")],
        ["F2", i18nc("@item:intable shortcut action", "Rename Playlist")],
        ["Ctrl+S", i18nc("@item:intable shortcut action", "Save User Preferences")],
        ["Ctrl+,", i18nc("@item:intable shortcut action", "Settings")],
        ["Ctrl+Shift+P", i18nc("@item:intable shortcut action", "Expand Player")],
        ["Ctrl+Shift+/", i18nc("@item:intable shortcut action", "Show Keyboard Shortcuts")]
    ]
    // The keys of the player page (PlayerPage.qml)
    readonly property var playerRows: [
        ["Space", i18nc("@item:intable shortcut action", "Play or Pause")],
        ["M", i18nc("@item:intable shortcut action", "Mute or Unmute")],
        ["+", i18nc("@item:intable shortcut action", "Volume Up")],
        ["-", i18nc("@item:intable shortcut action", "Volume Down")],
        ["F", i18nc("@item:intable shortcut action", "Toggle Full Screen")],
        ["F11", i18nc("@item:intable shortcut action", "Toggle Full Screen")],
        ["Escape", i18nc("@item:intable shortcut action", "Cancel Up Next, Leave Full Screen, or Go Back")],
        ["Return", i18nc("@item:intable shortcut action", "Play the Up Next Item Now")],
        ["Left", i18nc("@item:intable shortcut action", "Skip Back 10 Seconds")],
        ["Right", i18nc("@item:intable shortcut action", "Skip Forward 10 Seconds")],
        ["Down", i18nc("@item:intable shortcut action", "Skip Back 1 Minute")],
        ["Up", i18nc("@item:intable shortcut action", "Skip Forward 1 Minute")],
        ["PgDown", i18nc("@item:intable shortcut action", "Previous Chapter")],
        ["PgUp", i18nc("@item:intable shortcut action", "Next Chapter")],
        ["I", i18nc("@item:intable shortcut action", "Set the Clip Start")],
        ["O", i18nc("@item:intable shortcut action", "Set the Clip End")]
    ]
    // The pad's labels are its own (A, B, X, Y or Cross, Circle, Square, Triangle); the dialog shows these groups only while a pad is connected
    readonly property var gamepadRows: [
        [i18nc("@item:intable game controller button", "D-pad"), i18nc("@item:intable game controller control", "Move the Focus")],
        [i18nc("@item:intable game controller button", "Left Stick"), i18nc("@item:intable game controller control", "Scroll")],
        [faceLabels[0], i18nc("@item:intable game controller control", "Open or Activate")],
        [faceLabels[1], i18nc("@item:intable shortcut action", "Go Back")],
        [faceLabels[2], i18nc("@item:intable shortcut action", "Open Item Menu")],
        [faceLabels[3], i18nc("@item:intable shortcut action", "Play or Pause")],
        [i18nc("@item:intable game controller button", "Start"), i18nc("@item:intable game controller control", "Open the Main Menu")],
        [i18nc("@item:intable game controller button", "Select"), i18nc("@item:intable shortcut action", "Expand Player")],
        [i18nc("@item:intable game controller button", "Shoulder Buttons"), i18nc("@item:intable game controller control", "Previous or Next Tab")],
        [i18nc("@item:intable game controller button", "Triggers"), i18nc("@item:intable game controller control", "Previous or Next Chapter")],
        [i18nc("@item:intable game controller button", "Right Stick"), i18nc("@item:intable shortcut action", "Volume Up or Down")]
    ]
    readonly property var gamepadPlayerRows: [
        [i18nc("@item:intable game controller button", "D-pad or Left Stick"), i18nc("@item:intable game controller control", "Skip 10 Seconds or 1 Minute")],
        [faceLabels[0], i18nc("@item:intable game controller control", "Play or Pause, or Start Up Next")],
        [faceLabels[1], i18nc("@item:intable shortcut action", "Cancel Up Next, Leave Full Screen, or Go Back")],
        [faceLabels[2], i18nc("@item:intable shortcut action", "Open the Player Menu")],
        [faceLabels[3], i18nc("@item:intable shortcut action", "Play or Pause")],
        [i18nc("@item:intable game controller button", "Start"), i18nc("@item:intable shortcut action", "Toggle Full Screen")],
        [i18nc("@item:intable game controller button", "Select"), i18nc("@item:intable shortcut action", "Expand Player")],
        [i18nc("@item:intable game controller button", "Shoulder Buttons"), i18nc("@item:intable game controller control", "Previous or Next Item")],
        [i18nc("@item:intable game controller button", "Triggers"), i18nc("@item:intable game controller control", "Previous or Next Chapter")],
        [i18nc("@item:intable game controller button", "Right Stick"), i18nc("@item:intable shortcut action", "Volume Up or Down")]
    ]

    // The keys of a combination and the "+" between them; a "+" on its own or at the end ("Ctrl++") is a key
    function keyParts(keys) {
        const names = keys === "+" ? ["+"] : keys.endsWith("++") ? keys.slice(0, -2).split("+").concat(["+"]) : keys.split("+");
        const parts = [];
        names.forEach((name, i) => {
            if (i > 0) {
                parts.push({ text: "+", separator: true });
            }
            parts.push({ text: name, separator: false });
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
                QQC2.ToolTip.text: text
                QQC2.ToolTip.visible: hovered || visualFocus
                QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
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

            Kirigami.Heading {
                level: 4
                text: i18nc("@title:group keyboard shortcuts", "General")
            }
            Repeater {
                model: dialog.rows
                delegate: shortcutRow
            }
            Kirigami.Heading {
                level: 4
                Layout.topMargin: Kirigami.Units.largeSpacing
                text: i18nc("@title:group keyboard shortcuts", "Player")
            }
            Repeater {
                model: dialog.playerRows
                delegate: shortcutRow
            }
            Kirigami.Heading {
                level: 4
                Layout.topMargin: Kirigami.Units.largeSpacing
                visible: dialog.gamepad && dialog.gamepad.connectedCount > 0
                text: i18nc("@title:group game controller buttons", "Game Controller")
            }
            Repeater {
                model: dialog.gamepad && dialog.gamepad.connectedCount > 0 ? dialog.gamepadRows : []
                delegate: shortcutRow
            }
            Kirigami.Heading {
                level: 4
                Layout.topMargin: Kirigami.Units.largeSpacing
                visible: dialog.gamepad && dialog.gamepad.connectedCount > 0
                text: i18nc("@title:group game controller buttons", "Game Controller in the Player")
            }
            Repeater {
                model: dialog.gamepad && dialog.gamepad.connectedCount > 0 ? dialog.gamepadPlayerRows : []
                delegate: shortcutRow
            }
        }

        Component {
            id: shortcutRow

            RowLayout {
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

                            required property var modelData
                            readonly property bool plus: modelData.separator

                            radius: Kirigami.Units.cornerRadius
                            color: plus ? "transparent" : Qt.alpha(Kirigami.Theme.textColor, 0.06)
                            border.width: plus ? 0 : 1
                            border.color: Qt.alpha(Kirigami.Theme.textColor, 0.25)
                            implicitWidth: plus ? label.implicitWidth : Math.max(label.implicitWidth + Kirigami.Units.largeSpacing * 2, implicitHeight)
                            implicitHeight: label.implicitHeight + Kirigami.Units.smallSpacing * 2

                            QQC2.Label {
                                id: label

                                anchors.centerIn: parent
                                text: key.modelData.text
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
