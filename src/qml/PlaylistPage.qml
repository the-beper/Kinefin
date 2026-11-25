// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// One playlist in its order, read again every time the page comes to the front. Play and Shuffle make the playlist the saved queue (Undo on the Queue page brings the old one back) and start it; the rows move at once (a removal after its confirmation) and their writes follow (see PlaylistEntriesModel)
Kirigami.ScrollablePage {
    id: page
    objectName: "playlistPage"

    required property ConnectionController connection
    // The window shell counts the pages: the entries stop reading with the last
    required property var shell
    required property string playlistId
    // The name the caller knew, else the list of playlists names it
    property string name: ""

    readonly property PlaylistEntriesModel entries: connection.playlistEntries
    readonly property PlayQueue queue: connection.queue
    readonly property bool loading: entries.status === PlaylistEntriesModel.Loading
    readonly property bool failed: entries.status === PlaylistEntriesModel.Failed
    // A queue that could not be read back stays untouched until it is (the Queue page offers Retry); the queue reads the playlist from the server, so not while edits are on their way
    readonly property bool canPlay: entries.count > 0 && !entries.writing && !queue.busy && !queue.restoreFailed && !!page.Window.window?.player && !shell.playlistWaiting && !shell.remotePlayWaiting
    // One write of a playlist at a time: a rename or a delete waits for the edits of the rows, and the other way round
    readonly property bool canChangePlaylist: !entries.writing && !connection.playlists.writing
    // The rows stay as they are while a rename or delete is on its way, and while the queue reads the playlist (an edit then would give it half of each order)
    readonly property bool canEditRows: !connection.playlists.writing && !shell.playlistWaiting
    // One box at a time, the first that applies: a failed read (the rows are stale), a refused change, the cap
    readonly property string notice: failed && entries.count > 0 ? "failed" : entries.editError !== PlaylistEntriesModel.NoEditError ? "editFailed" : entries.capped ? "capped" : ""

    // Read again with the list of playlists
    readonly property string knownName: name || (connection.playlists.status, connection.playlists.nameOf(playlistId))
    // The page row shows it as plain text
    title: knownName || i18n("Playlist")
    // The row of the page carries the way back, the name and the actions, as on the Movies and Shows tabs
    globalToolBarStyle: Kirigami.ApplicationHeaderStyle.None
    // Without Kirigami's title row the page names itself to a screen reader
    Accessible.role: Accessible.Pane
    Accessible.name: title

    Component.onCompleted: {
        shell.playlistPages++;
        entries.load(playlistId);
    }
    Component.onDestruction: shell?.playlistPageGone?.()
    // Read again when the page comes back to the front, not as it is first shown (Component.onCompleted reads then); at once when the shared model holds another playlist
    property bool shownOnce: false
    onIsCurrentPageChanged: {
        if (isCurrentPage && shownOnce && (entries.playlistId !== playlistId || !loading))
            entries.load(playlistId);
        shownOnce = shownOnce || isCurrentPage;
    }

    // What the main menu's Refresh calls while this page is in front
    function refreshPage() {
        if (!loading)
            entries.load(playlistId);
    }

    function focusPrimary() {
        list.forceActiveFocus(Qt.TabFocusReason);
    }

    // The queue becomes the playlist, in a random order with `shuffled`, and plays from `itemId` (the first row when empty)
    function play(itemId, shuffled) {
        if (page.canPlay)
            page.shell.playPlaylist(page.playlistId, itemId, shuffled);
    }

    function isThisPlaylist(id) {
        return id.replace(/-/g, "").toLowerCase() === playlistId.replace(/-/g, "").toLowerCase();
    }

    // The row follows the entry it moved
    function moveRow(from, delta) {
        const to = from + delta;
        if (to < 0 || to >= entries.count || !page.canEditRows)
            return;
        if (entries.move(from, to))
            list.currentIndex = to;
    }

    // The current row stays at the same place, so the next entry takes its spot
    function removeRow(row) {
        if (!page.canEditRows || !entries.remove(row))
            return;
        list.currentIndex = entries.count > 0 ? Math.min(row, entries.count - 1) : -1;
    }

    // The menu and the Delete key come here: the entry goes only once the dialog is confirmed
    function askRemoveRow(row) {
        const item = list.itemAtIndex(row);
        if (!item || !page.canEditRows || !entries.canRemove(row))
            return;
        removeDialog.row = row;
        removeDialog.itemId = item.itemId;
        removeDialog.itemName = item.title;
        removeDialog.open();
    }

    Connections {
        target: page.connection.playlists
        function onRenamed(playlistId, name) {
            if (page.isThisPlaylist(playlistId))
                page.name = name;
        }
    }

    actions: [
        Kirigami.Action {
            objectName: "playPlaylistAction"
            text: i18nc("@action:intoolbar", "Play")
            icon.name: "media-playback-start"
            enabled: page.canPlay
            onTriggered: page.play("", false)
        },
        Kirigami.Action {
            objectName: "shufflePlaylistAction"
            text: i18nc("@action:intoolbar", "Shuffle")
            icon.name: "media-playlist-shuffle"
            enabled: page.canPlay
            onTriggered: page.play("", true)
        },
        Kirigami.Action {
            objectName: "renamePlaylistAction"
            text: i18nc("@action:intoolbar", "Rename…")
            icon.name: "edit-rename"
            displayHint: Kirigami.DisplayHint.AlwaysHide
            // Only while the page is in front: a page left under another keeps its actions
            shortcut: "F2"
            enabled: page.canChangePlaylist && page.isCurrentPage && !removeDialog.visible
            onTriggered: page.shell.renamePlaylist(page.playlistId, page.knownName)
        },
        Kirigami.Action {
            objectName: "deletePlaylistAction"
            text: i18nc("@action:intoolbar", "Delete…")
            icon.name: "edit-delete"
            displayHint: Kirigami.DisplayHint.AlwaysHide
            enabled: page.canChangePlaylist
            onTriggered: page.shell.askDeletePlaylist(page.playlistId)
        }
    ]

    // The entry is named in a plain-text label: the dialog's subtitle would read a title that starts with a tag as rich text
    Kirigami.PromptDialog {
        id: removeDialog
        objectName: "removePlaylistEntryDialog"

        property int row: -1
        property string itemId: ""
        property string itemName: ""

        title: i18n("Remove from Playlist")
        standardButtons: Kirigami.Dialog.Cancel
        customFooterActions: [
            Kirigami.Action {
                objectName: "removePlaylistEntryConfirm"
                text: i18nc("@action:button", "Remove")
                icon.name: "edit-delete-remove"
                enabled: page.canEditRows
                onTriggered: {
                    removeDialog.close();
                    // The row may have moved or gone while the dialog was open
                    if (list.itemAtIndex(removeDialog.row)?.itemId === removeDialog.itemId)
                        page.removeRow(removeDialog.row);
                }
            }
        ]
        onClosed: page.focusPrimary()

        Controls.Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
            text: removeDialog.itemName.length > 0 ? i18n("Remove %1 from this playlist? The video stays in the library", removeDialog.itemName) : i18n("Remove this video from the playlist? The video stays in the library")
        }
    }

    header: ColumnLayout {
        spacing: 0

        PageToolbar {
            Layout.fillWidth: true
            backShown: true
            title: page.title
            actions: page.actions
            toolbarObjectName: "playlistToolbar"
        }

        // The playlist is read for the queue
        Controls.ProgressBar {
            objectName: "playlistQueueing"
            Layout.fillWidth: true
            visible: page.shell.playlistWaiting
            indeterminate: true
        }

        MessageBox {
            objectName: "playlistFailed"
            Layout.fillWidth: true
            Layout.margins: Kirigami.Units.largeSpacing
            visible: page.notice === "failed"
            type: Kirigami.MessageType.Warning
            text: i18n("The playlist could not be read again")
            actions: [
                Kirigami.Action {
                    text: i18n("Retry")
                    icon.name: "view-refresh"
                    onTriggered: page.entries.load(page.playlistId)
                }
            ]
        }
        MessageBox {
            objectName: "playlistEditFailed"
            Layout.fillWidth: true
            Layout.margins: Kirigami.Units.largeSpacing
            visible: page.notice === "editFailed"
            type: Kirigami.MessageType.Warning
            text: page.entries.editError === PlaylistEntriesModel.EditForbidden ? i18n("Missing permission to change this playlist") : i18n("The change could not be saved and the playlist was read again from the server: try the change again")
        }
        MessageBox {
            objectName: "playlistCapped"
            Layout.fillWidth: true
            Layout.margins: Kirigami.Units.largeSpacing
            visible: page.notice === "capped"
            text: i18np("Showing the first %1 entry of %2", "Showing the first %1 entries of %2", page.entries.count, page.entries.total)
        }
    }

    // Rows that move or go while the menu is open make it stale: it closes
    Connections {
        target: page.entries
        function onEdited() {
            menu.close();
        }
        function onModelReset() {
            menu.close();
        }
    }

    // The menu acts on the row it was opened for, and never shows server text; what the row allows is read as it opens
    Controls.Menu {
        id: menu
        objectName: "playlistMenu"

        property int row: -1
        property string itemId: ""
        property bool movable: false
        property bool removable: false

        Controls.MenuItem {
            text: i18n("Play From Here")
            icon.name: "media-playback-start"
            enabled: page.canPlay
            onTriggered: page.play(menu.itemId, false)
        }
        Controls.MenuItem {
            objectName: "playlistMenuMoveUp"
            text: i18n("Move Up")
            icon.name: "arrow-up"
            enabled: menu.movable && menu.row > 0
            onTriggered: page.moveRow(menu.row, -1)
        }
        Controls.MenuItem {
            objectName: "playlistMenuMoveDown"
            text: i18n("Move Down")
            icon.name: "arrow-down"
            enabled: menu.movable && menu.row < page.entries.count - 1
            onTriggered: page.moveRow(menu.row, 1)
        }
        Controls.MenuItem {
            objectName: "playlistMenuRemove"
            text: i18n("Remove from Playlist…")
            icon.name: "edit-delete-remove"
            enabled: menu.removable
            onTriggered: page.askRemoveRow(menu.row)
        }
        Controls.MenuSeparator {}
        Controls.MenuItem {
            text: i18n("Go to Item")
            icon.name: "go-next"
            onTriggered: page.Window.window.openItem(menu.itemId, page)
        }
    }

    function openMenu(row, x, y) {
        menu.row = row.index;
        menu.itemId = row.itemId;
        menu.movable = page.canEditRows && entries.canMove(row.index);
        menu.removable = page.canEditRows && entries.canRemove(row.index);
        menu.popup(row, x, y);
    }

    ListView {
        id: list
        // What scrolls under the page row fades into it instead of being cut by its edge
        TopShadow {
            parent: list
            z: 1
            atStart: list.atYBeginning
        }
        objectName: "playlistEntries"

        model: page.entries
        activeFocusOnTab: true
        keyNavigationEnabled: true
        currentIndex: 0
        Accessible.role: Accessible.List
        Accessible.name: page.title

        Keys.onReturnPressed: if (list.currentItem) page.Window.window.openItem(list.currentItem.itemId, page)
        Keys.onEnterPressed: if (list.currentItem) page.Window.window.openItem(list.currentItem.itemId, page)
        Keys.onSpacePressed: if (list.currentItem) page.Window.window.openItem(list.currentItem.itemId, page)
        Keys.onMenuPressed: if (list.currentItem) page.openMenu(list.currentItem, list.currentItem.sideMargin, list.currentItem.height)
        Keys.onDeletePressed: page.askRemoveRow(list.currentIndex)
        Keys.onPressed: event => {
            if (event.modifiers === Qt.ControlModifier && event.key === Qt.Key_Up) {
                page.moveRow(list.currentIndex, -1);
                event.accepted = true;
            } else if (event.modifiers === Qt.ControlModifier && event.key === Qt.Key_Down) {
                page.moveRow(list.currentIndex, 1);
                event.accepted = true;
            } else if (event.modifiers === Qt.ShiftModifier && event.key === Qt.Key_F10 && list.currentItem) {
                page.openMenu(list.currentItem, list.currentItem.sideMargin, list.currentItem.height);
                event.accepted = true;
            }
        }

        delegate: MediaListRow {
            id: entryRow
            objectName: "playlistRow_" + index
            onOpened: page.Window.window.openItem(itemId, page)
            onMenuRequested: (x, y) => page.openMenu(entryRow, x, y)
        }

        Controls.BusyIndicator {
            FadeInOnShow {}
            anchors.centerIn: parent
            running: visible
            visible: page.loading && list.count === 0
        }

        Kirigami.PlaceholderMessage {
            FadeInOnShow {}
            objectName: "playlistNotLoaded"
            anchors.centerIn: parent
            width: parent.width - Kirigami.Units.gridUnit * 4
            visible: page.failed && page.entries.count === 0
            icon.name: "dialog-warning"
            text: i18n("The playlist could not be loaded")
            explanation: i18n("Its entries could not be read from the server: try again")
            helpfulAction: Kirigami.Action {
                text: i18n("Retry")
                icon.name: "view-refresh"
                onTriggered: page.entries.load(page.playlistId)
            }
        }

        Kirigami.PlaceholderMessage {
            FadeInOnShow {}
            objectName: "playlistEmpty"
            anchors.centerIn: parent
            width: parent.width - Kirigami.Units.gridUnit * 4
            visible: page.entries.status === PlaylistEntriesModel.Ready && page.entries.count === 0
            icon.name: "view-media-playlist"
            text: i18n("This playlist is empty")
        }
    }
}
