// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Kirigami.Page {
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
    // The header modes of the window: Compact hides the header box (and this page's own field takes its place), Wide is where the toolbar shows its labels
    readonly property bool compactShell: Window.window ? Window.window.compactHeader : false
    readonly property bool wideShell: Window.window ? Window.window.wideHeader : true

    // The sub-tabs exist for the page of a tab only: `subtab` is the id of the one showing ("main" for the tile page)
    readonly property bool hasSubtabs: tabType !== ""
    readonly property LibrarySubtabController subtabs: connection.subtabs
    readonly property string subtab: hasSubtabs ? subtabs.current : "main"
    // The list the toolbar, the filters and the view act on: the model of the tab for the main sub-tab, the list of the sub-tab for Favorites, Collections, Episodes and a network's series, nothing for the others
    readonly property LibraryItemsModel listModel: subtab === "main" ? model : subtabs.list
    // Whether the Genres sub-tab is on: the rails are its, and the toolbar action that opens them as a page of their own makes way
    readonly property bool genresSubtab: hasSubtabs && subtabs.available.indexOf("genres") >= 0

    title: tabType === "movies" ? i18n("Movies") : tabType === "tvshows" ? i18n("Shows") : libraryName
    padding: 0
    // No title row: the page of a tab is named by the tab, a library opened from a tile by its own row (the way back and the name), and the title stays for the accessible name
    globalToolBarStyle: Kirigami.ApplicationHeaderStyle.None
    Accessible.role: Accessible.Pane
    Accessible.name: title

    // Whether the page shows all it has at once: the toolbar lays its buttons out only once every one is built, the list comes a few frames after its first page (a list still loading shows its spinner); the shell fades a new tab in once this holds
    readonly property bool contentReady: (!toolbarRow.visible || toolbar.implicitHeight > 0) && (listModel === null || (listContent.item?.settled ?? false))

    // The scroll position and the current tile, for the tab to come back as it was left
    function viewPosition() {
        return listContent.item ? listContent.item.viewPosition() : { currentIndex: 0, contentY: 0 };
    }

    // Applied once the view has rows: the tab may still be loading its first page
    function restoreViewPosition(position) {
        listContent.item?.restoreViewPosition(position);
    }

    // The tab's search field takes the keyboard
    function focusSearchField() {
        searchRow.item?.focusSearchField();
    }

    // The view takes the keys as soon as the page shows
    function focusPrimary() {
        if (listContent.item)
            listContent.item.focusView();
        else if (otherContent.item?.openCurrent)
            otherContent.item.forceActiveFocus(Qt.TabFocusReason);
    }

    // Retry of what failed, keeping the selection: the main list asks again itself, a sub-tab through the controller
    function retryList() {
        if (subtab === "main")
            model.retry();
        else
            subtabs.retry();
    }

    // What the main menu's Refresh calls while this page is in front: the list or the sub-tab showing is asked for again
    function refreshPage() {
        if (subtab === "main")
            model.reload();
        else
            subtabs.reload();
    }

    // Show All of a genre rail: the main list, from that genre alone (one edit that replaces every filter section)
    function showGenre(genreId) {
        model.filters = { genreIds: [genreId] };
        if (hasSubtabs)
            subtabs.select("main");
    }

    function subtabTitle(id) {
        switch (id) {
        case "suggestions":
            return i18nc("@title:tab sub-tab of Movies and TV Shows", "Suggestions");
        case "favorites":
            return i18nc("@title:tab sub-tab of Movies and TV Shows", "Favorites");
        case "collections":
            return i18nc("@title:tab sub-tab of Movies and TV Shows", "Collections");
        case "genres":
            return i18nc("@title:tab sub-tab of Movies and TV Shows", "Genres");
        case "upcoming":
            return i18nc("@title:tab sub-tab of TV Shows", "Upcoming");
        case "networks":
            return i18nc("@title:tab sub-tab of TV Shows", "Networks");
        case "episodes":
            return i18nc("@title:tab sub-tab of TV Shows", "Episodes");
        }
        return title;
    }

    // What the placeholder of an empty list says
    readonly property string emptyText: {
        switch (subtab) {
        case "favorites":
            return tabType === "tvshows" ? i18n("No TV show is marked as a favorite yet") : i18n("No movie is marked as a favorite yet");
        case "collections":
            return i18n("There are no collections yet");
        case "episodes":
            return i18n("There are no episodes yet");
        case "networks":
            return i18n("This network has no series");
        }
        switch (tabType) {
        case "movies":
            return i18n("There are no movies yet");
        case "tvshows":
            return i18n("There are no TV shows yet");
        }
        return i18n("This library has no items yet");
    }

    // The data of the sub-tab that is no list of the model: suggestions, upcoming episodes, networks. Genres load their own rails
    readonly property bool flatSubtab: hasSubtabs && listModel === null && subtab !== "genres"
    readonly property string flatEmptyText: {
        switch (subtab) {
        case "upcoming":
            return i18n("No episode is about to air");
        case "networks":
            return i18n("There are no networks");
        }
        return i18n("There are no suggestions yet");
    }

    // The way back from a network's series, and the count of episodes the server reported
    readonly property bool networkBackShown: hasSubtabs && subtab === "networks" && subtabs.networkId !== ""
    readonly property bool episodeCountShown: hasSubtabs && subtab === "episodes" && subtabs.episodeCount >= 0

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

    // One sort key of the menu: shown only when the list's scope offers it
    component SortAction: ChoiceAction {
        property string token
        property var list: null

        objectName: "sortKey_" + token
        visible: list !== null && list.sortKeys.indexOf(token) >= 0
        current: list !== null && list.sortBy === token
        onChosen: list.sortBy = token
    }

    // One image type of the view options menu
    component ImageAction: ChoiceAction {
        property string token
        property var list: null

        objectName: "imageType_" + token
        current: list !== null && list.imageType === token
        onChosen: list.imageType = token
    }

    // One sub-tab as a checkable entry of the toolbar, shown while the settings turn it on: the entry that is checked leads back to the main list
    component SubtabAction: ChoiceAction {
        property string subtabId

        objectName: "subtab_" + subtabId
        text: page.subtabTitle(subtabId)
        tooltip: text
        visible: page.hasSubtabs && page.subtabs.available.indexOf(subtabId) >= 0
        current: page.subtab === subtabId
        onChosen: page.subtabs.select(current ? "main" : subtabId)
    }

    FilterDialog {
        id: filterDialog

        // Only one list is open at a time: the dialog follows it, and the chips of the page read from it. A page without a list has none to filter
        library: page.listModel ?? page.model
        favoritesFixed: page.subtab === "favorites"
    }

    // Disabled while the list shows its first row; the grid then takes the keys, which the button would lose as it turns itself off
    Kirigami.Action {
        id: returnToTopAction
        objectName: "returnToTopAction"
        text: i18n("Return to Top")
        icon.name: "go-top"
        enabled: !(listContent.item?.atStart ?? true)
        onTriggered: {
            listContent.item?.returnToTop();
            listContent.item?.focusView();
        }
    }

    onListModelChanged: {
        if (listModel !== null)
            listContent.heldModel = listModel;
    }

    // A sub-tab was chosen while a search is typed in the header box: the search is the main list's
    Connections {
        target: page.model
        function onSearchTermChanged() {
            if (page.hasSubtabs && page.subtab !== "main" && page.model.searchTerm.length > 0)
                page.subtabs.select("main");
        }
    }

    // Everything in the page body, in the order of the Tab stops: the toolbar row (library, sub-tabs first), the Compact search field, chips, then the list
    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // The sub-tabs the settings turn on (the main list is none of them), then filters, sort, view and view options; what does not fit goes into the overflow menu, and below the Wide mode the buttons are icons with tooltips. A sub-tab without a list of the model leaves the sub-tab entries alone in the toolbar
        RowLayout {
            id: toolbarRow

            Layout.fillWidth: true
            Layout.leftMargin: Kirigami.Units.largeSpacing
            Layout.rightMargin: Kirigami.Units.largeSpacing
            visible: page.listModel !== null || page.hasSubtabs || page.tabType === ""
            spacing: Kirigami.Units.mediumSpacing

            // A library opened from a Home tile sits above Home: its way back and its name lead the row, as PageToolbar does on the other pages
            PageBackButton {
                visible: page.tabType === ""
            }

            PageTitle {
                objectName: "libraryNameLabel"
                visible: page.tabType === "" && text.length > 0
                text: page.libraryName
            }

            // The library of the type, while there is more than one: built only then, as a wide window with one library, the usual case, would pay its creation on every push
            Loader {
                active: page.tabType !== "" && page.libraries.length > 1
                visible: active
                // A long library name never pushes the toolbar out
                Layout.maximumWidth: Kirigami.Units.gridUnit * 16

                sourceComponent: Controls.ComboBox {
                    id: selector
                    objectName: "librarySelector"

                    // "*" is no Jellyfin id: openType takes any id that is not a library of the type as All
                    readonly property var entries: [{ id: "*", name: i18n("All libraries") }].concat(page.libraries)
                    function scopeIndex() {
                        const index = entries.findIndex(entry => entry.id === page.model.libraryId);
                        return index < 0 ? 0 : index;
                    }

                    model: entries
                    textRole: "name"
                    currentIndex: scopeIndex()
                    Accessible.name: i18nc("@label:listbox", "Library")
                    onActivated: index => {
                        page.connection.openType(page.tabType, entries[index].id);
                        currentIndex = Qt.binding(() => selector.scopeIndex());
                    }

                    // Library names come from the server: plain text only (the style paints the selected name itself, as plain text; a custom contentItem breaks it, which reads text input API)
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

            // The toolbar is the page's top row: the way back from a network's series sits in it, not on a row of its own
            Controls.ToolButton {
                objectName: "networkBackButton"
                visible: page.networkBackShown
                icon.name: "go-previous"
                text: i18n("All Networks")
                onClicked: page.subtabs.closeNetwork()
            }

            // The network open, so the series below are not anonymous: server text, plain only
            Controls.Label {
                objectName: "networkNameLabel"
                Layout.maximumWidth: Kirigami.Units.gridUnit * 12
                visible: page.networkBackShown && text.length > 0
                elide: Text.ElideRight
                font.weight: Font.DemiBold
                textFormat: Text.PlainText
                text: page.subtabs?.networkName ?? ""
                // A long name is cut: the whole one shows under the pointer
                Controls.ToolTip.text: text
                Controls.ToolTip.visible: truncated && networkNameHover.hovered
                Controls.ToolTip.delay: Kirigami.Units.toolTipDelay

                HoverHandler {
                    id: networkNameHover
                }
            }

            Kirigami.ActionToolBar {
                id: toolbar
                objectName: "libraryToolbar"

                Layout.fillWidth: true
                display: page.wideShell ? Controls.AbstractButton.TextBesideIcon : Controls.AbstractButton.IconOnly

                actions: [
                    SubtabAction {
                        subtabId: "suggestions"
                        icon.name: "starred-symbolic"
                    },
                    SubtabAction {
                        subtabId: "favorites"
                        icon.name: "bookmarks"
                    },
                    SubtabAction {
                        subtabId: "collections"
                        icon.name: "view-catalog"
                    },
                    SubtabAction {
                        subtabId: "upcoming"
                        icon.name: "view-calendar-upcoming-events"
                    },
                    SubtabAction {
                        subtabId: "genres"
                        icon.name: "tag"
                    },
                    SubtabAction {
                        subtabId: "networks"
                        icon.name: "globe"
                    },
                    SubtabAction {
                        subtabId: "episodes"
                        icon.name: "format-list-ordered"
                    },
                    Kirigami.Action {
                        separator: true
                        visible: page.hasSubtabs && page.subtabs.available.length > 1 && page.listModel !== null
                    },
                    Kirigami.Action {
                        objectName: "filtersAction"
                        text: i18n("Filters")
                        tooltip: text
                        icon.name: "view-filter"
                        visible: page.listModel !== null
                        onTriggered: filterDialog.open()
                    },
                    Kirigami.Action {
                        objectName: "sortAction"
                        text: i18n("Sort")
                        tooltip: text
                        icon.name: "view-sort"
                        visible: page.listModel !== null

                        SortAction {
                            token: "SortName"
                            list: page.listModel
                            text: i18n("Name")
                        }
                        SortAction {
                            token: "AiredEpisodeOrder"
                            list: page.listModel
                            text: i18n("Episode Order")
                        }
                        SortAction {
                            token: "Random"
                            list: page.listModel
                            text: i18n("Random")
                        }
                        SortAction {
                            token: "CommunityRating"
                            list: page.listModel
                            text: i18n("Community Rating")
                        }
                        SortAction {
                            token: "CriticRating"
                            list: page.listModel
                            text: i18n("Critic Rating")
                        }
                        SortAction {
                            token: "DateCreated"
                            list: page.listModel
                            text: i18n("Date Added")
                        }
                        SortAction {
                            token: "DateLastContentAdded"
                            list: page.listModel
                            text: i18n("Date Episode Added")
                        }
                        SortAction {
                            token: "DatePlayed"
                            list: page.listModel
                            text: i18n("Date Played")
                        }
                        SortAction {
                            token: "SeriesDatePlayed"
                            list: page.listModel
                            text: i18n("Date Played")
                        }
                        SortAction {
                            token: "OfficialRating"
                            list: page.listModel
                            text: i18n("Parental Rating")
                        }
                        SortAction {
                            token: "PlayCount"
                            list: page.listModel
                            text: i18n("Play Count")
                        }
                        SortAction {
                            token: "PremiereDate"
                            list: page.listModel
                            text: i18n("Release Date")
                        }
                        SortAction {
                            token: "Runtime"
                            list: page.listModel
                            text: i18n("Runtime")
                        }
                        Kirigami.Action {
                            separator: true
                        }
                        // The direction of a random order means nothing
                        ChoiceAction {
                            objectName: "sortAscending"
                            text: i18n("Ascending Order")
                            enabled: page.listModel !== null && !page.listModel.random
                            current: page.listModel !== null && !page.listModel.descending
                            onChosen: page.listModel.descending = false
                        }
                        ChoiceAction {
                            objectName: "sortDescending"
                            text: i18n("Descending Order")
                            enabled: page.listModel !== null && !page.listModel.random
                            current: page.listModel !== null && page.listModel.descending
                            onChosen: page.listModel.descending = true
                        }
                    },
                    Kirigami.Action {
                        objectName: "reshuffleAction"
                        text: i18n("Reshuffle")
                        tooltip: text
                        icon.name: "randomize"
                        visible: page.listModel !== null && page.listModel.random
                        onTriggered: page.listModel.reshuffle()
                    },
                    Kirigami.Action {
                        objectName: "viewModeAction"
                        readonly property bool listMode: page.listModel !== null && page.listModel.viewMode === "list"
                        text: listMode ? i18n("Grid View") : i18n("List View")
                        tooltip: text
                        icon.name: listMode ? "view-list-icons" : "view-list-details"
                        visible: page.listModel !== null
                        onTriggered: page.listModel.viewMode = listMode ? "grid" : "list"
                    },
                    Kirigami.Action {
                        objectName: "viewOptionsAction"
                        text: i18n("View Options")
                        tooltip: text
                        icon.name: "configure"
                        visible: page.listModel !== null

                        ImageAction {
                            token: "Primary"
                            list: page.listModel
                            text: i18nc("@item:inmenu type of image shown on the cards", "Poster")
                        }
                        ImageAction {
                            token: "Banner"
                            list: page.listModel
                            text: i18nc("@item:inmenu type of image shown on the cards", "Banner")
                        }
                        ImageAction {
                            token: "Disc"
                            list: page.listModel
                            text: i18nc("@item:inmenu type of image shown on the cards", "Disc")
                        }
                        ImageAction {
                            token: "Logo"
                            list: page.listModel
                            text: i18nc("@item:inmenu type of image shown on the cards", "Logo")
                        }
                        ImageAction {
                            token: "Thumb"
                            list: page.listModel
                            text: i18nc("@item:inmenu type of image shown on the cards", "Thumbnail")
                        }
                        Kirigami.Action {
                            separator: true
                        }
                        ChoiceAction {
                            objectName: "showTitleAction"
                            text: i18n("Show Title")
                            current: page.listModel !== null && page.listModel.showTitle
                            onChosen: page.listModel.showTitle = !page.listModel.showTitle
                        }
                        ChoiceAction {
                            objectName: "showYearAction"
                            text: i18n("Show Year")
                            current: page.listModel !== null && page.listModel.showYear
                            onChosen: page.listModel.showYear = !page.listModel.showYear
                        }
                        ChoiceAction {
                            objectName: "cardLayoutAction"
                            text: i18n("Card Layout")
                            current: page.listModel !== null && page.listModel.cardLayout
                            onChosen: page.listModel.cardLayout = !page.listModel.cardLayout
                        }
                    },
                    Kirigami.Action {
                        objectName: "browseGenresAction"
                        text: i18n("Browse by Genre")
                        tooltip: text
                        icon.name: "tag"
                        visible: page.listModel !== null && page.subtab === "main" && !page.genresSubtab && page.listModel.filterOptions.available
                        onTriggered: page.Window.window.openGenres(page)
                    }
                ]
            }

            Controls.Label {
                objectName: "episodeCountLabel"
                visible: page.episodeCountShown
                opacity: 0.7
                textFormat: Text.PlainText
                text: i18np("%1 episode", "%1 episodes", page.subtabs.episodeCount)
            }

            // At the right end, apart from the controls of the view, and never in the toolbar's overflow menu
            Controls.ToolButton {
                objectName: "returnToTopButton"
                visible: page.listModel !== null
                action: returnToTopAction
                display: page.wideShell ? Controls.AbstractButton.TextBesideIcon : Controls.AbstractButton.IconOnly
                Controls.ToolTip.text: text
                Controls.ToolTip.visible: (hovered || visualFocus) && display === Controls.AbstractButton.IconOnly
                Controls.ToolTip.delay: Kirigami.Units.toolTipDelay
            }
        }

        // A Compact window has no search box in the header: the tab's own field sits under the toolbar, built only then
        Loader {
            id: searchRow

            Layout.fillWidth: true
            Layout.maximumWidth: Kirigami.Units.gridUnit * 30
            Layout.leftMargin: Kirigami.Units.largeSpacing
            Layout.rightMargin: Kirigami.Units.largeSpacing
            Layout.topMargin: Kirigami.Units.smallSpacing
            Layout.bottomMargin: Kirigami.Units.smallSpacing
            active: page.tabType !== "" && page.compactShell
            visible: active

            sourceComponent: Kirigami.SearchField {
                id: tabSearch
                objectName: "librarySearchField"

                function focusSearchField() {
                    tabSearch.forceActiveFocus(Qt.TabFocusReason);
                    tabSearch.selectAll();
                }

                // The shell owns Ctrl+F: the field's own sequence would make it ambiguous
                focusSequences: []
                maximumLength: 128
                autoAccept: false
                placeholderText: page.tabType === "movies" ? i18nc("@info:placeholder", "Search in Movies") : i18nc("@info:placeholder", "Search in Shows")

                // The field is the source of truth while it holds the focus, and follows the term the header box changes
                onTextChanged: {
                    if (text !== page.model.searchTerm)
                        page.model.searchTerm = text;
                }
                Keys.onDownPressed: page.focusPrimary()
                Component.onCompleted: text = page.model.searchTerm

                Connections {
                    target: page.model
                    function onSearchTermChanged() {
                        if (tabSearch.text !== page.model.searchTerm)
                            tabSearch.text = page.model.searchTerm;
                    }
                }
            }
        }

        // The filters of the list as chips with a remove button each, and Clear Filters for all of them (not the search, the sort or the library)
        Flow {
            id: chipBar
            objectName: "filterChips"

            Layout.fillWidth: true
            Layout.leftMargin: Kirigami.Units.largeSpacing
            Layout.rightMargin: Kirigami.Units.largeSpacing
            Layout.topMargin: Kirigami.Units.smallSpacing
            visible: page.listModel !== null && (filterDialog.chips.length > 0 || page.listModel.resolvingFacets)
            spacing: Kirigami.Units.smallSpacing

            Repeater {
                model: filterDialog.chips

                delegate: Rectangle {
                    id: chip

                    required property var modelData
                    required property int index

                    objectName: "filterChip_" + index
                    implicitWidth: chipRow.implicitWidth + Kirigami.Units.smallSpacing * 2
                    implicitHeight: chipRow.implicitHeight + Kirigami.Units.smallSpacing
                    radius: height / 2
                    color: Kirigami.Theme.alternateBackgroundColor
                    border.width: 1
                    border.color: Qt.alpha(Kirigami.Theme.textColor, 0.2)

                    RowLayout {
                        id: chipRow
                        anchors.centerIn: parent
                        spacing: 0

                        // Genres, tags and ratings come from the server: plain text only
                        Controls.Label {
                            Layout.leftMargin: Kirigami.Units.smallSpacing
                            Layout.maximumWidth: Kirigami.Units.gridUnit * 14
                            elide: Text.ElideRight
                            textFormat: Text.PlainText
                            text: chip.modelData.label
                        }

                        // The label is server text for some chips: only the accessible name carries it, never the control text
                        Controls.ToolButton {
                            objectName: "filterChipRemove_" + chip.index
                            Layout.preferredWidth: Kirigami.Units.iconSizes.smallMedium + Kirigami.Units.smallSpacing
                            Layout.preferredHeight: Kirigami.Units.iconSizes.smallMedium + Kirigami.Units.smallSpacing
                            padding: 0
                            display: Controls.AbstractButton.IconOnly
                            icon.name: "window-close-symbolic"
                            icon.width: Kirigami.Units.iconSizes.small
                            icon.height: Kirigami.Units.iconSizes.small
                            Accessible.name: i18nc("accessible name of the button that removes one filter, %1 is the filter", "Remove the filter %1", chip.modelData.label)
                            Controls.ToolTip.text: i18n("Remove this filter")
                            Controls.ToolTip.visible: hovered
                            Controls.ToolTip.delay: Kirigami.Units.toolTipDelay
                            onClicked: filterDialog.removeChip(chip.modelData)
                        }
                    }
                }
            }

            Controls.Label {
                objectName: "resolvingFiltersLabel"
                visible: page.listModel !== null && page.listModel.resolvingFacets
                opacity: 0.7
                text: i18n("Applying the saved filters…")
            }

            Controls.Button {
                objectName: "clearFiltersButton"
                flat: true
                icon.name: "edit-clear-all"
                text: i18n("Clear Filters")
                onClicked: page.listModel.clearFilters()
            }
        }

        MessageBox {
            id: libraryFailedBox
            objectName: "libraryFailedBox"
            Layout.margins: Kirigami.Units.largeSpacing
            visible: page.listModel !== null && page.listModel.status === LibraryItemsModel.Failed
            type: Kirigami.MessageType.Error
            readonly property bool tooLong: page.listModel !== null && page.listModel.queryTooLong
            text: tooLong ? i18n("The filters and search are too long for the server: clear the filters or shorten the search") : i18n("Could not load this library: check the connection to the server and try again")
            actions: [
                Kirigami.Action {
                    visible: !libraryFailedBox.tooLong
                    text: i18n("Retry")
                    icon.name: "view-refresh"
                    onTriggered: page.retryList()
                },
                Kirigami.Action {
                    objectName: "libraryFailedClearFilters"
                    visible: libraryFailedBox.tooLong
                    text: i18n("Clear Filters")
                    icon.name: "edit-clear-all"
                    onTriggered: page.listModel.clearFilters()
                }
            ]
        }

        MessageBox {
            Layout.fillWidth: true
            Layout.topMargin: Kirigami.Units.largeSpacing
            visible: page.listModel !== null && page.listModel.pageError && page.listModel.status !== LibraryItemsModel.Failed
            type: Kirigami.MessageType.Warning
            text: i18n("Some items could not be loaded")
            actions: [
                Kirigami.Action {
                    text: i18n("Retry")
                    icon.name: "view-refresh"
                    onTriggered: page.retryList()
                }
            ]
        }

        MessageBox {
            objectName: "subtabFailedBox"
            Layout.margins: Kirigami.Units.largeSpacing
            visible: page.flatSubtab && page.subtabs.status === LibrarySubtabController.Failed
            type: Kirigami.MessageType.Error
            text: i18n("Could not load this section: check the connection to the server and try again")
            actions: [
                Kirigami.Action {
                    text: i18n("Retry")
                    icon.name: "view-refresh"
                    onTriggered: page.subtabs.retry()
                }
            ]
        }

        // What the list sub-tabs and the others show
        Item {
            id: contentArea

            Layout.fillWidth: true
            Layout.fillHeight: true

            // Another sub-tab or a network's series fade in instead of standing there at once
            // On the GUI thread: a render-thread animator can leave a stale frame when its item is hidden mid-fade
            NumberAnimation {
                id: contentFade
                property: "opacity"
                target: contentArea
                from: 0
                to: 1
                duration: Kirigami.Units.longDuration
                easing.type: Easing.OutCubic
            }
            // A network opened or left keeps the Networks sub-tab: the controller's own change covers both
            Connections {
                target: page.subtabs
                enabled: page.hasSubtabs
                function onCurrentChanged() {
                    contentFade.restart();
                }
            }

            // The list of the main sub-tab and of the list sub-tabs: the grid or the list of items
            Loader {
                id: listContent
                objectName: "listContent"

                // The last list there was: the content never sees the null of a sub-tab that has no list while it goes away
                property LibraryItemsModel heldModel: page.model

                anchors.fill: parent
                active: page.listModel !== null

                sourceComponent: LibraryContent {
                    listModel: listContent.heldModel
                    ownerPage: page
                    accessibleName: page.title
                    emptyText: page.emptyText
                    canClear: filterDialog.chips.length > 0 || listContent.heldModel.searchTerm.length > 0 || listContent.heldModel.resolvingFacets
                    onClearRequested: {
                        listContent.heldModel.clearFilters();
                        listContent.heldModel.searchTerm = "";
                    }
                    // A model of another list shows from its first item
                    onListModelChanged: {
                        if (ready)
                            Qt.callLater(scrollToStart);
                    }
                }
            }

            // The sub-tabs that are no list of the model
            Loader {
                id: otherContent
                objectName: "otherContent"

                anchors.fill: parent
                active: page.hasSubtabs && page.listModel === null
                sourceComponent: {
                    switch (page.subtab) {
                    case "suggestions":
                        return page.tabType === "movies" ? recommendationsComponent : suggestionsComponent;
                    case "genres":
                        return genresComponent;
                    case "upcoming":
                        return upcomingComponent;
                    case "networks":
                        return networksComponent;
                    }
                    return null;
                }
            }

            Kirigami.LoadingPlaceholder {
                FadeInOnShow {}
                objectName: "subtabLoading"
                anchors.centerIn: parent
                visible: page.flatSubtab && page.subtabs.status === LibrarySubtabController.Loading
                text: i18n("Loading…")
            }

            Kirigami.PlaceholderMessage {
                FadeInOnShow {}
                objectName: "subtabEmpty"
                anchors.centerIn: parent
                width: parent.width - Kirigami.Units.gridUnit * 4
                visible: page.flatSubtab && page.subtabs.empty
                icon.name: "folder-video"
                text: i18n("No items")
                explanation: page.flatEmptyText
            }
        }
    }

    Component {
        id: recommendationsComponent

        Controls.ScrollView {
            ListView {
                id: rows
                objectName: "recommendationRows"

                model: page.subtabs.recommendations
                spacing: Kirigami.Units.largeSpacing
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                Accessible.role: Accessible.List
                Accessible.name: page.title

                delegate: ColumnLayout {
                    id: rail
                    objectName: "recommendationRow_" + index

                    required property int index
                    required property int kind
                    required property string baselineName
                    required property var items

                    // The poster ratios match MediaCard: the height is the poster plus the two text lines
                    readonly property real cardWidth: Kirigami.Units.gridUnit * 8
                    readonly property real cardHeight: Math.round((cardWidth - Kirigami.Units.smallSpacing * 2) * 3 / 2) + Kirigami.Units.gridUnit * 3.5
                    // The wording stays translatable, the name of the film, director or actor is the server's
                    readonly property string title: {
                        switch (kind) {
                        case RecommendationRowsModel.SimilarToRecentlyPlayed:
                            return i18n("Because you watched %1", baselineName);
                        case RecommendationRowsModel.SimilarToLikedItem:
                            return i18n("Because you liked %1", baselineName);
                        case RecommendationRowsModel.HasDirectorFromRecentlyPlayed:
                            return i18n("Because you watched movies directed by %1", baselineName);
                        case RecommendationRowsModel.HasActorFromRecentlyPlayed:
                            return i18n("Because you watched movies with %1", baselineName);
                        case RecommendationRowsModel.HasLikedDirector:
                            return i18n("Because you like movies directed by %1", baselineName);
                        case RecommendationRowsModel.HasLikedActor:
                            return i18n("Because you like movies with %1", baselineName);
                        }
                        return i18n("Suggested for you");
                    }

                    width: ListView.view ? ListView.view.width : implicitWidth
                    spacing: Kirigami.Units.smallSpacing

                    Kirigami.Heading {
                        Layout.fillWidth: true
                        Layout.leftMargin: Kirigami.Units.largeSpacing
                        level: 2
                        // The names come from the server: plain text only
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                        Accessible.role: Accessible.Heading
                        text: rail.title
                    }

                    CardRow {
                        listObjectName: "recommendationRowList_" + rail.index
                        model: rail.items
                        cardHeight: rail.cardHeight
                        edgeMargin: Kirigami.Units.largeSpacing
                        accessibleName: rail.title
                        pageFlickable: rows

                        delegate: MediaCard {
                            width: rail.cardWidth
                            height: rail.cardHeight
                            // The list is the one tab stop of its row
                            focusPolicy: Qt.NoFocus
                            loaded: true
                            highlighted: ListView.isCurrentItem && ListView.view.activeFocus
                            onClicked: page.Window.window.openItem(itemId, page)
                        }
                    }
                }
            }
        }
    }

    Component {
        id: suggestionsComponent

        ItemRowGrid {
            objectName: "suggestionsGrid"

            model: page.subtabs.suggestions
            accessibleName: page.title
            onActivated: itemId => page.Window.window.openItem(itemId, page)
        }
    }

    Component {
        id: upcomingComponent

        ItemRowGrid {
            objectName: "upcomingGrid"

            model: page.subtabs.upcoming
            landscape: true
            accessibleName: page.title
            onActivated: itemId => page.Window.window.openItem(itemId, page)
        }
    }

    Component {
        id: networksComponent

        ItemRowGrid {
            objectName: "networksGrid"

            model: page.subtabs.networks
            landscape: true
            hasMenu: false
            accessibleName: page.title
            onActivated: itemId => page.subtabs.selectNetwork(itemId)
        }
    }

    Component {
        id: genresComponent

        Controls.ScrollView {
            GenreRailsView {
                connection: page.connection
                ownerPage: page
                accessibleName: page.title
                onShowAllRequested: genreId => page.showGenre(genreId)
            }
        }
    }
}
