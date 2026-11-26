// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// Who is watching: one tile per user saved for the server and an Add User button under them. A user without a stored session, or set to ask for the password, shows a lock and leads to the sign-in page
Kirigami.ScrollablePage {
    id: page
    objectName: "userPickerPage"

    // The darker View background on every page: a page without a list (a plain flickable) would get the lighter Window one, and the pages would not match
    Kirigami.Theme.colorSet: Kirigami.Theme.View
    Kirigami.Theme.inherit: false
    // The lone menu button stands in the top corner over the page: the content starts below it
    topPadding: Kirigami.Units.gridUnit * 2

    required property ConnectionController connection

    // The row toolbar is ToolBar everywhere (Main.qml): the pre-signin screens keep their identity in the top bar instead
    globalToolBarStyle: Kirigami.ApplicationHeaderStyle.None

    title: i18nc("@title", "Who is watching?")

    // The tiles take the keys as soon as the page shows
    function focusPrimary() {
        (grid.count > 0 ? grid : addUser).forceActiveFocus(Qt.TabFocusReason);
    }

    // The menu of a tile (a right click, the Menu key or the button of the tile): it opens at a point of the tile, or from the button when one is given. `ConnectionController::users` entries (`id`, `name`, `imageUrl`, `remembered`, `askPassword`) carry server metadata, rendered as plain text
    function openUserMenu(tile, x, y, button) {
        const user = page.connection.users[tile.index];
        userMenu.userId = user.id;
        userMenu.userName = user.name;
        askPasswordItem.checked = user.askPassword;
        if (button)
            userMenuPlacement.openFor(button);
        else
            userMenu.popup(tile, x, y);
    }

    function indexOfUser(userId) {
        return page.connection.users.findIndex(entry => entry.id === userId);
    }

    // The button's menu goes below the button, above it when there is no room below; the pointer's and the keyboard's open at their point
    PopupPlacement { id: userMenuPlacement; popup: userMenu }
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
                        grid.moveTo(index);
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

    ColumnLayout {
        spacing: Kirigami.Units.smallSpacing

        // The heading and the address are one block
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 0

            Kirigami.Heading {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                level: 1
                text: page.title
            }

            // The server address is server text: plain
            Controls.Label {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                textFormat: Text.PlainText
                color: Kirigami.Theme.disabledTextColor
                text: page.connection.serverUrl
            }
        }

        // The tiles are one tab stop with the arrow keys inside; the row is as wide as its columns, centred, and wraps when the page is narrower
        Item {
            id: grid
            objectName: "userPickerGrid"

            readonly property real gap: Kirigami.Units.largeSpacing
            readonly property real cellWidth: Kirigami.Units.gridUnit * 9 + gap
            readonly property int count: tiles.count
            readonly property bool rtl: Qt.application.layoutDirection === Qt.RightToLeft
            // As many columns as fit, down to one, and never more than there are tiles
            readonly property int columns: Math.max(1, Math.min(count, Math.floor((width + gap) / cellWidth)))
            property int currentIndex: 0
            property Item currentItem: null

            function refreshCurrent() {
                currentItem = tiles.itemAt(currentIndex);
            }

            function moveTo(index) {
                currentIndex = index;
                refreshCurrent();
                if (currentItem)
                    Spatial.reveal(currentItem);
            }

            Layout.fillWidth: true
            Layout.preferredHeight: flow.implicitHeight
            // Without users the page has Add User only, and there is no tab stop
            visible: count > 0
            activeFocusOnTab: true
            Accessible.role: Accessible.List
            Accessible.name: page.title
            onCurrentIndexChanged: refreshCurrent()

            // The keys act on the current tile
            Keys.onReturnPressed: currentItem?.clicked()
            Keys.onEnterPressed: currentItem?.clicked()
            Keys.onSpacePressed: currentItem?.clicked()
            Keys.onMenuPressed: if (currentItem) page.openUserMenu(currentItem, 0, currentItem.height)
            Keys.onPressed: event => {
                const last = count - 1;
                const row = Math.floor(currentIndex / columns);
                const lastRow = Math.floor(last / columns);
                let handled = last >= 0;
                switch (event.key) {
                case Qt.Key_Left:
                    moveTo(rtl ? Math.min(last, currentIndex + 1) : Math.max(0, currentIndex - 1));
                    break;
                case Qt.Key_Right:
                    moveTo(rtl ? Math.max(0, currentIndex - 1) : Math.min(last, currentIndex + 1));
                    break;
                case Qt.Key_Home:
                    moveTo(0);
                    break;
                case Qt.Key_End:
                    moveTo(last);
                    break;
                case Qt.Key_Up:
                    if (row > 0)
                        moveTo(currentIndex - columns);
                    else
                        handled = Spatial.move(grid, Qt.Key_Up);
                    break;
                case Qt.Key_Down:
                    // The last row leads to Add User
                    if (row < lastRow)
                        moveTo(Math.min(last, currentIndex + columns));
                    else
                        addUser.forceActiveFocus(Qt.TabFocusReason);
                    break;
                case Qt.Key_F10:
                    handled = event.modifiers === Qt.ShiftModifier && currentItem !== null;
                    if (handled)
                        page.openUserMenu(currentItem, 0, currentItem.height);
                    break;
                default:
                    handled = false;
                }
                event.accepted = handled;
            }

            Flow {
                id: flow
                anchors.horizontalCenter: parent.horizontalCenter
                width: grid.columns * grid.cellWidth - grid.gap
                spacing: grid.gap

                Repeater {
                    id: tiles
                    model: page.connection.users
                    onItemAdded: Qt.callLater(grid.refreshCurrent)
                    onItemRemoved: Qt.callLater(grid.refreshCurrent)

                    delegate: UserTile {
                        id: tile

                        required property int index
                        required property var modelData

                        objectName: "userTile_" + modelData.id
                        name: modelData.name
                        imageUrl: modelData.imageUrl
                        // No stored session, or a password wanted every time
                        locked: !modelData.remembered || modelData.askPassword
                        showMenu: true
                        highlighted: grid.currentIndex === index && grid.activeFocus

                        onPressed: {
                            grid.currentIndex = tile.index;
                            grid.forceActiveFocus(Qt.MouseFocusReason);
                        }
                        onClicked: page.connection.chooseUser(modelData.id)
                        onMenuRequested: (x, y, button) => {
                            grid.currentIndex = tile.index;
                            grid.forceActiveFocus(Qt.MouseFocusReason);
                            page.openUserMenu(tile, x, y, button);
                        }
                    }
                }
            }
        }

        Controls.Button {
            id: addUser
            objectName: "userAddButton"
            Layout.alignment: Qt.AlignHCenter
            Layout.topMargin: Kirigami.Units.largeSpacing
            text: i18nc("@action:button", "Add User")
            icon.name: "list-add-user"
            onClicked: page.connection.addUser()
            Keys.onReturnPressed: clicked()
            Keys.onEnterPressed: clicked()
            Keys.onUpPressed: event => {
                event.accepted = grid.count > 0;
                if (event.accepted)
                    grid.forceActiveFocus(Qt.BacktabFocusReason);
            }
        }
    }
}
