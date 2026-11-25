// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// Every favorite of one media type, the "Show All" page of a row of the Favorites page. It reads the snapshot of that page and starts no scan of its own: it counts as a Favorites page, so the scan stays while it exists. Like the Favorites page it only reads, and it follows the filters of the model, which its own dialog edits as well
Kirigami.ScrollablePage {
    id: page
    objectName: "favoritesSectionPage"

    // The darker View background on every page: a page without a list (a plain flickable) would get the lighter Window one, and the sections would not match
    Kirigami.Theme.colorSet: Kirigami.Theme.View
    Kirigami.Theme.inherit: false

    required property ConnectionController connection
    // The window shell counts the pages: the scan stops with the last one
    required property var shell
    // The item type of the snapshot this page lists ("Movie", "Series", ...); the title is the name of the row
    required property string type

    readonly property FavoritesModel favorites: connection.favorites
    readonly property bool loading: favorites.status === FavoritesModel.Loading

    // The row of the page names it, as the other pages above a tab
    globalToolBarStyle: Kirigami.ApplicationHeaderStyle.None
    // Without Kirigami's title row the page names itself to a screen reader
    Accessible.role: Accessible.Pane
    Accessible.name: title

    actions: [
        Kirigami.Action {
            objectName: "favoritesSectionFiltersAction"
            text: i18n("Filters…")
            tooltip: text
            icon.name: "view-filter"
            onTriggered: filterDialog.open()
        }
    ]

    Component.onCompleted: shell.favoritesPages++
    Component.onDestruction: shell?.favoritesPageGone?.()

    // What the main menu's Refresh calls while this page is in front
    function refreshPage() {
        if (!loading)
            favorites.reload();
    }

    // The grid takes the keys: after a long scroll the current tile is not built any more, and the page keeps its place
    function focusPrimary() {
        grid.focusPrimary(Qt.TabFocusReason);
    }

    // The scan ended: the cards take the keys unless the user went elsewhere meanwhile (a card already focused). The grid holding the focus while it is created is not a choice
    function focusWhenLoaded() {
        if (!page.isCurrentPage || !page.shell || grid.count === 0)
            return;
        const focused = page.Window.window?.activeFocusItem;
        if (focused && focused !== grid && page.shell.focusIsIn(grid))
            return;
        page.focusPrimary();
    }

    // The Clear Filters button of the placeholder goes with the filters it clears, and a hidden item keeps the focus: the grid takes it
    function keepFocusInView() {
        const focused = page.Window.window?.activeFocusItem;
        if (page.isCurrentPage && page.shell && grid.count > 0 && focused && !focused.visible)
            page.focusPrimary();
    }

    Connections {
        target: page.favorites
        function onStatusChanged() {
            if (!page.loading)
                Qt.callLater(page.focusWhenLoaded);
        }
        function onFiltersChanged() {
            Qt.callLater(page.keepFocusInView);
        }
    }

    // The Favorites page below has a dialog of its own on the same model: the object name tells them apart
    FilterDialog {
        id: filterDialog
        objectName: "favoritesSectionFilterDialog"

        list: page.favorites
        favoritesFixed: true
        genreIdsAreNames: true
    }

    header: ColumnLayout {
        spacing: 0

        PageToolbar {
            Layout.fillWidth: true
            backShown: true
            title: page.title
            actions: page.actions
        }

        FilterChips {
            dialog: filterDialog
            list: page.favorites
            clearShown: !(page.favorites.filtered && page.favorites.matchCount === 0)
        }

        FavoritesBanners {
            Layout.fillWidth: true
            favorites: page.favorites
            namePrefix: "favoritesSection"
        }
    }

    GridView {
        id: grid
        objectName: "favoritesSectionGrid"

        // Poster tiles of about ten grid units, at least one to a row
        readonly property real usableWidth: Math.max(0, width - leftMargin)
        readonly property int columns: Math.max(1, Math.floor(usableWidth / (Kirigami.Units.gridUnit * 10)))

        // After a long scroll the current tile is far out of view: the keys start from the first tile in view, so the view does not jump back to the top
        function focusPrimary(reason) {
            const current = grid.currentItem;
            if (!current || current.y + current.height <= grid.contentY || current.y >= grid.contentY + grid.height) {
                // A probe can fall into the gap between two rows, which has no tile: a few heights are tried
                let inView = -1;
                for (const at of [0.5, 0.25, 0.75])
                    inView = inView >= 0 ? inView : grid.indexAt(grid.cellWidth / 2, grid.contentY + grid.cellHeight * at);
                if (inView >= 0)
                    grid.currentIndex = inView;
            }
            grid.forceActiveFocus(reason);
        }

        // The tiles are not tab stops: the grid is, and it opens the current item
        function openCurrent() {
            if (grid.currentItem?.enabled)
                page.Window.window.openItem(grid.currentItem.itemId, page);
        }

        // What scrolls under the page row fades into it instead of being cut by its edge
        TopShadow {
            parent: grid
            z: 1
            atStart: grid.atYBeginning
        }

        Kirigami.LoadingPlaceholder {
            FadeInOnShow {}
            objectName: "favoritesSectionLoading"
            anchors.centerIn: parent
            width: parent.width - Kirigami.Units.gridUnit * 4
            visible: page.loading
            text: i18n("Loading favorites…")
            explanation: page.favorites.libraryCount > 0 ? i18nc("@info the favorites found, then how many libraries were read", "%1, %2", i18np("%1 favorite found", "%1 favorites found", page.favorites.loadedCount), i18np("%2 of %1 library read", "%2 of %1 libraries read", page.favorites.libraryCount, page.favorites.librariesDone)) : ""
            determinate: page.favorites.libraryCount > 0
            progressBar.value: page.favorites.libraryCount > 0 ? 100 * page.favorites.librariesDone / page.favorites.libraryCount : 0
        }

        // A failed library is no empty list: its banner shows instead. After a reload that finds nothing of the type any more the page stays and says so
        Kirigami.PlaceholderMessage {
            FadeInOnShow {}
            objectName: "favoritesSectionEmpty"
            anchors.centerIn: parent
            width: parent.width - Kirigami.Units.gridUnit * 4
            visible: !page.loading && grid.count === 0 && !page.favorites.libraryFailed && !page.favorites.filtered
            icon.name: "favorite"
            text: i18n("No favorites yet")
            explanation: i18n("Movies, shows, and collections you mark as favorites on the server show up here")
            helpfulAction: Kirigami.Action {
                text: i18n("Reload")
                icon.name: "view-refresh"
                onTriggered: page.favorites.reload()
            }
        }

        // The filters leave no favorite of this type (the other types of the snapshot may still have some)
        Kirigami.PlaceholderMessage {
            FadeInOnShow {}
            objectName: "favoritesSectionNoMatch"
            anchors.centerIn: parent
            width: parent.width - Kirigami.Units.gridUnit * 4
            visible: !page.loading && grid.count === 0 && page.favorites.filtered
            icon.name: "view-filter"
            text: i18n("No favorites match the filters")
            helpfulAction: Kirigami.Action {
                objectName: "favoritesSectionClearFiltersAction"
                text: i18n("Clear Filters")
                icon.name: "edit-clear-all"
                onTriggered: page.favorites.clearFilters()
            }
        }

        model: FavoritesSectionModel {
            source: page.favorites
            type: page.type
        }
        // The page margin on the left, as the library grid has it: the cards do not touch the edge
        leftMargin: Kirigami.Units.largeSpacing
        cellWidth: Math.floor(usableWidth / columns)
        cellHeight: Math.round((cellWidth - Kirigami.Units.largeSpacing - Kirigami.Units.smallSpacing * 2) * 1.5) + Kirigami.Units.gridUnit * 3.5 + Kirigami.Units.largeSpacing
        topMargin: Kirigami.Units.largeSpacing * 2
        bottomMargin: Kirigami.Units.largeSpacing
        activeFocusOnTab: true
        keyNavigationEnabled: true
        currentIndex: 0
        Accessible.role: Accessible.List
        Accessible.name: page.title

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

        delegate: FavoriteCard {
            objectName: "favoritesSectionCard_" + index
            width: grid.cellWidth - Kirigami.Units.largeSpacing
            height: grid.cellHeight - Kirigami.Units.largeSpacing
            highlighted: GridView.isCurrentItem && grid.activeFocus
            onClicked: page.Window.window.openItem(itemId, page)
        }
    }
}
