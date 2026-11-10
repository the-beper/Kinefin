// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Kirigami.ScrollablePage {
    id: page
    objectName: "genreRailsPage"

    required property ConnectionController connection

    readonly property var rails: connection.genreRails

    Kirigami.ColumnView.fillWidth: true
    title: i18n("Genres")

    // The library page that opened this one: its grid shows the genre chosen with Show All
    function libraryPageBelow() {
        const stack = page.Window.window.pageStack;
        for (let i = 0; i < stack.depth; ++i) {
            if (stack.get(i)?.objectName === "libraryPage")
                return stack.get(i);
        }
        return null;
    }

    // Moves the focus to the nearest rail with cards above or below `from`: the rails around it may not exist yet, so the view scrolls to each one before it looks
    function focusNeighbour(from, step) {
        for (let i = from + step; i >= 0 && i < list.count; i += step) {
            list.currentIndex = i;
            list.positionViewAtIndex(i, ListView.Contain);
            const rail = list.itemAtIndex(i);
            // A rail still loading is where the focus goes next: stop and let a second key press move on
            if (rail?.shown && (rail.railStatus === GenreRailsModel.RailIdle || rail.railStatus === GenreRailsModel.RailLoading))
                return;
            if (rail?.hasCards) {
                rail.focusRow();
                return;
            }
        }
        if (step > 0)
            loadMoreWhenAtEnd();
    }

    function loadMoreWhenAtEnd() {
        if (list.atYEnd && rails.canLoadMore)
            rails.loadMore();
    }

    // The end of a short list is reached without a scroll, so a new page or a state change asks again
    Connections {
        target: page.rails
        function onStatusChanged() {
            Qt.callLater(page.loadMoreWhenAtEnd);
        }
    }

    ListView {
        id: list
        objectName: "genreRailsList"

        model: page.rails
        spacing: Kirigami.Units.largeSpacing
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        currentIndex: 0
        Accessible.role: Accessible.List
        Accessible.name: page.title

        onAtYEndChanged: page.loadMoreWhenAtEnd()
        onContentHeightChanged: Qt.callLater(page.loadMoreWhenAtEnd)

        Kirigami.LoadingPlaceholder {
            anchors.centerIn: parent
            visible: page.rails.status === GenreRailsModel.Loading && list.count === 0
            text: i18n("Loading…")
        }

        // A failed page of genres sits after the rails already loaded, where the user reached the end
        footer: Item {
            width: list.width
            height: failure.visible ? failure.implicitHeight + Kirigami.Units.largeSpacing * 2 : 0

            MessageBox {
                id: failure
                x: Kirigami.Units.largeSpacing
                y: Kirigami.Units.largeSpacing
                width: parent.width - Kirigami.Units.largeSpacing * 2
                visible: page.rails.status === GenreRailsModel.Failed
                type: Kirigami.MessageType.Error
                text: i18n("Could not load the genres: check the connection to the server and try again")
                actions: [
                    Kirigami.Action {
                        text: i18n("Retry")
                        icon.name: "view-refresh"
                        onTriggered: page.rails.retryList()
                    }
                ]
            }
        }

        Kirigami.PlaceholderMessage {
            anchors.centerIn: parent
            width: list.width - Kirigami.Units.gridUnit * 4
            visible: page.rails.status === GenreRailsModel.Ready && list.count === 0
            icon.name: "view-list-details"
            text: i18n("No genres")
            explanation: i18n("This library has no genres")
        }

        delegate: ColumnLayout {
            id: row
            objectName: "genreRail_" + index

            required property int index
            required property string genreId
            required property string name
            required property int railStatus
            required property var items
            required property bool shown

            // The poster ratios match MediaCard: the height is the poster plus the two text lines
            readonly property bool landscape: items.landscape
            readonly property real cardWidth: Kirigami.Units.gridUnit * (landscape ? 14 : 8)
            readonly property real cardHeight: Math.round((cardWidth - Kirigami.Units.smallSpacing * 2) * (landscape ? 9 / 16 : 3 / 2)) + Kirigami.Units.gridUnit * 3.5
            readonly property bool hasCards: shown && railStatus === GenreRailsModel.RailReady && items.count > 0

            function focusRow() {
                cardRow.focusList(Qt.TabFocusReason);
            }

            // A hidden rail takes no room in the list
            width: ListView.view.width
            height: shown ? implicitHeight : 0
            visible: shown
            spacing: Kirigami.Units.smallSpacing

            Component.onCompleted: page.rails.want(genreId)
            Component.onDestruction: page.rails.unwant(genreId)

            RowLayout {
                Layout.fillWidth: true

                Kirigami.Heading {
                    Layout.fillWidth: true
                    level: 2
                    // Genre names come from the server: plain text only
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                    Accessible.role: Accessible.Heading
                    text: row.name
                }

                Controls.Button {
                    flat: true
                    visible: row.hasCards
                    text: i18n("Show All")
                    // The name is server text: only the accessible name carries it, never the control text
                    Accessible.name: i18nc("accessible name of the button that opens the grid of one genre, %1 is the genre", "Show All in %1", row.name)
                    onClicked: {
                        const libraryPage = page.libraryPageBelow();
                        if (!libraryPage)
                            return;
                        // The rail ignores the grid's filter: the grid starts from the genre alone
                        page.connection.library.filter = LibraryItemsModel.All;
                        page.connection.library.genreIds = [row.genreId];
                        page.Window.window.pageStack.pop(libraryPage);
                    }
                }
            }

            Item {
                Layout.fillWidth: true
                Layout.preferredHeight: Kirigami.Units.gridUnit * 4
                visible: row.railStatus === GenreRailsModel.RailIdle || row.railStatus === GenreRailsModel.RailLoading

                Controls.BusyIndicator {
                    anchors.centerIn: parent
                    running: parent.visible
                    Accessible.name: i18n("Loading…")
                }
            }

            // Created only on failure: a message box builds its action bar asynchronously, and the delegates of a long list come and go while the view scrolls
            Loader {
                Layout.fillWidth: true
                active: row.railStatus === GenreRailsModel.RailFailed
                visible: active
                sourceComponent: MessageBox {
                    type: Kirigami.MessageType.Error
                    text: i18n("Could not load this genre: check the connection to the server and try again")
                    actions: [
                        Kirigami.Action {
                            text: i18n("Retry")
                            icon.name: "view-refresh"
                            onTriggered: page.rails.retry(row.index)
                        }
                    ]
                }
            }

            CardRow {
                id: cardRow
                visible: row.railStatus === GenreRailsModel.RailReady
                listObjectName: "genreRailRow_" + row.index
                model: row.items
                cardHeight: row.cardHeight
                accessibleName: row.name
                pageFlickable: page.flickable
                externalVerticalNavigation: true
                onUpRequested: page.focusNeighbour(row.index, -1)
                onDownRequested: page.focusNeighbour(row.index, 1)

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
