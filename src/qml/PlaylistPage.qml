// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// One playlist in its order, read again every time the page comes to the front. Play and Shuffle make the playlist the saved queue (Undo on the Queue page brings the old one back) and start it
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
    // A queue that could not be read back stays untouched until it is (the Queue page offers Retry)
    readonly property bool canPlay: entries.count > 0 && !queue.busy && !queue.restoreFailed && !!page.Window.window?.player && !shell.playlistWaiting

    // Server text: the toolbar title delegate renders it as plain text. Read again with the list of playlists
    title: name || (connection.playlists.status, connection.playlists.nameOf(playlistId)) || i18n("Playlist")

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
        }
    ]

    header: ColumnLayout {
        spacing: 0

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
            visible: page.failed && page.entries.count > 0
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
            objectName: "playlistCapped"
            Layout.fillWidth: true
            Layout.margins: Kirigami.Units.largeSpacing
            visible: page.entries.capped
            text: i18np("Showing the first %1 entry of %2", "Showing the first %1 entries of %2", page.entries.count, page.entries.total)
        }
    }

    // The menu acts on the row it was opened for, and never shows server text
    Controls.Menu {
        id: menu
        objectName: "playlistMenu"

        property string itemId: ""

        Controls.MenuItem {
            text: i18n("Play From Here")
            icon.name: "media-playback-start"
            enabled: page.canPlay
            onTriggered: page.play(menu.itemId, false)
        }
        Controls.MenuItem {
            text: i18n("Go to Item")
            icon.name: "go-next"
            onTriggered: page.Window.window.openItem(menu.itemId, page)
        }
    }

    function openMenu(row, x, y) {
        menu.itemId = row.itemId;
        menu.popup(row, x, y);
    }

    ListView {
        id: list
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
        Keys.onMenuPressed: if (list.currentItem) page.openMenu(list.currentItem, 0, list.currentItem.height)
        Keys.onPressed: event => {
            if (event.modifiers === Qt.ShiftModifier && event.key === Qt.Key_F10 && list.currentItem) {
                page.openMenu(list.currentItem, 0, list.currentItem.height);
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
            anchors.centerIn: parent
            running: visible
            visible: page.loading && list.count === 0
        }

        Kirigami.PlaceholderMessage {
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
            objectName: "playlistEmpty"
            anchors.centerIn: parent
            width: parent.width - Kirigami.Units.gridUnit * 4
            visible: page.entries.status === PlaylistEntriesModel.Ready && page.entries.count === 0
            icon.name: "view-media-playlist"
            text: i18n("This playlist is empty")
        }
    }
}
