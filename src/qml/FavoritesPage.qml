// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// The favorites of the signed-in user across the libraries, as one row of cards per media type, which the filters narrow (the Show All pages share them). The page only reads: nothing here marks or unmarks a favorite
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
    readonly property bool noLibraries: !loading && !idle && favorites.libraryCount === 0
    // A failed library is no empty list: its banner shows instead
    readonly property bool empty: !loading && !idle && favorites.libraryCount > 0 && favorites.count === 0 && !favorites.libraryFailed
    // The filters leave no favorite of the snapshot: the rows are all hidden
    readonly property bool noMatch: !loading && !idle && favorites.count > 0 && favorites.matchCount === 0

    // The rows, top to bottom: the item type of the snapshot and the title of the row. A type without favorites has no row, one more media type is one more entry
    readonly property var sections: [
        {
            type: "Movie",
            title: i18nc("@title:group section of the favorites page", "Movies")
        },
        {
            type: "Series",
            title: i18nc("@title:group section of the favorites page", "Shows")
        },
        {
            type: "Season",
            title: i18nc("@title:group section of the favorites page", "Seasons")
        },
        {
            type: "Episode",
            title: i18nc("@title:group section of the favorites page", "Episodes")
        },
        {
            type: "BoxSet",
            title: i18nc("@title:group section of the favorites page", "Collections")
        }
    ]
    // The cards of a row, as the Home rows; a type with more has Show All
    readonly property int rowLimit: 24

    title: i18n("Favorites")
    // No title bar, no toolbar: the filters are chosen on the Show All pages, and the Favorites tab of the header says which page this is
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

    // The first card of the first row takes the keys
    function focusPrimary() {
        for (let i = 0; i < rowsRepeater.count; ++i) {
            const row = rowsRepeater.itemAt(i);
            if (row?.hasCards) {
                row.cards.focusList(Qt.TabFocusReason);
                return;
            }
        }
    }

    // The scan ended: the first row takes the keys unless the user went elsewhere meanwhile (a row already focused)
    function focusWhenLoaded() {
        if (!page.isCurrentPage || !page.shell || page.favorites.count === 0)
            return;
        // A row that lost its cards keeps the active focus while it is hidden: that focus is no choice of the user
        if (page.shell.focusIsIn(rowsColumn) && page.Window.window?.activeFocusItem?.visible)
            return;
        page.focusPrimary();
    }

    // The filters can empty a row whose list holds the focus or take away the Clear Filters button of the placeholder, and a hidden item keeps the focus: the first card still showing takes it (a hidden item is no choice of the user)
    function keepFocusInView() {
        const focused = page.Window.window?.activeFocusItem;
        if (page.isCurrentPage && page.shell && focused && !focused.visible)
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

    // The one filter of the Favorites pages lives in the model: the dialog of a Show All page edits it, the chips and the rows of this page and of every Show All page follow it. This dialog is never opened: the chips build their labels and take a filter off through it
    FilterDialog {
        id: filterDialog

        list: page.favorites
        favoritesFixed: true
        genreIdsAreNames: true
        // The focus goes back to the control that had it, which the filter may have hidden meanwhile
        onClosed: Qt.callLater(page.keepFocusInView)
    }

    header: ColumnLayout {
        spacing: 0

        FilterChips {
            dialog: filterDialog
            list: page.favorites
            clearShown: !page.noMatch
        }

        FavoritesBanners {
            Layout.fillWidth: true
            favorites: page.favorites
        }
    }

    ColumnLayout {
        id: rowsColumn
        spacing: Kirigami.Units.largeSpacing

        // The states of the page without rows, centred in the room the page has
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.max(Kirigami.Units.gridUnit * 8, (page.flickable ? page.flickable.height : page.availableHeight) - Kirigami.Units.gridUnit * 2)
            visible: page.loading || page.idle || page.noLibraries || page.empty || page.noMatch

            Kirigami.LoadingPlaceholder {
                FadeInOnShow {}
                objectName: "favoritesLoading"
                anchors.centerIn: parent
                width: parent.width - Kirigami.Units.gridUnit * 4
                visible: page.loading
                text: i18n("Loading favorites…")
                explanation: page.favorites.libraryCount > 0 ? i18nc("@info the favorites found, then how many libraries were read", "%1, %2", i18np("%1 favorite found", "%1 favorites found", page.favorites.loadedCount), i18np("%2 of %1 library read", "%2 of %1 libraries read", page.favorites.libraryCount, page.favorites.librariesDone)) : ""
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
                visible: page.noLibraries
                icon.name: "folder-video"
                text: i18n("No libraries to show")
                explanation: i18n("Favorites come from your movie, TV show, and collection libraries")
                helpfulAction: Kirigami.Action {
                    text: i18n("Go to Home")
                    icon.name: "go-home"
                    onTriggered: page.shell.goHome()
                }
            }

            Kirigami.PlaceholderMessage {
                FadeInOnShow {}
                objectName: "favoritesEmpty"
                anchors.centerIn: parent
                width: parent.width - Kirigami.Units.gridUnit * 4
                visible: page.empty
                icon.name: "favorite"
                text: i18n("No favorites yet")
                explanation: i18n("Movies, shows, and collections you mark as favorites on the server show up here")
                helpfulAction: Kirigami.Action {
                    text: i18n("Reload")
                    icon.name: "view-refresh"
                    onTriggered: page.favorites.reload()
                }
            }

            Kirigami.PlaceholderMessage {
                FadeInOnShow {}
                objectName: "favoritesNoMatch"
                anchors.centerIn: parent
                width: parent.width - Kirigami.Units.gridUnit * 4
                visible: page.noMatch
                icon.name: "view-filter"
                text: i18n("No favorites match the filters")
                helpfulAction: Kirigami.Action {
                    objectName: "favoritesClearFiltersAction"
                    text: i18n("Clear Filters")
                    icon.name: "edit-clear-all"
                    onTriggered: page.favorites.clearFilters()
                }
            }
        }

        Repeater {
            id: rowsRepeater
            model: page.sections
            delegate: ColumnLayout {
                id: row
                required property var modelData

                readonly property string type: modelData.type
                readonly property string title: modelData.title
                readonly property bool hasCards: items.count > 0
                // The row's list of cards, for the keys
                readonly property Item cards: cardRow
                // The poster ratios match MediaCard: the height is the poster plus the two text lines
                readonly property real cardWidth: Kirigami.Units.gridUnit * 8
                readonly property real cardHeight: Math.round((cardWidth - Kirigami.Units.smallSpacing * 2) * 3 / 2) + Kirigami.Units.gridUnit * 3.5

                objectName: "favoritesRow_" + type
                Layout.fillWidth: true
                visible: hasCards
                spacing: Kirigami.Units.smallSpacing

                FavoritesSectionModel {
                    id: items
                    source: page.favorites
                    type: row.type
                    limit: page.rowLimit
                }

                RowLayout {
                    Layout.fillWidth: true

                    Kirigami.Heading {
                        Layout.fillWidth: true
                        level: 2
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                        text: row.title
                    }

                    Controls.Button {
                        objectName: "favoritesSeeAll_" + row.type
                        visible: items.total > page.rowLimit
                        text: i18n("Show All")
                        icon.name: "go-next"
                        Accessible.name: i18n("Show All in %1", row.title)
                        onClicked: page.Window.window.openFavoritesSection(row.type, row.title, page)
                    }
                }

                CardRow {
                    id: cardRow
                    listObjectName: "favoritesRowList_" + row.type
                    model: items
                    cardHeight: row.cardHeight
                    accessibleName: row.title
                    pageFlickable: page.flickable

                    delegate: FavoriteCard {
                        width: row.cardWidth
                        height: row.cardHeight
                        highlighted: ListView.isCurrentItem && ListView.view.activeFocus
                        onClicked: page.Window.window.openItem(itemId, page)
                    }
                }
            }
        }
    }
}
