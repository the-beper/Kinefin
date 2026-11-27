// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kirigamiaddons.components as Components

// A square tile of a user: the round avatar (or the initial) with the name under it, a lock badge for a user that asks for the password, and the button of the tile's menu where the page offers one. The page owns the current tile and the keys: it sets `highlighted` and reacts to `pressed`, `clicked` and `menuRequested`
Controls.ItemDelegate {
    id: tile

    // The user name and the avatar URL come from the server: the name is plain text and the URL is one built by JellyfinClient::userImageUrl
    property string name
    property string imageUrl
    property bool locked: false
    property bool showMenu: false
    // A line of small text under the name, and whether the tile keeps room for it when it is empty
    property string caption
    property bool captionShown: false
    // The side of the tile; the avatar takes what is left once the name (and the caption) have their lines
    property real size: Kirigami.Units.gridUnit * 9
    property string accessibleLabel: name
    readonly property real avatarSize: size - Kirigami.Units.gridUnit * (captionShown ? 4 : 3)
    readonly property string lockedText: i18nc("@info:tooltip", "Asks for the password")
    // The current tile shows only once the keys (or a pad) drive the window, as the cards do: a page that opens with a current tile lights nothing
    readonly property bool ring: highlighted && (Window.window?.keyboardDriven ?? true)

    // The pointer's right click passes no button: the menu opens at the point
    signal menuRequested(real x, real y, Item button)

    width: size
    height: size
    // The page's list is the one tab stop, the arrow keys move inside it
    focusPolicy: Qt.NoFocus
    Accessible.name: accessibleLabel
    Accessible.description: [caption, locked ? lockedText : ""].filter(part => part.length > 0).join(" · ")
    Controls.ToolTip.text: lockedText
    Controls.ToolTip.visible: hovered && locked && !menuButton.hovered && !nameLabel.truncated

    // A name too long for its line shows whole here, with the lock's words when there is one; the name is server text
    PlainToolTip {
        text: tile.locked ? tile.name + " · " + tile.lockedText : tile.name
        visible: tile.hovered && nameLabel.truncated && !menuButton.hovered
    }
    Controls.ToolTip.delay: Kirigami.Units.toolTipDelay

    TapHandler {
        acceptedButtons: Qt.RightButton
        enabled: tile.showMenu
        onTapped: eventPoint => tile.menuRequested(eventPoint.position.x, eventPoint.position.y, null)
    }

    // The style's highlight fills the whole tile: here the current one has a ring only, like the cards
    background: Rectangle {
        objectName: "userTileBackground"
        radius: Kirigami.Units.cornerRadius
        color: tile.pressed ? Qt.alpha(Kirigami.Theme.highlightColor, 0.3) : (tile.hovered ? Qt.alpha(Kirigami.Theme.highlightColor, 0.15) : "transparent")
        border.width: tile.ring ? 2 : 0
        border.color: Kirigami.Theme.highlightColor
    }

    contentItem: ColumnLayout {
        spacing: Kirigami.Units.smallSpacing

        Item {
            Layout.fillHeight: true
        }

        Item {
            Layout.alignment: Qt.AlignHCenter
            Layout.preferredWidth: tile.avatarSize
            Layout.preferredHeight: tile.avatarSize

            Components.Avatar {
                objectName: "userAvatar"
                anchors.fill: parent
                // The tile names the user to a screen reader
                Accessible.ignored: true
                name: tile.name
                // Only URLs built by JellyfinClient::userImageUrl belong here: a raw server string must never become a source
                source: tile.imageUrl
                // The image is decoded at the size it shows
                sourceSize.width: width * Screen.devicePixelRatio
                sourceSize.height: height * Screen.devicePixelRatio
            }

            Rectangle {
                objectName: "userLock"
                anchors {
                    right: parent.right
                    bottom: parent.bottom
                }
                width: Kirigami.Units.iconSizes.smallMedium + Kirigami.Units.smallSpacing
                height: width
                radius: width / 2
                visible: tile.locked
                color: Kirigami.Theme.backgroundColor
                border.width: 1
                border.color: Kirigami.Theme.disabledTextColor

                Kirigami.Icon {
                    anchors.centerIn: parent
                    width: Kirigami.Units.iconSizes.small
                    height: width
                    source: "lock"
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 0

            // As wide as the menu button: the name stays centred under the avatar
            Item {
                visible: tile.showMenu
                Layout.preferredWidth: Kirigami.Units.iconSizes.smallMedium
            }

            // User names come from the server: plain text only
            Controls.Label {
                id: nameLabel
                objectName: "userName"
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                elide: Text.ElideRight
                maximumLineCount: 1
                textFormat: Text.PlainText
                text: tile.name
            }

            Controls.ToolButton {
                id: menuButton
                objectName: "userMenuButton"
                Layout.preferredWidth: Kirigami.Units.iconSizes.smallMedium
                Layout.preferredHeight: Kirigami.Units.iconSizes.smallMedium
                focusPolicy: Qt.NoFocus
                padding: 0
                visible: tile.showMenu
                display: Controls.AbstractButton.IconOnly
                icon.name: "overflow-menu"
                icon.width: Kirigami.Units.iconSizes.small
                icon.height: Kirigami.Units.iconSizes.small
                text: i18nc("@action:button", "More Actions")
                // The tooltip keeps the short text: a screen reader hears which user the button belongs to
                Accessible.name: i18nc("@label %1 is a user name", "More Actions for %1", tile.name)
                Controls.ToolTip.text: text
                Controls.ToolTip.visible: hovered
                Controls.ToolTip.delay: Kirigami.Units.toolTipDelay
                onClicked: tile.menuRequested(0, 0, menuButton)
            }
        }

        Controls.Label {
            objectName: "userCaption"
            visible: tile.captionShown
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            elide: Text.ElideRight
            maximumLineCount: 1
            font: Kirigami.Theme.smallFont
            textFormat: Text.PlainText
            text: tile.caption
            // The tile names it to a screen reader
            Accessible.ignored: true
        }

        Item {
            Layout.fillHeight: true
        }
    }
}
