// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// The loaded favorites of one group of the Favorites page as a grid, the page Show All opens above it. The group is a cut of the snapshot the page holds: when the snapshot is replaced the page goes
Kirigami.ScrollablePage {
    id: page
    objectName: "favoritesGroupPage"

    required property ConnectionController connection
    // The cards of the group (the `items` of a row of FavoritesGroupsModel), which lives as long as the snapshot
    required property var items
    // The title of the group, already translated or server text
    required property string heading

    // The group is shown as it was loaded: the notice says so when the list may be partial
    readonly property bool partial: connection.favorites.capped || connection.favorites.libraryFailed

    // Server text (a library or a genre name) in the title: the page row renders it as plain text
    title: heading
    // The row of the page carries the way back and the name, as on the Movies and Shows tabs
    globalToolBarStyle: Kirigami.ApplicationHeaderStyle.None
    // Without Kirigami's title row the page names itself to a screen reader
    Accessible.role: Accessible.Pane
    Accessible.name: title

    // The grid takes the keys as soon as the page shows
    function focusPrimary() {
        grid.forceActiveFocus(Qt.TabFocusReason);
    }

    // The page and the ones above it leave with the snapshot they were cut from
    function leave() {
        const stack = page.Window.window?.pageStack;
        for (let i = 1; stack && i < stack.depth; ++i) {
            if (stack.get(i) === page) {
                // The row slides back to the page below, as Back does
                page.Window.window.popTo(stack.get(i - 1), true);
                return;
            }
        }
    }

    Connections {
        target: page.connection.favorites
        function onModelReset() {
            Qt.callLater(page.leave);
        }
    }

    header: ColumnLayout {
        spacing: 0

        PageToolbar {
            Layout.fillWidth: true
            backShown: true
            title: page.title
        }

        MessageBox {
            Layout.margins: Kirigami.Units.largeSpacing
            visible: page.partial
            type: Kirigami.MessageType.Information
            text: i18n("These are the favorites loaded so far: the list may be incomplete")
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
        objectName: "favoritesGroupGrid"

        // Poster tiles of about ten grid units, at least one to a row
        readonly property int columns: Math.max(1, Math.floor(width / (Kirigami.Units.gridUnit * 10)))

        model: page.items
        cellWidth: Math.floor(width / columns)
        // A 2:3 poster inside the tile margins, then the name and the second line
        cellHeight: Math.round((cellWidth - Kirigami.Units.largeSpacing - Kirigami.Units.smallSpacing * 2) * 1.5) + Kirigami.Units.gridUnit * 3.5 + Kirigami.Units.largeSpacing
        activeFocusOnTab: true
        keyNavigationEnabled: true
        currentIndex: 0
        Accessible.role: Accessible.List
        Accessible.name: page.title

        // The tiles are not tab stops: the grid is, and it opens the current item
        function openCurrent() {
            if (grid.currentItem?.enabled)
                page.Window.window.openItem(grid.currentItem.itemId, page);
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

            width: grid.cellWidth - Kirigami.Units.largeSpacing
            height: grid.cellHeight - Kirigami.Units.largeSpacing
            // The grid is the one tab stop, the arrow keys move inside it
            focusPolicy: Qt.NoFocus
            loaded: true
            highlighted: GridView.isCurrentItem && grid.activeFocus
            onClicked: page.Window.window.openItem(itemId, page)
            subtitle: {
                if (episodeNumber >= 0) {
                    const label = seasonNumber >= 0 ? i18nc("season and episode number", "S%1:E%2", seasonNumber, episodeNumber) : i18nc("episode number", "E%1", episodeNumber);
                    return episodeTitle.length > 0 ? i18nc("episode label and episode title", "%1 — %2", label, episodeTitle) : label;
                }
                return year > 0 ? String(year) : "";
            }
        }
    }
}
