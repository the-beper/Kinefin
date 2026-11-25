// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// The search of the libraries, in a popup near the top of the window: the field above the results in four groups (movies, series, episodes, collections). The shell opens it (Ctrl+F, the rail's Search entry, the Compact header's button); a result opens its details page above the page in front and closes the popup. While a new term is searched the previous results stay, and a thin progress bar under the field shows only for a slow search
Controls.Popup {
    id: popup
    objectName: "searchPopup"

    required property ConnectionController connection

    // A result was chosen: the shell opens its details page
    signal itemChosen(string itemId)
    // The control that had the keyboard before the popup opened is gone: the shell gives the keyboard to the page in front
    signal focusLost

    readonly property int searchState: connection.search.state
    readonly property bool searching: searchState === SearchModel.Searching
    // The groups show only while there is something to show or wait for: a stale group never outlives an empty or too short term
    readonly property bool groupsVisible: searching || searchState === SearchModel.Results
    // A search shorter than this shows no progress bar
    readonly property int slowSearchMs: 300
    property bool slowSearch: false
    // The state the placeholder shows: the one the last finished search left, so that a new term makes it neither blink nor jump
    property int settledState: SearchModel.Empty
    readonly property int placeholderState: searching ? settledState : searchState
    readonly property bool placeholderShown: placeholderState === SearchModel.Empty || placeholderState === SearchModel.TooShort || placeholderState === SearchModel.NoResults
    readonly property real maximumHeight: parent ? parent.height * 0.8 : 0
    // The height of the empty states, which every state keeps at least, so that the popup does not resize at each keystroke
    readonly property real minimumBodyHeight: Kirigami.Units.gridUnit * 10
    // The room of the results and their scroll bar around the content
    readonly property real bodyMargin: Kirigami.Units.largeSpacing

    // The control that had the keyboard when the popup opened, which gets it back when the popup closes without a choice
    property Item focusBefore: null
    // A result closed the popup: the page it opens takes the keyboard
    property bool chosen: false

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

    function choose(itemId: string): void {
        chosen = true;
        close();
        itemChosen(itemId);
    }

    parent: Controls.Overlay.overlay
    modal: true
    focus: true
    closePolicy: Controls.Popup.CloseOnEscape | Controls.Popup.CloseOnPressOutside
    // The border takes one pixel of the popup, the content the rest
    padding: 1
    width: Math.max(0, Math.min((parent?.width ?? 0) - Kirigami.Units.gridUnit * 2, Kirigami.Units.gridUnit * 52))
    height: Math.min(implicitHeight, maximumHeight)
    x: Math.round(((parent?.width ?? 0) - width) / 2)
    y: Math.round((parent?.height ?? 0) * 0.1)


    onSearchingChanged: {
        if (!searching)
            slowSearch = false;
    }
    onSearchStateChanged: {
        if (!searching)
            settledState = searchState;
    }
    Component.onCompleted: settledState = searchState
    onAboutToShow: {
        const win = parent ? parent.Window.window : null;
        focusBefore = win ? win.activeFocusItem : null;
    }
    onOpened: focusSearchField()
    onClosed: {
        const target = focusBefore;
        focusBefore = null;
        if (chosen) {
            chosen = false;
            return;
        }
        if (target?.visible && target.enabled && target.Window.window)
            target.forceActiveFocus(Qt.TabFocusReason);
        else
            focusLost();
    }

    background: Rectangle {
        color: Kirigami.Theme.backgroundColor
        radius: Kirigami.Units.cornerRadius
        border.width: 1
        border.color: Qt.alpha(Kirigami.Theme.textColor, 0.3)
    }

    contentItem: ColumnLayout {
        // A popup has no Accessible attached property: its content names it
        Accessible.role: Accessible.Dialog
        Accessible.name: i18n("Search")
        spacing: 0

        // The session ends under an open popup: it closes with it
        Connections {
            target: popup.connection
            function onStateChanged() {
                if (popup.connection.state !== ConnectionController.Authenticated)
                    popup.close();
            }
        }

        // The progress bar shows only once a search has lasted `slowSearchMs`
        Timer {
            interval: popup.slowSearchMs
            running: popup.searching && popup.visible
            onTriggered: popup.slowSearch = true
        }

        Kirigami.SearchField {
            id: field
            objectName: "searchField"
            Layout.fillWidth: true
            Layout.margins: Kirigami.Units.largeSpacing
            focus: true
            // The shell owns Ctrl+F: the field's own sequence would make it ambiguous
            focusSequences: []
            maximumLength: 128
            autoAccept: false
            placeholderText: i18nc("@info:placeholder", "Search")

            // The field is the source of truth while it holds the focus, and follows the term of the search
            onTextChanged: {
                if (text !== popup.connection.search.term)
                    popup.connection.search.term = text;
            }
            onAccepted: popup.connection.search.searchNow()
            Keys.onDownPressed: popup.focusFirstResults()
            Component.onCompleted: text = popup.connection.search.term
        }

        Connections {
            target: popup.connection.search
            function onTermChanged() {
                if (field.text !== popup.connection.search.term)
                    field.text = popup.connection.search.term;
            }
        }

        // The line under the field: the progress bar of a slow search takes its place
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: Kirigami.Units.smallSpacing
            clip: true

            Kirigami.Separator {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                visible: !progress.visible
                Accessible.ignored: true
            }

            Controls.ProgressBar {
                id: progress
                objectName: "searchProgress"
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                padding: 0
                indeterminate: true
                visible: popup.slowSearch && popup.searching
                Accessible.name: i18n("Loading…")
            }
        }

        Flickable {
            id: results
            objectName: "searchResults"
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.preferredHeight: Math.max(column.implicitHeight + popup.bodyMargin * 2, popup.minimumBodyHeight)
            contentWidth: width
            contentHeight: column.implicitHeight + popup.bodyMargin * 2
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            Controls.ScrollBar.vertical: Controls.ScrollBar {}

            ColumnLayout {
                id: column
                x: popup.bodyMargin
                y: popup.bodyMargin
                width: results.width - popup.bodyMargin * 2
                spacing: Kirigami.Units.largeSpacing

                MessageBox {
                    visible: popup.searchState === SearchModel.AllFailed
                    type: Kirigami.MessageType.Error
                    text: i18n("Could not load these results: check the connection to the server and try again")
                    actions: [
                        Kirigami.Action {
                            text: i18n("Retry")
                            icon.name: "view-refresh"
                            onTriggered: popup.connection.search.searchNow()
                        }
                    ]
                }

                Item {
                    Layout.fillWidth: true
                    Layout.preferredHeight: popup.minimumBodyHeight - popup.bodyMargin * 2
                    visible: popup.placeholderShown

                    Kirigami.PlaceholderMessage {
                        FadeInOnShow {}
                        anchors.centerIn: parent
                        width: parent.width - Kirigami.Units.largeSpacing * 4
                        text: {
                            switch (popup.placeholderState) {
                            case SearchModel.TooShort:
                                return i18n("Keep typing");
                            case SearchModel.NoResults:
                                return i18n("No results");
                            default:
                                return i18n("Search your libraries");
                            }
                        }
                        explanation: {
                            switch (popup.placeholderState) {
                            case SearchModel.TooShort:
                                return i18np("Type at least %1 character", "Type at least %1 characters", popup.connection.search.minTermLength);
                            case SearchModel.NoResults:
                                return i18n("Nothing in your libraries matches");
                            default:
                                return i18n("Type the name of a movie, series, or episode");
                            }
                        }
                        icon.name: popup.placeholderState === SearchModel.NoResults ? "edit-none" : "system-search"
                    }
                }

                Repeater {
                    id: resultRows
                    model: popup.connection.search
                    delegate: ColumnLayout {
                        id: row
                        objectName: "searchGroup_" + index

                        required property int index
                        required property int kind
                        required property int status
                        required property var items
                        required property bool landscape

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
                        // A group still loading shows its previous cards, or nothing: no title and no spinner flash while a term is typed
                        visible: (hasCards || status === SearchModel.Failed) && popup.groupsVisible
                        spacing: Kirigami.Units.smallSpacing

                        Kirigami.Heading {
                            Layout.fillWidth: true
                            level: 2
                            textFormat: Text.PlainText
                            elide: Text.ElideRight
                            Accessible.role: Accessible.Heading
                            text: row.title
                        }

                        MessageBox {
                            visible: row.status === SearchModel.Failed
                            type: Kirigami.MessageType.Error
                            text: i18n("Could not load these results: check the connection to the server and try again")
                            actions: [
                                Kirigami.Action {
                                    text: i18n("Retry")
                                    icon.name: "view-refresh"
                                    onTriggered: popup.connection.search.retry(row.index)
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
                            pageFlickable: results

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
                                onClicked: popup.choose(itemId)
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
    }
}
