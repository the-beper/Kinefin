// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls

// The menu of a card, shared by every page: it acts on the item it was opened for and shows no server text
Controls.Menu {
    id: menu
    objectName: "cardMenu"

    required property ConnectionController connection

    property string itemId: ""
    property string itemType: ""
    // The series of an episode or season; empty for anything else
    property string seriesId: ""

    // The saved position of a card with a quick play (0: none), -1 for a card without one: then the menu has no Resume or Play entry
    property real resumeTicks: -1
    readonly property bool quickPlay: resumeTicks >= 0 && playable

    readonly property bool playable: ["Movie", "Episode", "Video"].indexOf(itemType) >= 0

    // The kinds the play queue takes, as on the details page: keep the two lists in step
    readonly property bool queueable: ["Movie", "Episode", "Video", "Season", "Series", "BoxSet", "Playlist"].indexOf(itemType) >= 0

    // The favorite state of the item the menu was opened for
    property bool favorite: false
    // The kinds the details page can mark as favorite
    readonly property bool favoritable: ["Movie", "Series", "Season", "Episode", "BoxSet", "Video"].indexOf(itemType) >= 0

    // The page that opened the item gives way, as for a click on the card
    signal openRequested(string targetId)
    // A popup is not an Item, so the window that plays is passed in by the owner
    property bool playAvailable: false
    signal playRequested(string targetId)
    signal resumeRequested(string targetId, real ticks)
    signal favoriteRequested(string targetId, bool favorite)
    signal addToPlaylistRequested(string targetId)
    signal renamePlaylistRequested(string targetId)
    signal deletePlaylistRequested(string targetId)

    Controls.MenuItem {
        objectName: "cardMenuResume"
        text: menu.resumeTicks > 0 ? i18n("Resume") : i18n("Play")
        icon.name: "media-playback-start"
        visible: menu.quickPlay
        height: visible ? implicitHeight : 0
        enabled: menu.playAvailable
        onTriggered: menu.resumeRequested(menu.itemId, menu.resumeTicks)
    }

    Controls.MenuItem {
        objectName: "cardMenuOpen"
        text: i18n("Open")
        icon.name: "document-open"
        onTriggered: menu.openRequested(menu.itemId)
    }
    Controls.MenuItem {
        objectName: "cardMenuGoToSeries"
        text: i18n("Go to Series")
        icon.name: "go-next"
        visible: menu.seriesId.length > 0
        height: visible ? implicitHeight : 0
        onTriggered: menu.openRequested(menu.seriesId)
    }
    Controls.MenuItem {
        objectName: "cardMenuPlayNow"
        // Beside Resume it starts over, so it says so and takes another icon
        readonly property bool besideResume: menu.quickPlay && menu.resumeTicks > 0
        text: besideResume ? i18n("Play from Beginning") : i18n("Play Now")
        icon.name: besideResume ? "media-skip-backward" : "media-playback-start"
        visible: menu.playable && !(menu.quickPlay && menu.resumeTicks <= 0)
        height: visible ? implicitHeight : 0
        enabled: menu.playAvailable
        onTriggered: menu.playRequested(menu.itemId)
    }
    Controls.MenuItem {
        objectName: "cardMenuPlayNext"
        text: i18n("Play Next")
        icon.name: "media-skip-forward"
        visible: menu.queueable
        height: visible ? implicitHeight : 0
        enabled: !menu.connection.queue.busy
        onTriggered: menu.connection.queue.add(menu.itemId, PlayQueue.Next)
    }
    Controls.MenuItem {
        objectName: "cardMenuAddToQueue"
        text: i18n("Add to Queue")
        icon.name: "list-add"
        visible: menu.queueable
        height: visible ? implicitHeight : 0
        enabled: !menu.connection.queue.busy
        onTriggered: menu.connection.queue.add(menu.itemId, PlayQueue.Last)
    }
    Controls.MenuItem {
        objectName: "cardMenuAddToPlaylist"
        text: i18nc("@action:inmenu", "Add to Playlist…")
        icon.name: "media-playlist-append"
        visible: menu.playable
        height: visible ? implicitHeight : 0
        onTriggered: menu.addToPlaylistRequested(menu.itemId)
    }
    Controls.MenuSeparator {
        visible: menu.favoritable
        height: visible ? implicitHeight : 0
    }
    Controls.MenuItem {
        objectName: "cardMenuFavorite"
        text: menu.favorite ? i18n("Remove from Favorites") : i18n("Add to Favorites")
        icon.name: menu.favorite ? "starred-symbolic" : "non-starred-symbolic"
        visible: menu.favoritable
        height: visible ? implicitHeight : 0
        onTriggered: menu.favoriteRequested(menu.itemId, !menu.favorite)
    }
    Controls.MenuSeparator {
        visible: menu.itemType === "Playlist"
        height: visible ? implicitHeight : 0
    }
    Controls.MenuItem {
        objectName: "cardMenuRenamePlaylist"
        text: i18nc("@action:inmenu", "Rename…")
        icon.name: "edit-rename"
        visible: menu.itemType === "Playlist"
        height: visible ? implicitHeight : 0
        enabled: !menu.connection.playlists.writing
        onTriggered: menu.renamePlaylistRequested(menu.itemId)
    }
    Controls.MenuItem {
        objectName: "cardMenuDeletePlaylist"
        text: i18nc("@action:inmenu", "Delete…")
        icon.name: "edit-delete"
        visible: menu.itemType === "Playlist"
        height: visible ? implicitHeight : 0
        enabled: !menu.connection.playlists.writing
        onTriggered: menu.deletePlaylistRequested(menu.itemId)
    }
}
