// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import org.kde.kirigami as Kirigami

// A flat list of cards from an ItemRowModel (the upcoming episodes, the networks, the TV suggestions) as a grid: the one tab stop of its page, arrows to move, Return and Space to open. `activated` carries the id of the card opened
GridView {
    id: grid

    required property string accessibleName
    // A 16:9 image area instead of the 2:3 poster
    property bool landscape: false
    // False for the items that have no card menu (a network)
    property bool hasMenu: true

    signal activated(string itemId)

    readonly property real tileWidth: Kirigami.Units.gridUnit * (landscape ? 14 : 10)
    readonly property int columns: Math.max(1, Math.floor(usableWidth / tileWidth))
    // The gap between two cards and the margin around them; the width the page margin and the vertical scroll bar take is not shared out to the cells, so no card sits under the bar
    readonly property real gutter: Kirigami.Units.largeSpacing
    readonly property real usableWidth: Math.max(0, width - leftMargin - vbar.implicitWidth)
    readonly property real aspect: landscape ? 9 / 16 : 3 / 2

    cellWidth: Math.floor(usableWidth / columns)
    cellHeight: Math.round((cellWidth - gutter - Kirigami.Units.smallSpacing * 2) * aspect) + Kirigami.Units.gridUnit * 3.5 + gutter
    leftMargin: gutter
    topMargin: gutter
    clip: true
    boundsBehavior: Flickable.StopAtBounds
    activeFocusOnTab: true
    keyNavigationEnabled: true
    currentIndex: 0
    Accessible.role: Accessible.List
    Accessible.name: grid.accessibleName

    Controls.ScrollBar.vertical: Controls.ScrollBar {
        id: vbar
    }

    // The tiles are not tab stops: the grid is, and it opens the current one
    function openCurrent() {
        if (grid.currentItem?.enabled)
            grid.activated(grid.currentItem.itemId);
    }
    Keys.onReturnPressed: openCurrent()
    Keys.onEnterPressed: openCurrent()
    Keys.onSpacePressed: openCurrent()
    Keys.onMenuPressed: if (grid.currentItem?.enabled) grid.currentItem.openMenu(0, grid.currentItem.height)
    Keys.onPressed: event => {
        if (event.modifiers === Qt.ShiftModifier && event.key === Qt.Key_F10 && grid.currentItem?.enabled) {
            grid.currentItem.openMenu(0, grid.currentItem.height);
            event.accepted = true;
        }
    }

    delegate: MediaCard {
        required property int seasonNumber
        required property int episodeNumber
        required property string episodeTitle

        width: grid.cellWidth - grid.gutter
        height: grid.cellHeight - grid.gutter
        // The grid is the one tab stop, the arrow keys move inside it
        focusPolicy: Qt.NoFocus
        landscape: grid.landscape
        hasMenu: grid.hasMenu
        loaded: true
        highlighted: GridView.isCurrentItem && grid.activeFocus
        onClicked: grid.activated(itemId)
        subtitle: {
            if (episodeNumber >= 0) {
                const label = EpisodeLabel.label(seasonNumber, episodeNumber);
                return episodeTitle.length > 0 ? i18nc("episode label and episode title", "%1 — %2", label, episodeTitle) : label;
            }
            return year > 0 ? String(year) : "";
        }
        spokenSubtitle: {
            if (episodeNumber >= 0) {
                const label = EpisodeLabel.spoken(seasonNumber, episodeNumber);
                return episodeTitle.length > 0 ? i18nc("episode label and episode title", "%1 — %2", label, episodeTitle) : label;
            }
            return year > 0 ? String(year) : "";
        }
    }
}
