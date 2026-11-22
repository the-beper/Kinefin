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
    // The watched state of the item the menu was opened for, and its saved position: the Undo of Mark as Watched puts it back
    property bool played: false
    // A rewatch stopped halfway is in progress, not watched: the server keeps Played on it, and Mark as Watched is what finishes it
    readonly property bool watched: played && positionTicks <= 0
    property real positionTicks: 0
    // The kinds the details page can mark as favorite
    readonly property bool favoritable: ["Movie", "Series", "Season", "Episode", "BoxSet", "Video"].indexOf(itemType) >= 0

    // What the download engine says of this item for the signed-in user: empty when there is no record
    readonly property bool downloadable: connection.downloads.canDownload && playable
    readonly property int downloadsRevision: connection.downloads.revision
    readonly property string downloadState: {
        downloadsRevision; // the binding follows every queue change
        return downloadable ? connection.downloads.downloadState(itemId) : "";
    }
    readonly property bool downloadCancellable: ["queued", "fetching", "waiting"].indexOf(downloadState) >= 0
    readonly property bool downloadRemovable: ["paused", "error", "completed", "exported"].indexOf(downloadState) >= 0

    // The page that opened the item gives way, as for a click on the card
    signal openRequested(string targetId)
    // A popup is not an Item, so the window that plays is passed in by the owner
    property bool playAvailable: false
    // False while following a SyncPlay group: the group decides what plays, so no device can take an item of its own
    property bool playOnAvailable: true
    signal playRequested(string targetId)
    // Cast…: the target picker, then the same item at its resume point
    signal playOnRequested(string targetId, real ticks)
    signal resumeRequested(string targetId, real ticks)
    signal favoriteRequested(string targetId, bool favorite)
    signal playedRequested(string targetId, bool played)
    signal downloadRequested(string targetId)
    signal cancelDownloadRequested(string targetId)
    signal removeDownloadRequested(string targetId)
    signal addToPlaylistRequested(string targetId)
    // Play Next (`next`) or Add to Queue: the shell decides which queue, the user's or a SyncPlay group's
    signal queueRequested(string targetId, bool next)
    // Whether Play Next and Add to Queue can be taken now: the user's queue is not busy, or a SyncPlay group takes them
    property bool queueAvailable: !connection.queue.busy
    signal renamePlaylistRequested(string targetId)
    signal deletePlaylistRequested(string targetId)

    // From the More Actions button of a card: below the button, above it when there is no room below, never over it (the menu at the pointer or from the keyboard opens with `popup` at its point)
    function openFor(button: Item): void {
        placement.openFor(button);
    }
    PopupPlacement { id: placement; popup: menu }

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
        objectName: "cardMenuPlayOn"
        text: i18nc("@action:inmenu", "Cast…")
        icon.name: "kinefin-cast"
        visible: menu.playable
        height: visible ? implicitHeight : 0
        enabled: menu.playAvailable && menu.playOnAvailable
        Controls.ToolTip.visible: hovered && !menu.playOnAvailable
        Controls.ToolTip.text: i18nc("@info:tooltip", "Not available while following a SyncPlay group")
        onTriggered: menu.playOnRequested(menu.itemId, menu.resumeTicks > 0 ? menu.resumeTicks : 0)
    }
    Controls.MenuItem {
        objectName: "cardMenuPlayNext"
        text: i18n("Play Next")
        icon.name: "media-skip-forward"
        visible: menu.queueable
        height: visible ? implicitHeight : 0
        enabled: menu.queueAvailable
        onTriggered: menu.queueRequested(menu.itemId, true)
    }
    Controls.MenuItem {
        objectName: "cardMenuAddToQueue"
        text: i18n("Add to Queue")
        icon.name: "list-add"
        visible: menu.queueable
        height: visible ? implicitHeight : 0
        enabled: menu.queueAvailable
        onTriggered: menu.queueRequested(menu.itemId, false)
    }
    Controls.MenuItem {
        objectName: "cardMenuAddToPlaylist"
        text: i18nc("@action:inmenu", "Add to Playlist…")
        icon.name: "media-playlist-append"
        visible: menu.playable
        height: visible ? implicitHeight : 0
        onTriggered: menu.addToPlaylistRequested(menu.itemId)
    }
    Controls.MenuItem {
        objectName: "cardMenuDownload"
        text: i18n("Download")
        icon.name: "download-symbolic"
        visible: menu.downloadable && menu.downloadState.length === 0
        height: visible ? implicitHeight : 0
        onTriggered: menu.downloadRequested(menu.itemId)
    }
    Controls.MenuItem {
        objectName: "cardMenuCancelDownload"
        text: i18n("Cancel Download")
        icon.name: "process-stop"
        visible: menu.downloadCancellable
        height: visible ? implicitHeight : 0
        onTriggered: menu.cancelDownloadRequested(menu.itemId)
    }
    Controls.MenuItem {
        objectName: "cardMenuRemoveDownload"
        text: i18n("Delete Download…")
        icon.name: "edit-delete"
        visible: menu.downloadRemovable
        height: visible ? implicitHeight : 0
        onTriggered: menu.removeDownloadRequested(menu.itemId)
    }
    Controls.MenuSeparator {
        visible: menu.favoritable || menu.downloadable || menu.playable
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
    // A video only, as on its details page: a series or season has the state of its episodes
    Controls.MenuItem {
        objectName: "cardMenuPlayed"
        text: menu.watched ? i18n("Mark as Unwatched") : i18n("Mark as Watched")
        icon.name: menu.watched ? "view-hidden" : "checkmark-symbolic"
        visible: menu.playable
        height: visible ? implicitHeight : 0
        onTriggered: menu.playedRequested(menu.itemId, !menu.watched)
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
