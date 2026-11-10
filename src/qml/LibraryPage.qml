// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Kirigami.ScrollablePage {
    id: page
    objectName: "libraryPage"

    required property LibraryItemsModel model
    // The name comes from the server: the page title renders it as plain text
    property string libraryName: ""

    title: libraryName
    // Take the width Home leaves instead of a fixed column
    Kirigami.ColumnView.fillWidth: true

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
        }
    ]

    header: ColumnLayout {
        spacing: 0

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

    GridView {
        id: grid
        objectName: "libraryGrid"

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
        Accessible.name: page.libraryName

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
            explanation: page.model.filter === LibraryItemsModel.All ? i18n("This library has no items yet") : i18n("No item matches the selected filter")
        }

        delegate: MediaCard {
            width: grid.cellWidth
            height: grid.cellHeight
            // The grid is the one tab stop, the arrow keys move inside it
            focusPolicy: Qt.NoFocus
            highlighted: GridView.isCurrentItem && grid.activeFocus
        }
    }
}
