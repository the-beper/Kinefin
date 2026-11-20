// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Kirigami.ScrollablePage {
    id: page
    objectName: "searchPage"

    required property ConnectionController connection

    readonly property int searchState: connection.search.state
    // The groups show only while there is something to show or wait for: a stale group never outlives an empty or too short term
    readonly property bool groupsVisible: searchState === SearchModel.Searching || searchState === SearchModel.Results
    // The header box hides in Compact mode, and this page's own field takes its place
    readonly property bool compactShell: Window.window ? Window.window.compactHeader : false

    title: i18n("Search")
    // A section of the header, as the queue: no title row, the header's box names it
    globalToolBarStyle: Kirigami.ApplicationHeaderStyle.None
    // Without Kirigami's title row the page names itself to a screen reader
    Accessible.role: Accessible.Pane
    Accessible.name: title

    // The drawer action owns the shortcut and calls this after the page shows
    function focusSearchField() {
        field.forceActiveFocus();
        field.selectAll();
    }

    // Down from the field goes to the first row that has cards
    function focusFirstResults() {
        for (let i = 0; i < resultRows.count; ++i) {
            const row = resultRows.itemAt(i);
            if (!row?.visible || !row.hasCards)
                continue;
            row.cards.focusList(Qt.TabFocusReason);
            return;
        }
    }

    header: ColumnLayout {
        spacing: 0

        Kirigami.SearchField {
            id: field
            objectName: "searchField"
            // The header owns the box on a wide window: here it shows only in Compact mode, where the header has none
            visible: page.compactShell
            Layout.fillWidth: true
            Layout.maximumWidth: Kirigami.Units.gridUnit * 30
            Layout.margins: Kirigami.Units.largeSpacing
            // The shell owns Ctrl+F: the field's own sequence would make it ambiguous
            focusSequences: []
            maximumLength: 128
            autoAccept: false

            // The field is the source of truth while it holds the focus, and follows the term the header box changes
            onTextChanged: {
                if (text !== page.connection.search.term)
                    page.connection.search.term = text;
            }
            onAccepted: page.connection.search.searchNow()
            Keys.onDownPressed: page.focusFirstResults()
            Component.onCompleted: text = page.connection.search.term
        }

        Connections {
            target: page.connection.search
            function onTermChanged() {
                if (field.text !== page.connection.search.term)
                    field.text = page.connection.search.term;
            }
        }
    }

    ColumnLayout {
        spacing: Kirigami.Units.largeSpacing

        MessageBox {
            visible: page.searchState === SearchModel.AllFailed
            type: Kirigami.MessageType.Error
            text: i18n("Could not load these results: check the connection to the server and try again")
            actions: [
                Kirigami.Action {
                    text: i18n("Retry")
                    icon.name: "view-refresh"
                    onTriggered: page.connection.search.searchNow()
                }
            ]
        }

        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: Math.max(Kirigami.Units.gridUnit * 8, (page.flickable ? page.flickable.height : page.availableHeight) - Kirigami.Units.gridUnit * 2)
            visible: page.searchState === SearchModel.Empty || page.searchState === SearchModel.TooShort || page.searchState === SearchModel.NoResults

            Kirigami.PlaceholderMessage {
                FadeInOnShow {}
                anchors.centerIn: parent
                width: parent.width - Kirigami.Units.largeSpacing * 4
                text: {
                    switch (page.searchState) {
                    case SearchModel.TooShort:
                        return i18n("Keep typing");
                    case SearchModel.NoResults:
                        return i18n("No results");
                    default:
                        return i18n("Search your libraries");
                    }
                }
                explanation: {
                    switch (page.searchState) {
                    case SearchModel.TooShort:
                        return i18np("Type at least %1 character", "Type at least %1 characters", page.connection.search.minTermLength);
                    case SearchModel.NoResults:
                        return i18n("Nothing in your libraries matches");
                    default:
                        return i18n("Type the name of a movie, series or episode");
                    }
                }
                icon.name: page.searchState === SearchModel.NoResults ? "edit-none" : "system-search"
            }
        }

        Repeater {
            id: resultRows
            model: page.connection.search
            delegate: ColumnLayout {
                id: row
                objectName: "searchGroup_" + index

                required property int index
                required property int kind
                required property int status
                required property var items
                required property bool landscape
                required property bool shown

                readonly property bool hasCards: items.count > 0
                readonly property Item cards: cardRow
                // The poster ratios match MediaCard: the height is the poster plus the two text lines
                readonly property real cardWidth: Kirigami.Units.gridUnit * (landscape ? 14 : 8)
                readonly property real cardHeight: Math.round((cardWidth - Kirigami.Units.smallSpacing * 2) * (landscape ? 9 / 16 : 3 / 2)) + Kirigami.Units.gridUnit * 3.5
                readonly property string title: {
                    switch (kind) {
                    case SearchModel.Movies:
                        return i18n("Movies");
                    case SearchModel.Series:
                        return i18n("Series");
                    case SearchModel.Episodes:
                        return i18n("Episodes");
                    default:
                        return i18n("Collections");
                    }
                }

                Layout.fillWidth: true
                visible: shown && page.groupsVisible
                spacing: Kirigami.Units.smallSpacing

                Kirigami.Heading {
                    Layout.fillWidth: true
                    level: 2
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                    Accessible.role: Accessible.Heading
                    text: row.title
                }

                Item {
                    Layout.fillWidth: true
                    Layout.preferredHeight: Kirigami.Units.gridUnit * 4
                    visible: row.status === SearchModel.Loading && !row.hasCards

                    Controls.BusyIndicator {
                        FadeInOnShow {}
                        anchors.centerIn: parent
                        running: parent.visible
                        Accessible.name: i18n("Loading…")
                    }
                }

                MessageBox {
                    visible: row.status === SearchModel.Failed
                    type: Kirigami.MessageType.Error
                    text: i18n("Could not load these results: check the connection to the server and try again")
                    actions: [
                        Kirigami.Action {
                            text: i18n("Retry")
                            icon.name: "view-refresh"
                            onTriggered: page.connection.search.retry(row.index)
                        }
                    ]
                }

                CardRow {
                    id: cardRow
                    visible: row.hasCards
                    listObjectName: "searchRow_" + row.index
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
                }
            }
        }
    }
}
