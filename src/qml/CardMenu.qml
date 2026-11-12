// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls

// The menu of a card, shared by every page: it acts on the item it was opened for and shows no server text. Playing is left out on purpose, the details page does that
Controls.Menu {
    id: menu
    objectName: "cardMenu"

    required property ConnectionController connection

    property string itemId: ""
    property string itemType: ""
    // The series of an episode or season; empty for anything else
    property string seriesId: ""

    readonly property bool playable: ["Movie", "Episode", "Video"].indexOf(itemType) >= 0

    // The kinds the play queue takes, as on the details page: keep the two lists in step
    readonly property bool queueable: ["Movie", "Episode", "Video", "Season", "Series", "BoxSet"].indexOf(itemType) >= 0

    // The page that opened the item gives way, as for a click on the card
    signal openRequested(string targetId)
    // A popup is not an Item, so the window that plays is passed in by the owner
    property bool playAvailable: false
    signal playRequested(string targetId)

    Controls.MenuItem {
        objectName: "cardMenuOpen"
        text: i18n("Open")
        icon.name: "document-open"
        onTriggered: menu.openRequested(menu.itemId)
    }
    Controls.MenuItem {
        objectName: "cardMenuPlayNow"
        text: i18n("Play Now")
        icon.name: "media-playback-start"
        visible: menu.playable
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
        objectName: "cardMenuGoToSeries"
        text: i18n("Go to Series")
        icon.name: "go-next"
        visible: menu.seriesId.length > 0
        height: visible ? implicitHeight : 0
        onTriggered: menu.openRequested(menu.seriesId)
    }
}
