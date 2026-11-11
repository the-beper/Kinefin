// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Kirigami.ScrollablePage {
    id: page
    objectName: "libraryPage"

    required property LibraryItemsModel model
    required property ConnectionController connection
    // The name comes from the server: the page title renders it as plain text
    property string libraryName: ""
    // "movies" or "tvshows" for the page of a tab (its title is the tab name and the header box searches its list), empty for a library opened from a tile
    property string tabType: ""
    // The libraries the tab can list (`id` and `name` of each): the selector shows from two
    readonly property var libraries: tabType === "movies" ? connection.movieLibraries : tabType === "tvshows" ? connection.showLibraries : []
    // The header box hides in Compact mode, and this page's own field takes its place
    readonly property bool compactShell: Window.window ? Window.window.compactHeader : false

    title: tabType === "movies" ? i18n("Movies") : tabType === "tvshows" ? i18n("TV Shows") : libraryName

    // The scroll position and the current tile, for the tab to come back as it was left
    function viewPosition() {
        return { currentIndex: grid.currentIndex, contentY: grid.contentY };
    }

    // Applied once the grid has rows: the tab may still be loading its first page
    property var pendingPosition: null
    function restoreViewPosition(position) {
        pendingPosition = position;
        applyPendingPosition();
    }
    function applyPendingPosition() {
        if (!pendingPosition || grid.count === 0)
            return;
        const position = pendingPosition;
        pendingPosition = null;
        grid.currentIndex = Math.min(position.currentIndex, grid.count - 1);
        // The page may reset the scroll while it is created: the position goes in after that
        Qt.callLater(() => {
            if (grid)
                grid.contentY = Math.min(position.contentY, Math.max(0, grid.contentHeight - grid.height));
        });
    }

    // The tab's search field takes the keyboard
    function focusSearchField() {
        const field = tabSearch.visible ? tabSearch : null;
        field?.forceActiveFocus(Qt.TabFocusReason);
        field?.selectAll();
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

    actions: [
        Kirigami.Action {
            text: i18n("Sort")
            icon.name: "view-sort"

            ChoiceAction {
                text: i18n("Name")
                current: page.model.sortKey === LibraryItemsModel.Name
                onChosen: page.model.sortKey = LibraryItemsModel.Name
            }
            ChoiceAction {
                text: i18n("Date Added")
                current: page.model.sortKey === LibraryItemsModel.DateAdded
                onChosen: page.model.sortKey = LibraryItemsModel.DateAdded
            }
            ChoiceAction {
                text: i18n("Release Date")
                current: page.model.sortKey === LibraryItemsModel.ReleaseDate
                onChosen: page.model.sortKey = LibraryItemsModel.ReleaseDate
            }
            ChoiceAction {
                text: i18n("Community Rating")
                current: page.model.sortKey === LibraryItemsModel.CommunityRating
                onChosen: page.model.sortKey = LibraryItemsModel.CommunityRating
            }
            ChoiceAction {
                text: i18n("Date Played")
                current: page.model.sortKey === LibraryItemsModel.DatePlayed
                onChosen: page.model.sortKey = LibraryItemsModel.DatePlayed
            }
            ChoiceAction {
                text: i18n("Runtime")
                current: page.model.sortKey === LibraryItemsModel.Runtime
                onChosen: page.model.sortKey = LibraryItemsModel.Runtime
            }
            Kirigami.Action {
                separator: true
            }
            ChoiceAction {
                text: i18n("Descending Order")
                current: page.model.descending
                onChosen: page.model.descending = !page.model.descending
            }
        },
        Kirigami.Action {
            text: i18n("Filter")
            icon.name: "view-filter"

            ChoiceAction {
                text: i18n("All")
                current: page.model.filter === LibraryItemsModel.All
                onChosen: page.model.filter = LibraryItemsModel.All
            }
            ChoiceAction {
                text: i18n("Unplayed")
                current: page.model.filter === LibraryItemsModel.Unplayed
                onChosen: page.model.filter = LibraryItemsModel.Unplayed
            }
            ChoiceAction {
                text: i18n("Played")
                current: page.model.filter === LibraryItemsModel.Played
                onChosen: page.model.filter = LibraryItemsModel.Played
            }
            ChoiceAction {
                text: i18n("Resumable")
                visible: page.model.resumableAvailable
                current: page.model.filter === LibraryItemsModel.Resumable
                onChosen: page.model.filter = LibraryItemsModel.Resumable
            }
            ChoiceAction {
                text: i18n("Favorites")
                current: page.model.filter === LibraryItemsModel.Favorites
                onChosen: page.model.filter = LibraryItemsModel.Favorites
            }
            Kirigami.Action {
                separator: true
                visible: page.model.genreOptions.available
            }
            ChoiceAction {
                text: i18n("Filter by Genre…")
                visible: page.model.genreOptions.available
                current: page.model.genreIds.length > 0
                onChosen: genreDialog.open()
            }
        },
        Kirigami.Action {
            text: i18n("Browse by Genre")
            icon.name: "view-list-details"
            visible: page.model.genreOptions.available
            onTriggered: page.Window.window.openGenres(page)
        }
    ]

    GenreFilterDialog {
        id: genreDialog
        library: page.model
    }

    header: ColumnLayout {
        spacing: 0

        RowLayout {
            Layout.margins: Kirigami.Units.largeSpacing
            visible: tabSearch.visible || selector.visible
            spacing: Kirigami.Units.largeSpacing

            Kirigami.SearchField {
                id: tabSearch
                objectName: "librarySearchField"

                Layout.fillWidth: true
                Layout.maximumWidth: Kirigami.Units.gridUnit * 30
                visible: page.tabType !== "" && page.compactShell
                // The shell owns Ctrl+F: the field's own sequence would make it ambiguous
                focusSequences: []
                maximumLength: 128
                autoAccept: false
                placeholderText: page.tabType === "movies" ? i18nc("@info:placeholder", "Search in Movies") : i18nc("@info:placeholder", "Search in TV Shows")

                // The field is the source of truth while it holds the focus, and follows the term the header box changes
                onTextChanged: {
                    if (text !== page.model.searchTerm)
                        page.model.searchTerm = text;
                }
                Keys.onDownPressed: page.focusPrimary()
                Component.onCompleted: text = page.model.searchTerm
            }

            Connections {
                target: page.model
                function onSearchTermChanged() {
                    if (tabSearch.text !== page.model.searchTerm)
                        tabSearch.text = page.model.searchTerm;
                }
            }

            Controls.ComboBox {
                id: selector
                objectName: "librarySelector"

                // "*" is no Jellyfin id: openType takes any id that is not a library of the type as All
                readonly property var entries: [{ id: "*", name: i18n("All libraries") }].concat(page.libraries)
                function scopeIndex() {
                    const index = entries.findIndex(entry => entry.id === page.model.libraryId);
                    return index < 0 ? 0 : index;
                }

                visible: page.tabType !== "" && page.libraries.length > 1
                model: entries
                textRole: "name"
                currentIndex: scopeIndex()
                Accessible.name: i18nc("@label:listbox", "Library")
                onActivated: index => {
                    page.connection.openType(page.tabType, entries[index].id);
                    currentIndex = Qt.binding(() => selector.scopeIndex());
                }

                // Library names come from the server: plain text only
                contentItem: Controls.Label {
                    leftPadding: Kirigami.Units.smallSpacing
                    verticalAlignment: Text.AlignVCenter
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                    text: selector.displayText
                }
                delegate: Controls.ItemDelegate {
                    id: entry

                    required property var modelData
                    required property int index

                    width: ListView.view ? ListView.view.width : implicitWidth
                    highlighted: selector.highlightedIndex === entry.index
                    contentItem: Controls.Label {
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                        text: entry.modelData.name
                    }
                }
            }
        }

        MessageBox {
            Layout.margins: Kirigami.Units.largeSpacing
            visible: page.model.status === LibraryItemsModel.Failed
            type: Kirigami.MessageType.Error
            text: i18n("Could not load this library: check the connection to the server and try again")
            actions: [
                Kirigami.Action {
                    text: i18n("Retry")
                    icon.name: "view-refresh"
                    onTriggered: page.model.retry()
                }
            ]
        }

        MessageBox {
            Layout.margins: Kirigami.Units.largeSpacing
            visible: page.model.pageError && page.model.status !== LibraryItemsModel.Failed
            type: Kirigami.MessageType.Warning
            text: i18n("Some items could not be loaded")
            actions: [
                Kirigami.Action {
                    text: i18n("Retry")
                    icon.name: "view-refresh"
                    onTriggered: page.model.retry()
                }
            ]
        }
    }

    // The grid takes the keys as soon as the page shows
    function focusPrimary() {
        grid.forceActiveFocus(Qt.TabFocusReason);
    }

    GridView {
        id: grid
        objectName: "libraryGrid"

        onCountChanged: page.applyPendingPosition()

        // Poster tiles of about ten grid units, at least two to a row
        readonly property int columns: Math.max(2, Math.floor(width / (Kirigami.Units.gridUnit * 10)))

        model: page.model
        cellWidth: Math.floor(width / columns)
        // A 2:3 poster inside the tile margins, then the name and the year
        cellHeight: Math.round((cellWidth - Kirigami.Units.smallSpacing * 2) * 1.5) + Kirigami.Units.gridUnit * 3.5
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

        Kirigami.LoadingPlaceholder {
            anchors.centerIn: parent
            visible: page.model.status === LibraryItemsModel.Loading && grid.count === 0
            text: i18n("Loading…")
        }

        Kirigami.PlaceholderMessage {
            anchors.centerIn: parent
            width: parent.width - Kirigami.Units.gridUnit * 4
            visible: page.model.status === LibraryItemsModel.Ready && page.model.totalCount === 0
            icon.name: "folder-video"
            text: i18n("No items")
            explanation: {
                if (page.model.filtered)
                    return i18n("No item matches the search or the selected filters");
                switch (page.tabType) {
                case "movies":
                    return i18n("There are no movies yet");
                case "tvshows":
                    return i18n("There are no TV shows yet");
                }
                return i18n("This library has no items yet");
            }
        }

        delegate: MediaCard {
            width: grid.cellWidth
            height: grid.cellHeight
            // The grid is the one tab stop, the arrow keys move inside it
            focusPolicy: Qt.NoFocus
            highlighted: GridView.isCurrentItem && grid.activeFocus
            onClicked: page.Window.window.openItem(itemId, page)
        }
    }
}
