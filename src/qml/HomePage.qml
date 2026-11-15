// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Kirigami.ScrollablePage {
    id: page
    objectName: "homePage"

    required property ConnectionController connection

    // The row toolbar is ToolBar everywhere (Main.qml): this page keeps its identity and actions in the top bar instead
    globalToolBarStyle: Kirigami.ApplicationHeaderStyle.None

    title: i18n("Home")

    // The scroll position, for the tab to come back as it was left
    function viewPosition() {
        return { currentIndex: 0, contentY: page.flickable.contentY };
    }
    function restoreViewPosition(position) {
        // The page may be gone by the time the call runs, when the session ends right after the tab opens
        Qt.callLater(() => {
            const flickable = page?.flickable;
            if (flickable)
                flickable.contentY = Math.min(position.contentY, Math.max(0, flickable.contentHeight - flickable.height));
        });
    }

    readonly property bool showLibraries: page.connection.homeLibraries.length > 0 && (page.Window.window?.homeLibrariesEnabled ?? true)

    ColumnLayout {
        id: homeColumn
        spacing: Kirigami.Units.largeSpacing

        MessageBox {
            visible: page.connection.errorText.length > 0
            type: Kirigami.MessageType.Error
            text: page.connection.errorText
            actions: [
                Kirigami.Action {
                    text: i18n("Retry")
                    icon.name: "view-refresh"
                    onTriggered: page.connection.reloadLibraries()
                }
            ]
        }

        Kirigami.PlaceholderMessage {
            Layout.fillWidth: true
            visible: page.connection.librariesLoaded && page.connection.libraries.length === 0 && page.connection.errorText.length === 0
            text: i18n("No libraries to show")
            icon.name: "folder-video"
        }

        // The tiles follow the user's preferences (order and hidden libraries) and the Show on Home setting; the placeholder above is about the libraries themselves, so hiding every tile leaves the rows without it
        Kirigami.Heading {
            objectName: "homeLibrariesHeading"
            visible: page.showLibraries
            level: 2
            text: i18n("Libraries")
        }

        Flow {
            objectName: "homeLibraries"
            Layout.fillWidth: true
            visible: page.showLibraries
            spacing: Kirigami.Units.largeSpacing
            Repeater {
                // No tile is built while the section is off, so no image is fetched for it
                model: page.showLibraries ? page.connection.homeLibraries : []
                delegate: LibraryTile {
                    id: tile
                    required property var modelData
                    // From the page's column, not from the Flow: a Flow child sized by the Flow's own width never settles under the software renderer
                    width: Math.min(Kirigami.Units.gridUnit * 16, homeColumn.width)
                    library: tile.modelData
                    // A movies or TV shows library opens its tab on that library, the server's Playlists view the Playlists page; the other types open above Home
                    onClicked: {
                        if (tile.library.tab)
                            page.Window.window.openTab(tile.library.tab, tile.library.id);
                        else if (tile.library.collectionType === "playlists")
                            page.Window.window.openPlaylists();
                        else
                            page.Window.window.openLibrary(tile.library.id, tile.library.name);
                    }
                }
            }
        }

        Repeater {
            model: page.connection.homeRows
            delegate: ColumnLayout {
                id: row
                objectName: "homeRow_" + index

                required property int index
                required property int kind
                required property string libraryId
                required property string libraryName
                required property int status
                required property var items
                required property bool landscape
                required property bool shown

                // The poster ratios match MediaCard: the height is the poster plus the two text lines
                readonly property real cardWidth: Kirigami.Units.gridUnit * (landscape ? 14 : 8)
                readonly property real cardHeight: Math.round((cardWidth - Kirigami.Units.smallSpacing * 2) * (landscape ? 9 / 16 : 3 / 2)) + Kirigami.Units.gridUnit * 3.5
                readonly property string title: {
                    switch (kind) {
                    case HomeRowsModel.ContinueWatching:
                        return i18n("Continue Watching");
                    case HomeRowsModel.NextUp:
                        return i18n("Next Up");
                    case HomeRowsModel.Favorites:
                        return i18n("Favorites");
                    default:
                        return i18n("Recently Added in %1", libraryName);
                    }
                }

                // The type tab a Recently Added row lists in full, empty for the other rows
                readonly property string seeAllTab: kind === HomeRowsModel.RecentlyAdded ? (page.connection.libraries.find(entry => entry.id === libraryId)?.tab ?? "") : ""
                // Continue Watching, Favorites and Recently Added have a list of their own, Next Up has none
                readonly property bool hasSeeAll: kind === HomeRowsModel.ContinueWatching || kind === HomeRowsModel.Favorites || seeAllTab !== ""

                function seeAll() {
                    if (kind === HomeRowsModel.ContinueWatching)
                        page.Window.window.openResumeItems();
                    else if (kind === HomeRowsModel.Favorites)
                        page.Window.window.openFavorites();
                    else
                        page.Window.window.openRecentlyAdded(seeAllTab, libraryId);
                }

                Layout.fillWidth: true
                visible: shown
                spacing: Kirigami.Units.smallSpacing

                RowLayout {
                    Layout.fillWidth: true

                    Kirigami.Heading {
                        Layout.fillWidth: true
                        level: 2
                        // Library names come from the server: plain text only
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                        text: row.title
                    }

                    Controls.Button {
                        objectName: "homeRowSeeAll_" + row.index
                        visible: row.hasSeeAll && row.status === HomeRowsModel.Ready
                        text: i18n("See All")
                        icon.name: "go-next"
                        Accessible.name: i18n("See All in %1", row.title)
                        onClicked: row.seeAll()
                    }
                }

                Item {
                    Layout.fillWidth: true
                    Layout.preferredHeight: Kirigami.Units.gridUnit * 4
                    visible: row.status === HomeRowsModel.Loading

                    Controls.BusyIndicator {
                        anchors.centerIn: parent
                        running: parent.visible
                        Accessible.name: i18n("Loading…")
                    }
                }

                MessageBox {
                    visible: row.status === HomeRowsModel.Failed
                    type: Kirigami.MessageType.Error
                    text: i18n("Could not load this row: check the connection to the server and try again")
                }

                // A row retry button, not a MessageBox action: Kirigami's action toolbar fails when a row flips between states
                Controls.Button {
                    visible: row.status === HomeRowsModel.Failed
                    Layout.alignment: Qt.AlignLeft
                    text: i18n("Retry")
                    icon.name: "view-refresh"
                    onClicked: page.connection.homeRows.retry(row.index)
                }

                CardRow {
                    visible: row.status === HomeRowsModel.Ready
                    listObjectName: "homeRowList_" + row.index
                    model: row.items
                    cardHeight: row.cardHeight
                    accessibleName: row.title
                    pageFlickable: page.flickable

                    delegate: MediaCard {
                        required property int seasonNumber
                        required property int episodeNumber
                        required property string episodeTitle

                        width: row.cardWidth
                        height: row.cardHeight
                        // The list is the one tab stop of its row
                        focusPolicy: Qt.NoFocus
                        landscape: row.landscape
                        loaded: true
                        highlighted: ListView.isCurrentItem && ListView.view.activeFocus
                        onClicked: page.Window.window.openItem(itemId, page)
                        quickPlay: row.kind === HomeRowsModel.ContinueWatching
                        onQuickPlayRequested: page.Window.window.playItem(itemId, resumeTicks)
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
        }
    }
}
