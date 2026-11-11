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

    title: i18n("Home")

    actions: [
        Kirigami.Action {
            text: i18n("Refresh")
            icon.name: "view-refresh"
            // The details pages bind the same key: only the page in front answers it, the button stays usable
            shortcut: page.isCurrentPage ? StandardKey.Refresh : ""
            // The libraries come first: their answer reloads the rows
            onTriggered: page.connection.reloadLibraries()
        }
    ]

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

    ColumnLayout {
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

        // The tiles follow the user's preferences (order and hidden libraries); the placeholder above is about the libraries themselves, so hiding every tile leaves the rows without it
        Kirigami.Heading {
            visible: page.connection.homeLibraries.length > 0
            level: 2
            text: i18n("Libraries")
        }

        Kirigami.CardsLayout {
            Layout.fillWidth: true
            visible: page.connection.homeLibraries.length > 0
            Repeater {
                model: page.connection.homeLibraries
                delegate: LibraryTile {
                    id: tile
                    required property var modelData
                    library: tile.modelData
                    // A movies or TV shows library opens its tab on that library; the other types open above Home
                    onClicked: tile.library.tab ? page.Window.window.openTab(tile.library.tab, tile.library.id) : page.Window.window.openLibrary(tile.library.id, tile.library.name)
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
                    default:
                        return i18n("Recently Added in %1", libraryName);
                    }
                }

                // The type tab a Recently Added row lists in full, empty for the other rows
                readonly property string seeAllTab: kind === HomeRowsModel.RecentlyAdded ? (page.connection.libraries.find(entry => entry.id === libraryId)?.tab ?? "") : ""
                // Continue Watching and Recently Added have a list of their own, Next Up has none
                readonly property bool hasSeeAll: kind === HomeRowsModel.ContinueWatching || seeAllTab !== ""

                function seeAll() {
                    if (kind === HomeRowsModel.ContinueWatching)
                        page.Window.window.openResumeItems();
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
                    actions: [
                        Kirigami.Action {
                            text: i18n("Retry")
                            icon.name: "view-refresh"
                            onTriggered: page.connection.homeRows.retry(row.index)
                        }
                    ]
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
