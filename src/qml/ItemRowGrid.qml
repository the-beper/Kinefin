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
    readonly property int columns: Math.max(1, Math.floor(width / tileWidth))
    readonly property real aspect: landscape ? 9 / 16 : 3 / 2

    cellWidth: Math.floor(width / columns)
    cellHeight: Math.round((cellWidth - Kirigami.Units.smallSpacing * 2) * aspect) + Kirigami.Units.gridUnit * 3.5
    clip: true
    boundsBehavior: Flickable.StopAtBounds
    activeFocusOnTab: true
    keyNavigationEnabled: true
    currentIndex: 0
    Accessible.role: Accessible.List
    Accessible.name: grid.accessibleName

    Controls.ScrollBar.vertical: Controls.ScrollBar {}

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

        width: grid.cellWidth
        height: grid.cellHeight
        // The grid is the one tab stop, the arrow keys move inside it
        focusPolicy: Qt.NoFocus
        landscape: grid.landscape
        hasMenu: grid.hasMenu
        loaded: true
        highlighted: GridView.isCurrentItem && grid.activeFocus
        onClicked: grid.activated(itemId)
        subtitle: {
            if (episodeNumber >= 0) {
                const label = seasonNumber >= 0 ? i18nc("season and episode number", "S%1:E%2", seasonNumber, episodeNumber) : i18nc("episode number", "E%1", episodeNumber);
                return episodeTitle.length > 0 ? i18nc("episode label and episode title", "%1 — %2", label, episodeTitle) : label;
            }
            return year > 0 ? String(year) : "";
        }
    }
}
