// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// The queue as a dialog over the page in front, as Remote Sessions: the rows with their keys and menu (Play From Here is the menu's, on the row it was opened for), and its actions in the footer. Playing, opening an item and leaving the session close it first. Clear Queue asks nothing: Undo brings the queue back
Kirigami.Dialog {
    id: dialog
    objectName: "queueDialogContent"

    required property ConnectionController connection
    readonly property PlayQueue queue: connection.queue

    title: i18n("Queue")
    preferredWidth: Kirigami.Units.gridUnit * 40
    maximumHeight: absoluteMaximumHeight - Kirigami.Units.gridUnit * 4
    padding: 0
    customFooterActions: [
        Kirigami.Action {
            objectName: "resumeQueueAction"
            text: i18n("Resume Queue")
            icon.name: "media-playlist-play"
            enabled: dialog.queue.count > 0 && !dialog.queue.busy && !!list.Window.window?.player
            onTriggered: dialog.closeThen(window => window.resumeQueue())
        },
        Kirigami.Action {
            objectName: "undoQueueAction"
            text: i18n("Undo")
            icon.name: "edit-undo"
            enabled: dialog.queue.canUndo && !dialog.queue.busy
            onTriggered: dialog.undoLastEdit()
        },
        Kirigami.Action {
            objectName: "clearQueueAction"
            text: i18n("Clear Queue")
            icon.name: "edit-clear-all"
            enabled: dialog.queue.count > 0 && !dialog.queue.busy
            onTriggered: {
                dialog.queue.clear();
                list.forceActiveFocus(Qt.TabFocusReason);
            }
        }
    ]

    header: DialogTitleBar {
        title: dialog.title
        onCloseRequested: dialog.close()
    }

    // The list takes the keys as soon as the dialog shows
    function focusPrimary() {
        list.forceActiveFocus(Qt.TabFocusReason);
    }

    // What the dialog leads to runs once it has gone, with the window: a page or the player opened under the fading dialog would lose the keyboard to it
    property var afterClose: null
    function closeThen(action) {
        const window = list.Window.window;
        afterClose = () => action(window);
        close();
    }

    // The menu acts on the row it was opened for, as long as that row still holds the same item
    function menuRowIsCurrent() {
        return menu.row >= 0 && list.itemAtIndex(menu.row)?.itemId === menu.itemId;
    }

    // The row follows the item it moved
    function moveRow(from, delta) {
        const to = from + delta;
        if (from < 0 || to < 0 || to >= queue.count)
            return;
        if (queue.move(from, to))
            list.currentIndex = to;
    }

    // The current row stays at the same place, so the next item takes its spot
    function removeRow(row) {
        if (row < 0 || row >= queue.count)
            return;
        queue.remove(row);
        list.currentIndex = queue.count > 0 ? Math.min(row, queue.count - 1) : -1;
    }

    function undoLastEdit() {
        const row = list.currentIndex;
        if (!queue.undo())
            return;
        list.currentIndex = queue.count > 0 ? Math.max(0, Math.min(row, queue.count - 1)) : -1;
        list.forceActiveFocus(Qt.TabFocusReason);
    }

    function openRow(itemId) {
        dialog.closeThen(window => window.openItem(itemId, null));
    }

    function openMenu(row, anchorItem, x, y) {
        menu.row = row;
        menu.itemId = anchorItem.itemId;
        menu.popup(anchorItem, x, y);
    }

    // Each opening starts at the top with the first row current, as the page did
    onAboutToShow: {
        list.currentIndex = queue.count > 0 ? 0 : -1;
        list.positionViewAtBeginning();
    }
    onOpened: focusPrimary()
    onAboutToHide: menu.close()
    onClosed: {
        const next = afterClose;
        afterClose = null;
        if (next)
            Qt.callLater(next);
    }

    Shortcut {
        sequences: [StandardKey.Undo]
        enabled: dialog.opened && dialog.queue.canUndo && !dialog.queue.busy
        onActivated: dialog.undoLastEdit()
    }

    // A session that ended must not leave the dialog over the server the user left
    Connections {
        target: dialog.connection
        function onStateChanged() {
            if (dialog.connection.state !== ConnectionController.Authenticated)
                dialog.close();
        }
    }

    // A playback that starts from elsewhere (another client, SyncPlay) brings the player to the front: the dialog does not stay over it
    Connections {
        target: list.Window.window
        ignoreUnknownSignals: true
        function onPlaybackPageShowingChanged() {
            if (list.Window.window.playbackPageShowing)
                dialog.close();
        }
    }

    // Rows that move or go while a menu is open make it stale: it closes
    Connections {
        target: dialog.queue
        function onEdited() {
            menu.close();
        }
    }

    // The menu acts on the row it was opened for, and never shows server text
    Controls.Menu {
        id: menu
        objectName: "queueMenu"

        property int row: -1
        property string itemId: ""

        Controls.MenuItem {
            text: i18n("Play From Here")
            icon.name: "media-playback-start"
            enabled: !!list.Window.window?.player
            onTriggered: {
                if (!dialog.menuRowIsCurrent())
                    return;
                // The row is looked up once the dialog has gone: an edit from elsewhere may have moved it meanwhile
                const itemId = menu.itemId;
                dialog.closeThen(window => {
                    const row = window.connection.queue.indexOf(itemId);
                    if (row >= 0)
                        window.playQueue(row);
                });
            }
        }
        Controls.MenuItem {
            text: i18n("Move Up")
            icon.name: "arrow-up"
            enabled: menu.row > 0
            onTriggered: if (dialog.menuRowIsCurrent()) dialog.moveRow(menu.row, -1)
        }
        Controls.MenuItem {
            text: i18n("Move Down")
            icon.name: "arrow-down"
            enabled: menu.row >= 0 && menu.row < dialog.queue.count - 1
            onTriggered: if (dialog.menuRowIsCurrent()) dialog.moveRow(menu.row, 1)
        }
        Controls.MenuItem {
            text: i18n("Remove")
            icon.name: "edit-delete-remove"
            onTriggered: if (dialog.menuRowIsCurrent()) dialog.removeRow(menu.row)
        }
        Controls.MenuSeparator {}
        Controls.MenuItem {
            text: i18n("Go to Item")
            icon.name: "go-next"
            onTriggered: dialog.openRow(menu.itemId)
        }
    }

    ListView {
        id: list
        objectName: "queueList"
        // One height whatever the queue holds, so the dialog does not move under the pointer as rows go and come; the rows scroll in it
        implicitWidth: Kirigami.Units.gridUnit * 40
        implicitHeight: Kirigami.Units.gridUnit * 28
        topMargin: Kirigami.Units.smallSpacing
        bottomMargin: Kirigami.Units.smallSpacing
        clip: true

        // The rows and their images exist only while the dialog shows
        model: dialog.visible ? dialog.queue : null
        activeFocusOnTab: true
        keyNavigationEnabled: true
        currentIndex: 0
        Accessible.role: Accessible.List
        Accessible.name: dialog.title

        // The rows are not tab stops: the list is, and it opens the current item
        function openCurrent() {
            if (list.currentItem)
                dialog.openRow(list.currentItem.itemId);
        }
        function openCurrentMenu() {
            const item = list.currentItem;
            if (item)
                dialog.openMenu(list.currentIndex, item, item.sideMargin, item.height);
        }
        Keys.onReturnPressed: openCurrent()
        Keys.onEnterPressed: openCurrent()
        Keys.onSpacePressed: openCurrent()
        Keys.onMenuPressed: openCurrentMenu()
        Keys.onDeletePressed: dialog.removeRow(list.currentIndex)
        Keys.onPressed: event => {
            if (event.modifiers === Qt.ControlModifier && event.key === Qt.Key_Up) {
                dialog.moveRow(list.currentIndex, -1);
                event.accepted = true;
            } else if (event.modifiers === Qt.ControlModifier && event.key === Qt.Key_Down) {
                dialog.moveRow(list.currentIndex, 1);
                event.accepted = true;
            } else if (event.modifiers === Qt.ShiftModifier && event.key === Qt.Key_F10) {
                openCurrentMenu();
                event.accepted = true;
            }
        }

        // While an expansion runs on top of rows, a strip above them shows it: the rows stay usable
        header: Item {
            width: list.width
            height: visible ? Kirigami.Units.gridUnit * 3 : 0
            visible: dialog.queue.busy && list.count > 0

            RowLayout {
                anchors.centerIn: parent
                spacing: Kirigami.Units.largeSpacing

                Controls.BusyIndicator {
                    FadeInOnShow {}
                    running: parent.visible
                    Accessible.ignored: true
                }

                Controls.Label {
                    textFormat: Text.PlainText
                    text: i18n("Loading…")
                }
            }
        }

        Kirigami.LoadingPlaceholder {
            FadeInOnShow {}
            anchors.centerIn: parent
            visible: dialog.queue.busy && list.count === 0
            text: i18n("Loading…")
        }

        MessageBox {
            x: Kirigami.Units.largeSpacing
            y: Kirigami.Units.largeSpacing
            width: list.width - Kirigami.Units.largeSpacing * 2
            visible: dialog.queue.restoreFailed
            type: Kirigami.MessageType.Error
            text: i18n("Could not restore the queue: check the connection to the server and try again")
            actions: [
                Kirigami.Action {
                    text: i18n("Retry")
                    icon.name: "view-refresh"
                    onTriggered: dialog.queue.retryRestore()
                }
            ]
        }

        Kirigami.PlaceholderMessage {
            FadeInOnShow {}
            anchors.centerIn: parent
            width: parent.width - Kirigami.Units.gridUnit * 4
            visible: dialog.queue.count === 0 && !dialog.queue.busy && !dialog.queue.restoreFailed
            icon.name: "media-track-queue-amarok"
            text: i18n("The queue is empty")
            explanation: i18n("Add a movie, an episode, a season, or a series with Add to Queue")
        }

        delegate: MediaListRow {
            id: queueRow
            objectName: "queueRow_" + index
            onOpened: dialog.openRow(itemId)
            onMenuRequested: (x, y) => dialog.openMenu(index, queueRow, x, y)
        }
    }
}
