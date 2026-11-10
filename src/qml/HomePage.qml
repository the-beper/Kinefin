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
            visible: page.connection.libraries.length === 0
            text: i18n("No libraries to show")
            icon.name: "folder-video"
        }

        Kirigami.Heading {
            visible: page.connection.libraries.length > 0
            level: 2
            text: i18n("Libraries")
        }

        Kirigami.CardsLayout {
            Layout.fillWidth: true
            visible: page.connection.libraries.length > 0
            Repeater {
                model: page.connection.libraries
                delegate: Kirigami.AbstractCard {
                    id: card
                    required property var modelData
                    objectName: "libraryCard_" + modelData.id
                    enabled: card.modelData.browsable
                    showClickFeedback: true
                    Accessible.name: card.modelData.name
                    Accessible.description: card.modelData.browsable ? "" : i18n("Not supported yet")
                    onClicked: {
                        const stack = page.Window.window.pageStack;
                        // A library already open above Home gives way to the new one: pop it before the shared model switches to the new library
                        stack.pop(page);
                        if (!page.connection.openLibrary(card.modelData.id))
                            return;
                        stack.push(Qt.resolvedUrl("LibraryPage.qml"), { model: page.connection.library, libraryName: card.modelData.name });
                    }
                    contentItem: ColumnLayout {
                        spacing: Kirigami.Units.smallSpacing

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: width * 9 / 16
                            color: Kirigami.Theme.alternateBackgroundColor
                            clip: true

                            Kirigami.Icon {
                                anchors.centerIn: parent
                                width: Kirigami.Units.iconSizes.huge
                                height: width
                                source: "folder-video"
                                visible: image.status !== Image.Ready
                            }

                            Image {
                                id: image
                                anchors.fill: parent
                                asynchronous: true
                                fillMode: Image.PreserveAspectCrop
                                sourceSize.width: width * Screen.devicePixelRatio
                                // Only URLs built by JellyfinClient::imageUrl belong here: the image fetcher allows nothing else, but a raw server string must never become a source
                                source: card.modelData.imageUrl
                            }
                        }

                        Controls.Label {
                            Layout.fillWidth: true
                            // Library names come from the server: plain text only
                            textFormat: Text.PlainText
                            elide: Text.ElideRight
                            text: card.modelData.name
                        }

                        Controls.Label {
                            Layout.fillWidth: true
                            visible: !card.modelData.browsable
                            opacity: 0.7
                            textFormat: Text.PlainText
                            text: i18n("Not supported yet")
                        }
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

                Layout.fillWidth: true
                visible: shown
                spacing: Kirigami.Units.smallSpacing

                Kirigami.Heading {
                    Layout.fillWidth: true
                    level: 2
                    // Library names come from the server: plain text only
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                    text: row.title
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
