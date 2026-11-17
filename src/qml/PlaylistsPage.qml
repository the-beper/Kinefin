// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// The video playlists of the signed-in user as a grid of cards, read again every time the page comes to the front; a card opens its playlist
Kirigami.ScrollablePage {
    id: page
    objectName: "playlistsPage"

    required property ConnectionController connection
    // The window shell counts the pages: the list stops reading with the last
    required property var shell

    readonly property PlaylistsController playlists: connection.playlists
    readonly property bool loading: playlists.status === PlaylistsController.Loading
    readonly property bool failed: playlists.status === PlaylistsController.Failed

    title: i18n("Playlists")
    // The row of the page carries the actions, as on the Movies and Shows tabs
    globalToolBarStyle: Kirigami.ApplicationHeaderStyle.None
    // Without Kirigami's title row the page names itself to a screen reader
    Accessible.role: Accessible.Pane
    Accessible.name: title

    Component.onCompleted: {
        shell.playlistsPages++;
        playlists.reload();
    }
    Component.onDestruction: shell?.playlistsPageGone?.()
    // Read again when the page comes back to the front, not as it is first shown (Component.onCompleted reads then)
    property bool shownOnce: false
    onIsCurrentPageChanged: {
        if (isCurrentPage && shownOnce && !loading)
            playlists.reload();
        shownOnce = shownOnce || isCurrentPage;
    }

    // What the main menu's Refresh calls while this page is in front
    function refreshPage() {
        if (!loading)
            playlists.reload();
    }

    function focusPrimary() {
        grid.forceActiveFocus(Qt.TabFocusReason);
    }

    actions: [
        Kirigami.Action {
            objectName: "newPlaylistAction"
            text: i18nc("@action:intoolbar", "New Playlist…")
            icon.name: "list-add"
            enabled: !page.playlists.writing
            onTriggered: page.Window.window.newPlaylist()
        }
    ]

    header: ColumnLayout {
        spacing: 0

        PageToolbar {
            Layout.fillWidth: true
            actions: page.actions
            toolbarObjectName: "playlistsToolbar"
        }

        MessageBox {
            objectName: "playlistsFailed"
            Layout.fillWidth: true
            Layout.margins: Kirigami.Units.largeSpacing
            visible: page.failed && page.playlists.list.count > 0
            type: Kirigami.MessageType.Warning
            text: i18n("The playlists could not be read again")
            actions: [
                Kirigami.Action {
                    text: i18n("Retry")
                    icon.name: "view-refresh"
                    onTriggered: page.playlists.reload()
                }
            ]
        }
        MessageBox {
            objectName: "playlistsCapped"
            Layout.fillWidth: true
            Layout.margins: Kirigami.Units.largeSpacing
            visible: page.playlists.capped
            text: i18np("Showing the first %1 playlist of %2", "Showing the first %1 playlists of %2", page.playlists.list.count, page.playlists.total)
        }
    }

    GridView {
        id: grid
        // What scrolls under the page row fades into it instead of being cut by its edge
        TopFade {
            parent: grid
            z: 1
            atStart: grid.atYBeginning
        }
        objectName: "playlistsGrid"

        // Poster tiles of about ten grid units, at least one to a row
        readonly property int columns: Math.max(1, Math.floor(width / (Kirigami.Units.gridUnit * 10)))

        model: page.playlists.list
        cellWidth: Math.floor(width / columns)
        // A 2:3 poster inside the tile margins, then the name and the second line
        cellHeight: Math.round((cellWidth - Kirigami.Units.largeSpacing - Kirigami.Units.smallSpacing * 2) * 1.5) + Kirigami.Units.gridUnit * 3.5 + Kirigami.Units.largeSpacing
        activeFocusOnTab: true
        keyNavigationEnabled: true
        currentIndex: 0
        Accessible.role: Accessible.List
        Accessible.name: page.title

        // The tiles are not tab stops: the grid is, and it opens the current playlist
        function openCurrent() {
            if (grid.currentItem)
                page.Window.window.openPlaylist(grid.currentItem.itemId, page, grid.currentItem.name);
        }
        Keys.onReturnPressed: openCurrent()
        Keys.onEnterPressed: openCurrent()
        Keys.onSpacePressed: openCurrent()
        Keys.onMenuPressed: grid.currentItem?.openMenu(0, grid.currentItem.height)
        Keys.onPressed: event => {
            if (event.modifiers === Qt.ShiftModifier && event.key === Qt.Key_F10 && grid.currentItem) {
                grid.currentItem.openMenu(0, grid.currentItem.height);
                event.accepted = true;
            }
        }

        delegate: MediaCard {
            required property int seasonNumber
            required property int episodeNumber
            required property string episodeTitle

            width: grid.cellWidth - Kirigami.Units.largeSpacing
            height: grid.cellHeight - Kirigami.Units.largeSpacing
            // The grid is the one tab stop, the arrow keys move inside it
            focusPolicy: Qt.NoFocus
            loaded: true
            highlighted: GridView.isCurrentItem && grid.activeFocus
            onClicked: page.Window.window.openPlaylist(itemId, page, name)
        }

        Controls.BusyIndicator {
            FadeInOnShow {}
            anchors.centerIn: parent
            running: visible
            visible: page.loading && page.playlists.list.count === 0
        }

        Kirigami.PlaceholderMessage {
            FadeInOnShow {}
            objectName: "playlistsNotLoaded"
            anchors.centerIn: parent
            width: parent.width - Kirigami.Units.gridUnit * 4
            visible: page.failed && page.playlists.list.count === 0
            icon.name: "dialog-warning"
            text: i18n("Playlists could not be loaded")
            explanation: i18n("The playlists could not be read from the server: try again")
            helpfulAction: Kirigami.Action {
                text: i18n("Retry")
                icon.name: "view-refresh"
                onTriggered: page.playlists.reload()
            }
        }

        Kirigami.PlaceholderMessage {
            FadeInOnShow {}
            objectName: "playlistsEmpty"
            anchors.centerIn: parent
            width: parent.width - Kirigami.Units.gridUnit * 4
            visible: page.playlists.status === PlaylistsController.Ready && page.playlists.list.count === 0
            icon.name: "view-media-playlist"
            text: i18n("No playlists")
            // The row above already carries New Playlist…: the placeholder only says where
            explanation: i18n("Create one with New Playlist…, or with Add to Playlist… in the menu of a video")
        }
    }
}
