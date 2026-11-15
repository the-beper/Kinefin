// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// The favorites of the signed-in user across the libraries, grouped on the client: one row of cards per group. The page only reads: nothing here marks or unmarks a favorite
Kirigami.ScrollablePage {
    id: page
    objectName: "favoritesPage"

    required property ConnectionController connection
    // The window shell counts the pages: the scan stops with the last one
    required property var shell

    readonly property FavoritesModel favorites: connection.favorites
    readonly property FavoritesGroupsModel groups: connection.favoriteGroups
    readonly property bool loading: favorites.status === FavoritesModel.Loading
    // The page starts the scan as it is created, so Idle here means the scan could not start
    readonly property bool idle: favorites.status === FavoritesModel.Idle
    // The poster ratios match MediaCard: the height is the poster plus the two text lines
    readonly property real cardWidth: Kirigami.Units.gridUnit * 8
    readonly property real cardHeight: Math.round((cardWidth - Kirigami.Units.smallSpacing * 2) * 3 / 2) + Kirigami.Units.gridUnit * 3.5

    title: i18n("Favorites")
    // No title bar, as in the Movies and Shows tabs: the actions sit in a toolbar on top of the content, the header's star says which page this is
    globalToolBarStyle: Kirigami.ApplicationHeaderStyle.None

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

    // The first row of cards, or the grid, takes the keys
    function focusPrimary() {
        groupsList.itemAtIndex(0)?.focusPrimary(Qt.TabFocusReason);
    }

    // The scan ended: the cards take the keys unless the user went elsewhere meanwhile (the header box, or a card already focused). The list itself holding the focus is what the page gives it while it is created, not a choice
    function focusWhenLoaded() {
        if (!page.isCurrentPage || !page.shell || page.groups.count === 0 || page.shell.header?.searchFieldHasFocus)
            return;
        const focused = page.Window.window?.activeFocusItem;
        if (focused && focused !== groupsList && page.shell.focusIsIn(groupsList))
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

    // The title of a group: the types and the fixed groups are translated here, library and genre names are server text and stay as they are
    function groupHeading(kind, key, title) {
        switch (kind) {
        case FavoritesGroupsModel.TypeGroup:
            switch (key) {
            case "Movie":
                return i18nc("favorites grouped by type", "Movies");
            case "Series":
                return i18nc("favorites grouped by type", "Shows");
            case "Season":
                return i18nc("favorites grouped by type", "Seasons");
            case "Episode":
                return i18nc("favorites grouped by type", "Episodes");
            case "BoxSet":
                return i18nc("favorites grouped by type", "Collections");
            }
            return i18nc("favorites grouped by type", "Other");
        case FavoritesGroupsModel.MoreLibrariesGroup:
            return i18nc("favorites listed by several libraries", "More Libraries");
        case FavoritesGroupsModel.NoGenreGroup:
            return i18nc("favorites without a genre", "No Genre");
        case FavoritesGroupsModel.NoYearGroup:
            return i18nc("favorites without a year", "No Year");
        case FavoritesGroupsModel.AllGroup:
            return i18nc("all favorites in one group", "All Favorites");
        }
        return title;
    }

    // Show All of a group: its cards as a grid, a page above this one
    function showAll(items, heading) {
        page.Window.window.pageStack.push(Qt.resolvedUrl("FavoritesGroupPage.qml"), { connection: page.connection, items: items, heading: heading });
    }

    // A checkable action whose check mark always follows `current`: a click toggles the check first, then the binding is restored
    component ChoiceAction: Kirigami.Action {
        id: choice
        property bool current: false
        signal chosen

        checkable: true
        checked: current
        onTriggered: {
            chosen();
            checked = Qt.binding(() => choice.current);
        }
    }

    // The card of a favorite: an episode shows its label under the series name
    component FavoriteCard: MediaCard {
        required property int seasonNumber
        required property int episodeNumber
        required property string episodeTitle

        // The list or the grid is the one tab stop, the arrow keys move inside it
        focusPolicy: Qt.NoFocus
        loaded: true
        subtitle: {
            if (episodeNumber >= 0) {
                const label = seasonNumber >= 0 ? i18nc("season and episode number", "S%1:E%2", seasonNumber, episodeNumber) : i18nc("episode number", "E%1", episodeNumber);
                return episodeTitle.length > 0 ? i18nc("episode label and episode title", "%1 — %2", label, episodeTitle) : label;
            }
            return year > 0 ? String(year) : "";
        }
    }

    actions: [
        Kirigami.Action {
            objectName: "favoritesGroupBy"
            text: i18n("Group By")
            icon.name: "view-group"
            enabled: !page.loading

            ChoiceAction {
                objectName: "favoritesGroupBy_Type"
                text: i18nc("group favorites by", "Type")
                current: page.groups.grouping === FavoritesGroupsModel.Type
                onChosen: page.groups.grouping = FavoritesGroupsModel.Type
            }
            ChoiceAction {
                objectName: "favoritesGroupBy_Library"
                text: i18nc("group favorites by", "Library")
                current: page.groups.grouping === FavoritesGroupsModel.Library
                onChosen: page.groups.grouping = FavoritesGroupsModel.Library
            }
            ChoiceAction {
                objectName: "favoritesGroupBy_Genre"
                text: i18nc("group favorites by", "Genre")
                current: page.groups.grouping === FavoritesGroupsModel.Genre
                onChosen: page.groups.grouping = FavoritesGroupsModel.Genre
            }
            ChoiceAction {
                objectName: "favoritesGroupBy_Year"
                text: i18nc("group favorites by", "Year")
                current: page.groups.grouping === FavoritesGroupsModel.Year
                onChosen: page.groups.grouping = FavoritesGroupsModel.Year
            }
            ChoiceAction {
                objectName: "favoritesGroupBy_None"
                text: i18nc("do not group favorites", "None")
                current: page.groups.grouping === FavoritesGroupsModel.None
                onChosen: page.groups.grouping = FavoritesGroupsModel.None
            }
        }
    ]

    header: ColumnLayout {
        spacing: 0

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: Kirigami.Units.largeSpacing
            Layout.rightMargin: Kirigami.Units.largeSpacing
            spacing: Kirigami.Units.mediumSpacing

            // A Compact window hides the header's star, which otherwise names the page
            Kirigami.Heading {
                objectName: "favoritesHeading"
                visible: page.Window.window?.compactHeader ?? false
                level: 3
                text: page.title
            }

            Kirigami.ActionToolBar {
                objectName: "favoritesToolbar"
                Layout.fillWidth: true
                actions: page.actions
            }
        }

        MessageBox {
            objectName: "favoritesCappedBanner"
            Layout.margins: Kirigami.Units.largeSpacing
            visible: page.favorites.capped
            type: Kirigami.MessageType.Information
            text: i18np("Showing %1 favorite: the list may be incomplete", "Showing %1 favorites: the list may be incomplete", page.favorites.count)
        }

        MessageBox {
            objectName: "favoritesPartialBanner"
            Layout.margins: Kirigami.Units.largeSpacing
            visible: page.favorites.libraryFailed
            type: Kirigami.MessageType.Warning
            text: i18n("Some libraries could not be read")
            actions: [
                Kirigami.Action {
                    text: i18n("Retry")
                    icon.name: "view-refresh"
                    onTriggered: page.favorites.reload()
                }
            ]
        }
    }

    ListView {
        id: groupsList
        objectName: "favoritesGroups"

        // On the view, not in its content: the fade stays under the toolbar while the rows scroll
        TopFade {
            objectName: "favoritesTopFade"
            parent: groupsList
            z: 1
            atStart: groupsList.atYBeginning
        }

        model: page.groups
        spacing: Kirigami.Units.largeSpacing
        topMargin: Kirigami.Units.largeSpacing
        bottomMargin: Kirigami.Units.largeSpacing
        // Every group is built at once; DelegateModel still warns about a cancelled index in aLostSessionEmptiesTheFavorites under the desktop style, harmless because the page is empty and closed (the view resets while a group is being created asynchronously)
        cacheBuffer: 1000000

        // Moves the focus to the group above or below `from`: that row may not exist yet, so the view scrolls to it first
        function focusGroup(from, step) {
            const at = from + step;
            if (at < 0 || at >= groupsList.count)
                return;
            groupsList.positionViewAtIndex(at, ListView.Contain);
            groupsList.itemAtIndex(at)?.focusPrimary(step > 0 ? Qt.TabFocusReason : Qt.BacktabFocusReason);
        }

        Kirigami.LoadingPlaceholder {
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
            objectName: "favoritesNoLibraries"
            anchors.centerIn: parent
            width: parent.width - Kirigami.Units.gridUnit * 4
            visible: !page.loading && !page.idle && page.favorites.libraryCount === 0
            icon.name: "folder-video"
            text: i18n("No libraries to show")
            explanation: i18n("Favorites come from your movie, TV show and collection libraries")
            helpfulAction: Kirigami.Action {
                text: i18n("Go to Home")
                icon.name: "go-home"
                onTriggered: page.shell.goHome()
            }
        }

        // A failed library is no empty list: its banner shows instead
        Kirigami.PlaceholderMessage {
            objectName: "favoritesEmpty"
            anchors.centerIn: parent
            width: parent.width - Kirigami.Units.gridUnit * 4
            visible: !page.loading && !page.idle && page.favorites.libraryCount > 0 && page.favorites.count === 0 && !page.favorites.libraryFailed
            icon.name: "favorite"
            text: i18n("No favorites yet")
            explanation: i18n("Movies, shows and collections you mark as favorites on the server show up here")
            helpfulAction: Kirigami.Action {
                text: i18n("Reload")
                icon.name: "view-refresh"
                onTriggered: page.favorites.reload()
            }
        }

        delegate: Item {
            id: group
            objectName: "favoritesGroup_" + index

            required property int index
            required property int kind
            required property string key
            required property string title
            required property int count
            required property var items

            readonly property bool wholeList: kind === FavoritesGroupsModel.AllGroup
            readonly property string heading: page.groupHeading(kind, key, title)

            width: groupsList.width
            // The single group of "None" is the grid and takes the whole view, the others take the height of their row
            implicitHeight: wholeList ? groupsList.height - groupsList.topMargin - groupsList.bottomMargin : rowLoader.implicitHeight
            height: implicitHeight

            function focusPrimary(reason) {
                const shown = rowLoader.item ?? gridLoader.item;
                shown?.focusPrimary(reason);
            }

            Loader {
                id: rowLoader
                active: !group.wholeList
                width: group.width
                sourceComponent: rowGroup
            }

            Loader {
                id: gridLoader
                active: group.wholeList
                width: group.width
                height: group.height
                sourceComponent: gridGroup
            }

            Component {
                id: rowGroup

                ColumnLayout {
                    spacing: Kirigami.Units.smallSpacing

                    function focusPrimary(reason) {
                        cards.focusList(reason);
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.leftMargin: Kirigami.Units.largeSpacing
                        Layout.rightMargin: Kirigami.Units.largeSpacing

                        Kirigami.Heading {
                            objectName: "favoritesGroupTitle_" + group.index
                            Layout.fillWidth: true
                            level: 2
                            Accessible.role: Accessible.Heading
                            // Library and genre names come from the server: plain text only
                            textFormat: Text.PlainText
                            elide: Text.ElideRight
                            text: group.heading
                        }

                        Controls.Label {
                            objectName: "favoritesGroupCount_" + group.index
                            opacity: 0.7
                            textFormat: Text.PlainText
                            text: i18np("%1 favorite", "%1 favorites", group.count)
                        }

                        Controls.ToolButton {
                            objectName: "favoritesShowAll_" + group.index
                            text: i18n("Show All")
                            icon.name: "go-next"
                            Accessible.name: i18n("Show All in %1", group.heading)
                            onClicked: page.showAll(group.items, group.heading)
                        }
                    }

                    CardRow {
                        id: cards
                        Layout.leftMargin: Kirigami.Units.largeSpacing
                        Layout.rightMargin: Kirigami.Units.largeSpacing
                        listObjectName: "favoritesRow_" + group.index
                        model: group.items
                        cardHeight: page.cardHeight
                        accessibleName: group.heading
                        pageFlickable: groupsList
                        externalVerticalNavigation: true
                        onUpRequested: groupsList.focusGroup(group.index, -1)
                        onDownRequested: groupsList.focusGroup(group.index, 1)

                        delegate: FavoriteCard {
                            width: page.cardWidth
                            height: page.cardHeight
                            highlighted: ListView.isCurrentItem && ListView.view.activeFocus
                            onClicked: page.Window.window.openItem(itemId, page)
                        }
                    }
                }
            }

            Component {
                id: gridGroup

                GridView {
                    id: grid
                    objectName: "favoritesGrid"

                    // Poster tiles of about ten grid units, at least one to a row
                    readonly property int columns: Math.max(1, Math.floor(width / (Kirigami.Units.gridUnit * 10)))

                    function focusPrimary(reason) {
                        grid.forceActiveFocus(reason);
                    }

                    // The tiles are not tab stops: the grid is, and it opens the current item
                    function openCurrent() {
                        if (grid.currentItem?.enabled)
                            page.Window.window.openItem(grid.currentItem.itemId, page);
                    }

                    model: group.items
                    cellWidth: Math.floor(width / columns)
                    cellHeight: Math.round((cellWidth - Kirigami.Units.largeSpacing - Kirigami.Units.smallSpacing * 2) * 1.5) + Kirigami.Units.gridUnit * 3.5 + Kirigami.Units.largeSpacing
                    activeFocusOnTab: true
                    keyNavigationEnabled: true
                    currentIndex: 0
                    Accessible.role: Accessible.List
                    Accessible.name: group.heading

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
                        width: grid.cellWidth - Kirigami.Units.largeSpacing
                        height: grid.cellHeight - Kirigami.Units.largeSpacing
                        highlighted: GridView.isCurrentItem && grid.activeFocus
                        onClicked: page.Window.window.openItem(itemId, page)
                    }
                }
            }
        }
    }
}
