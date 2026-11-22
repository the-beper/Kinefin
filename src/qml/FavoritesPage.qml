// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// The favorites of the signed-in user across the libraries, as one flat grid. The page only reads: nothing here marks or unmarks a favorite
Kirigami.ScrollablePage {
    id: page
    objectName: "favoritesPage"

    required property ConnectionController connection
    // The window shell counts the pages: the scan stops with the last one
    required property var shell

    readonly property FavoritesModel favorites: connection.favorites
    readonly property bool loading: favorites.status === FavoritesModel.Loading
    // The page starts the scan as it is created, so Idle here means the scan could not start
    readonly property bool idle: favorites.status === FavoritesModel.Idle

    title: i18n("Favorites")
    // No title bar, no toolbar: the page has no actions, and the Favorites tab of the header says which page this is
    globalToolBarStyle: Kirigami.ApplicationHeaderStyle.None
    // Without Kirigami's title row the page names itself to a screen reader
    Accessible.role: Accessible.Pane
    Accessible.name: title

    Component.onCompleted: {
        shell.favoritesPages++;
        connection.openFavorites();
    }
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

    // The scan ended: the cards take the keys unless the user went elsewhere meanwhile (the header box, or a card already focused). The grid holding the focus while it is created is not a choice
    function focusWhenLoaded() {
        if (!page.isCurrentPage || !page.shell || page.favorites.count === 0 || page.shell.header?.searchFieldHasFocus)
            return;
        const focused = page.Window.window?.activeFocusItem;
        if (focused && focused !== grid && page.shell.focusIsIn(grid))
            return;
        page.focusPrimary();
    }

    Connections {
        target: page.favorites
        function onStatusChanged() {
            if (!page.loading)
                Qt.callLater(page.focusWhenLoaded);
        }
    }

    // The card of a favorite: an episode shows its label under the series name
    component FavoriteCard: MediaCard {
        required property int seasonNumber
        required property int episodeNumber
        required property string episodeTitle

        // The grid is the one tab stop, the arrow keys move inside it
        focusPolicy: Qt.NoFocus
        loaded: true
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

    header: ColumnLayout {
        spacing: 0

        // One box at a time: a failed library comes first, and its box says the list may be incomplete as well
        MessageBox {
            objectName: "favoritesCappedBanner"
            Layout.margins: Kirigami.Units.largeSpacing
            visible: page.favorites.capped && !page.favorites.libraryFailed
            type: Kirigami.MessageType.Information
            text: i18np("Showing %1 favorite: the list may be incomplete", "Showing %1 favorites: the list may be incomplete", page.favorites.count)
        }

        MessageBox {
            objectName: "favoritesPartialBanner"
            Layout.margins: Kirigami.Units.largeSpacing
            visible: page.favorites.libraryFailed
            type: Kirigami.MessageType.Warning
            text: i18n("Some libraries could not be read: the list may be incomplete")
            actions: [
                Kirigami.Action {
                    text: i18n("Retry")
                    icon.name: "view-refresh"
                    onTriggered: page.favorites.reload()
                }
            ]
        }
    }

    GridView {
        id: grid
        objectName: "favoritesGrid"

        // Poster tiles of about ten grid units, at least one to a row
        readonly property int columns: Math.max(1, Math.floor(width / (Kirigami.Units.gridUnit * 10)))

        // After a long scroll the current tile is far out of view: the keys start from the first tile in view, so the view does not jump back to the top
        function focusPrimary(reason) {
            const current = grid.currentItem;
            if (!current || current.y + current.height <= grid.contentY || current.y >= grid.contentY + grid.height) {
                const inView = grid.indexAt(grid.cellWidth / 2, grid.contentY + grid.cellHeight / 2);
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

        // On the view, not in its content: the fade stays under the toolbar while the tiles scroll
        TopFade {
            objectName: "favoritesTopFade"
            parent: grid
            z: 1
            atStart: grid.atYBeginning
        }

        Kirigami.LoadingPlaceholder {
            FadeInOnShow {}
            objectName: "favoritesLoading"
            anchors.centerIn: parent
            width: parent.width - Kirigami.Units.gridUnit * 4
            visible: page.loading
            text: i18n("Loading favorites…")
            explanation: page.favorites.libraryCount > 0 ? i18np("%1 favorite found, %2 of %3 libraries read", "%1 favorites found, %2 of %3 libraries read", page.favorites.loadedCount, page.favorites.librariesDone, page.favorites.libraryCount) : ""
            determinate: page.favorites.libraryCount > 0
            progressBar.value: page.favorites.libraryCount > 0 ? 100 * page.favorites.librariesDone / page.favorites.libraryCount : 0
        }

        Kirigami.PlaceholderMessage {
            FadeInOnShow {}
            objectName: "favoritesNotLoaded"
            anchors.centerIn: parent
            width: parent.width - Kirigami.Units.gridUnit * 4
            visible: page.idle
            icon.name: "dialog-warning"
            text: i18n("Favorites could not be loaded")
            explanation: i18n("The favorites could not be read from the server: try again")
            helpfulAction: Kirigami.Action {
                text: i18n("Retry")
                icon.name: "view-refresh"
                onTriggered: page.favorites.reload()
            }
        }

        Kirigami.PlaceholderMessage {
            FadeInOnShow {}
            objectName: "favoritesNoLibraries"
            anchors.centerIn: parent
            width: parent.width - Kirigami.Units.gridUnit * 4
            visible: !page.loading && !page.idle && page.favorites.libraryCount === 0
            icon.name: "folder-video"
            text: i18n("No libraries to show")
            explanation: i18n("Favorites come from your movie, TV show, and collection libraries")
            helpfulAction: Kirigami.Action {
                text: i18n("Go to Home")
                icon.name: "go-home"
                onTriggered: page.shell.goHome()
            }
        }

        // A failed library is no empty list: its banner shows instead
        Kirigami.PlaceholderMessage {
            FadeInOnShow {}
            objectName: "favoritesEmpty"
            anchors.centerIn: parent
            width: parent.width - Kirigami.Units.gridUnit * 4
            visible: !page.loading && !page.idle && page.favorites.libraryCount > 0 && page.favorites.count === 0 && !page.favorites.libraryFailed
            icon.name: "favorite"
            text: i18n("No favorites yet")
            explanation: i18n("Movies, shows, and collections you mark as favorites on the server show up here")
            helpfulAction: Kirigami.Action {
                text: i18n("Reload")
                icon.name: "view-refresh"
                onTriggered: page.favorites.reload()
            }
        }

        model: page.favorites
        cellWidth: Math.floor(width / columns)
        cellHeight: Math.round((cellWidth - Kirigami.Units.largeSpacing - Kirigami.Units.smallSpacing * 2) * 1.5) + Kirigami.Units.gridUnit * 3.5 + Kirigami.Units.largeSpacing
        topMargin: Kirigami.Units.largeSpacing
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
            objectName: "favoritesCard_" + index
            width: grid.cellWidth - Kirigami.Units.largeSpacing
            height: grid.cellHeight - Kirigami.Units.largeSpacing
            highlighted: GridView.isCurrentItem && grid.activeFocus
            onClicked: page.Window.window.openItem(itemId, page)
        }
    }
}
