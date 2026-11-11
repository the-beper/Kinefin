// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kirigamiaddons.components as Components

// Who is watching: one tile per user saved for the server and an Add User tile at the end. A user without a stored session, or set to ask for the password, shows a lock and leads to the sign-in page
Kirigami.ScrollablePage {
    id: page
    objectName: "userPickerPage"

    required property ConnectionController connection

    // `ConnectionController::users` entries (`id`, `name`, `imageUrl`, `remembered`, `askPassword`) carry server metadata, rendered as plain text; the last entry stands for Add User
    readonly property var entries: connection.users.concat([{ id: "", name: "", imageUrl: "", remembered: true, askPassword: false, add: true }])

    title: i18nc("@title", "Who is watching?")

    actions: [
        Kirigami.Action {
            text: i18nc("@action:button Goes back to the list of servers", "Switch Server")
            icon.name: "network-server"
            onTriggered: page.connection.cancel()
        }
    ]

    // The grid takes the keys as soon as the page shows
    function focusPrimary() {
        grid.forceActiveFocus(Qt.TabFocusReason);
    }

    // The menu of a tile (a right click, the Menu key or the button of the tile): it opens at a point of the tile
    function openUserMenu(tile, x, y) {
        if (tile.isAdd)
            return;
        userMenu.userId = tile.modelData.id;
        userMenu.userName = tile.modelData.name;
        askPasswordItem.checked = tile.modelData.askPassword;
        userMenu.popup(tile, x, y);
    }

    function indexOfUser(userId) {
        return page.entries.findIndex(entry => entry.id === userId);
    }

    Controls.Menu {
        id: userMenu
        objectName: "userMenu"

        // The user the menu was opened for: the list may be rebuilt while it is open
        property string userId
        property string userName

        Controls.MenuItem {
            id: askPasswordItem
            objectName: "userMenuAskPassword"
            text: i18nc("@action:inmenu", "Ask for Password")
            checkable: true
            onTriggered: {
                const userId = userMenu.userId;
                page.connection.setUserAskPassword(userId, checked);
                // The tiles are rebuilt by the change: the current one stays on the same user
                Qt.callLater(() => {
                    const index = page.indexOfUser(userId);
                    if (index >= 0)
                        grid.currentIndex = index;
                    page.focusPrimary();
                });
            }
        }
        Controls.MenuItem {
            objectName: "userMenuRemove"
            text: i18nc("@action:inmenu", "Remove User…")
            icon.name: "edit-delete-remove"
            onTriggered: {
                removeDialog.userId = userMenu.userId;
                removeDialog.userName = userMenu.userName;
                removeDialog.open();
            }
        }
    }

    Kirigami.PromptDialog {
        id: removeDialog
        objectName: "removeUserDialog"

        property string userId
        property string userName

        title: i18nc("@title:window", "Remove User")
        standardButtons: Kirigami.Dialog.Cancel
        customFooterActions: [
            Kirigami.Action {
                text: i18nc("@action:button", "Remove")
                icon.name: "edit-delete-remove"
                onTriggered: {
                    page.connection.removeUser(removeDialog.userId);
                    removeDialog.close();
                }
            }
        ]
        onClosed: page.focusPrimary()

        // The subtitle would render the user name as rich text: the name goes into a plain-text label
        Controls.Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
            text: i18nc("@info %1 is a user name", "Remove %1 and the saved queue from this system? The account on the server is not changed", removeDialog.userName)
        }
    }

    GridView {
        id: grid
        objectName: "userPickerGrid"

        readonly property real avatarSize: Kirigami.Units.gridUnit * 6
        readonly property real minCellWidth: Kirigami.Units.gridUnit * 10
        // As many columns as fit, down to one, and never more than there are tiles; the tiles share the width, so they sit in the middle without any margin that would have to follow the width
        readonly property int columns: Math.max(1, Math.min(count, Math.floor(width / minCellWidth)))

        model: page.entries
        cellWidth: Math.floor(width / columns)
        cellHeight: avatarSize + Kirigami.Units.gridUnit * 3.5
        topMargin: Kirigami.Units.gridUnit
        activeFocusOnTab: true
        keyNavigationEnabled: true
        currentIndex: 0
        Accessible.role: Accessible.List
        Accessible.name: page.title

        // The tiles are not tab stops: the grid is, and it chooses the current user
        Keys.onReturnPressed: currentItem?.choose()
        Keys.onEnterPressed: currentItem?.choose()
        Keys.onSpacePressed: currentItem?.choose()
        Keys.onMenuPressed: if (currentItem) page.openUserMenu(currentItem, 0, currentItem.height)
        Keys.onPressed: event => {
            if (event.modifiers === Qt.ShiftModifier && event.key === Qt.Key_F10 && currentItem) {
                page.openUserMenu(currentItem, 0, currentItem.height);
                event.accepted = true;
            }
        }

        delegate: Controls.ItemDelegate {
            id: tile

            required property int index
            required property var modelData

            readonly property bool isAdd: modelData.add === true
            // No stored session, or a password wanted every time
            readonly property bool locked: !isAdd && (!modelData.remembered || modelData.askPassword)
            readonly property string lockedText: i18nc("@info:tooltip", "Asks for the password")
            readonly property string addText: i18nc("@action:button", "Add User")

            function choose() {
                if (isAdd)
                    page.connection.addUser();
                else
                    page.connection.chooseUser(modelData.id);
            }

            objectName: isAdd ? "userTileAdd" : "userTile_" + modelData.id
            width: grid.cellWidth
            height: grid.cellHeight
            // The grid is the one tab stop, the arrow keys move inside it
            focusPolicy: Qt.NoFocus
            highlighted: GridView.isCurrentItem && grid.activeFocus
            Accessible.name: isAdd ? addText : modelData.name
            Accessible.description: locked ? lockedText : ""
            Controls.ToolTip.text: lockedText
            Controls.ToolTip.visible: hovered && locked && !menuButton.hovered
            Controls.ToolTip.delay: Kirigami.Units.toolTipDelay

            onPressed: {
                grid.currentIndex = tile.index;
                grid.forceActiveFocus(Qt.MouseFocusReason);
            }
            onClicked: choose()

            TapHandler {
                acceptedButtons: Qt.RightButton
                enabled: !tile.isAdd
                onTapped: eventPoint => {
                    grid.currentIndex = tile.index;
                    grid.forceActiveFocus(Qt.MouseFocusReason);
                    page.openUserMenu(tile, eventPoint.position.x, eventPoint.position.y);
                }
            }

            contentItem: ColumnLayout {
                spacing: Kirigami.Units.smallSpacing

                Item {
                    Layout.alignment: Qt.AlignHCenter
                    Layout.preferredWidth: grid.avatarSize
                    Layout.preferredHeight: grid.avatarSize

                    Components.Avatar {
                        objectName: "userAvatar"
                        anchors.fill: parent
                        visible: !tile.isAdd
                        // The tile names the user to a screen reader
                        Accessible.ignored: true
                        name: tile.modelData.name
                        // Only URLs built by JellyfinClient::userImageUrl belong here: a raw server string must never become a source
                        source: tile.modelData.imageUrl
                        // The image is decoded at the size it shows
                        sourceSize.width: width * Screen.devicePixelRatio
                        sourceSize.height: height * Screen.devicePixelRatio
                    }

                    Rectangle {
                        anchors.fill: parent
                        visible: tile.isAdd
                        radius: width / 2
                        color: Kirigami.Theme.alternateBackgroundColor
                        border.width: 1
                        border.color: Kirigami.Theme.disabledTextColor

                        Kirigami.Icon {
                            anchors.centerIn: parent
                            width: Kirigami.Units.iconSizes.large
                            height: width
                            source: "list-add-user"
                        }
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

                    // User names come from the server: plain text only
                    Controls.Label {
                        objectName: "userName"
                        Layout.fillWidth: true
                        horizontalAlignment: Text.AlignHCenter
                        elide: Text.ElideRight
                        maximumLineCount: 1
                        textFormat: Text.PlainText
                        text: tile.isAdd ? tile.addText : tile.modelData.name
                    }

                    Controls.ToolButton {
                        id: menuButton
                        objectName: "userMenuButton"
                        Layout.preferredWidth: Kirigami.Units.iconSizes.smallMedium
                        Layout.preferredHeight: Kirigami.Units.iconSizes.smallMedium
                        focusPolicy: Qt.NoFocus
                        padding: 0
                        visible: !tile.isAdd
                        display: Controls.AbstractButton.IconOnly
                        icon.name: "overflow-menu"
                        icon.width: Kirigami.Units.iconSizes.small
                        icon.height: Kirigami.Units.iconSizes.small
                        text: i18nc("@action:button", "More Actions")
                        // The tooltip keeps the short text: a screen reader hears which user the button belongs to
                        Accessible.name: i18nc("@label %1 is a user name", "More Actions for %1", tile.modelData.name)
                        Controls.ToolTip.text: text
                        Controls.ToolTip.visible: hovered
                        Controls.ToolTip.delay: Kirigami.Units.toolTipDelay
                        onClicked: {
                            grid.currentIndex = tile.index;
                            grid.forceActiveFocus(Qt.MouseFocusReason);
                            const point = menuButton.mapToItem(tile, 0, menuButton.height);
                            page.openUserMenu(tile, point.x, point.y);
                        }
                    }
                }
            }
        }
    }
}
