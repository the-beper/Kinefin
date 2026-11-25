// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// OpenSubtitles through the server's provider plugin: search the item playing and download a result; on success the player reopens at the same position so the track is in the source
Kirigami.Dialog {
    id: dialog
    objectName: "subtitleSearchDialog"

    required property var player

    // A search ran since the dialog opened: before the first answer the empty state would flash over the fade-in
    property bool searched: false
    // Set when the download request could not even start, so the dialog stays open with the reason
    property bool downloadFailed: false
    // A download started: the other results stay inert until the dialog closes
    property bool downloading: false
    // The item the results belong to; another item replacing it makes them stale
    readonly property string itemId: dialog.player?.itemId ?? ""
    property string openedItemId: ""
    onItemIdChanged: {
        if (dialog.opened && dialog.itemId !== dialog.openedItemId)
            dialog.close();
    }

    // Alphabetical by the translated name
    readonly property var languages: [
        { text: i18n("English"), value: "eng" },
        { text: i18n("French"), value: "fra" },
        { text: i18n("German"), value: "deu" },
        { text: i18n("Italian"), value: "ita" },
        { text: i18n("Spanish"), value: "spa" }
    ].sort((a, b) => a.text.localeCompare(b.text))

    // One size in every state: the results take the room the search leaves, and the dialog must not move while it searches or when the answer comes. The room the window leaves is a limit too, and the opening animation starts the dialog two grid units lower, so it must not reach the window edge while it moves
    readonly property real totalHeight: Math.min(Kirigami.Units.gridUnit * 28, absoluteMaximumHeight - Kirigami.Units.gridUnit * 4)
    readonly property real bodyHeight: Math.max(Kirigami.Units.gridUnit * 8, totalHeight - implicitHeaderHeight - implicitFooterHeight)

    title: i18nc("@title:window", "Search Online Subtitles")
    preferredWidth: Kirigami.Units.gridUnit * 30
    standardButtons: Kirigami.Dialog.Close

    header: DialogTitleBar {
        title: dialog.title
        onCloseRequested: dialog.close()
    }

    function errorText() {
        if (!dialog.player)
            return "";
        if (dialog.downloadFailed)
            return i18n("The subtitle could not be downloaded. Try again");
        switch (dialog.player.remoteSubtitleError) {
        case "noprovider":
            return i18n("No online subtitle provider is installed on the server. Ask the server administrator to install one");
        case "network":
            return i18n("Could not reach the server. Check the connection and try again");
        case "failed":
            return i18n("The search failed. Try again");
        }
        return "";
    }

    function search() {
        if (!dialog.player)
            return;
        dialog.downloadFailed = false;
        dialog.searched = true;
        dialog.player.searchRemoteSubtitles(languageBox.currentValue);
    }

    // Before the fade-in, so the empty state never covers it
    onAboutToShow: {
        dialog.searched = false;
        dialog.downloadFailed = false;
        dialog.downloading = false;
        dialog.openedItemId = dialog.itemId;
        dialog.search();
    }
    onOpened: languageBox.forceActiveFocus(Qt.PopupFocusReason)

    Item {
        implicitHeight: dialog.bodyHeight

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            ColumnLayout {
                Layout.fillWidth: true
                Layout.margins: Kirigami.Units.largeSpacing
                spacing: Kirigami.Units.largeSpacing

                RowLayout {
                    Layout.fillWidth: true
                    spacing: Kirigami.Units.largeSpacing
                    Controls.ComboBox {
                        id: languageBox
                        objectName: "subtitleLanguageBox"
                        Layout.fillWidth: true
                        enabled: dialog.player ? !dialog.player.remoteSubtitleSearching : false
                        textRole: "text"
                        valueRole: "value"
                        Accessible.name: i18nc("@label:listbox", "Subtitle language")
                        model: dialog.languages
                        onActivated: dialog.search()
                        // The list opens below the box and above it when there is no room below, never over it
                        PopupPlacement { popup: languageBox.popup; anchor: languageBox }
                    }
                    Controls.Button {
                        objectName: "subtitleSearchButton"
                        text: i18nc("@action:button", "Search")
                        icon.name: "search"
                        enabled: dialog.player ? !dialog.player.remoteSubtitleSearching : false
                        onClicked: dialog.search()
                    }
                }
                MessageBox {
                    objectName: "subtitleSearchError"
                    Layout.fillWidth: true
                    visible: dialog.errorText().length > 0
                    type: Kirigami.MessageType.Error
                    text: dialog.errorText()
                }
            }

            Kirigami.Separator {
                Layout.fillWidth: true
            }

            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true

                ListView {
                    id: results
                    objectName: "subtitleResults"

                    // The room of the vertical scroll bar, so that it does not cover the Download button of a row
                    readonly property real barRoom: bar.size < 1 ? bar.implicitWidth : 0

                    anchors.fill: parent
                    visible: count > 0
                    clip: true
                    model: dialog.player ? dialog.player.remoteSubtitles : []
                    Accessible.role: Accessible.List
                    Accessible.name: i18nc("@title:group", "Subtitle results")
                    Controls.ScrollBar.vertical: Controls.ScrollBar {
                        id: bar
                    }
                    delegate: Item {
                        id: result
                        required property var modelData
                        width: results.width - results.barRoom
                        implicitHeight: resultRow.implicitHeight + Kirigami.Units.smallSpacing * 2

                        RowLayout {
                            id: resultRow
                            anchors {
                                left: parent.left
                                right: parent.right
                                leftMargin: Kirigami.Units.largeSpacing
                                rightMargin: Kirigami.Units.largeSpacing
                                verticalCenter: parent.verticalCenter
                            }
                            spacing: Kirigami.Units.largeSpacing

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 0

                                Controls.Label {
                                    Layout.fillWidth: true
                                    textFormat: Text.PlainText
                                    elide: Text.ElideRight
                                    text: modelData.name
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: Kirigami.Units.smallSpacing
                                    Controls.Label {
                                        Layout.fillWidth: true
                                        textFormat: Text.PlainText
                                        elide: Text.ElideRight
                                        color: Kirigami.Theme.disabledTextColor
                                        font: Kirigami.Theme.smallFont
                                        text: i18nc("@info subtitle provider and format", "%1 · %2", modelData.provider, modelData.format)
                                    }
                                    Controls.Label {
                                        textFormat: Text.PlainText
                                        color: Kirigami.Theme.disabledTextColor
                                        font: Kirigami.Theme.smallFont
                                        text: i18ncp("@item:info subtitle download count", "%1 download", "%1 downloads", modelData.downloads)
                                    }
                                }
                            }
                            Controls.Button {
                                objectName: "subtitleResultDownloadButton"
                                text: i18n("Download")
                                icon.name: "download"
                                enabled: !dialog.downloading
                                Accessible.name: i18nc("@action:button %1 is the subtitle", "Download %1", modelData.name)
                                onClicked: {
                                    if (!dialog.player)
                                        return;
                                    dialog.downloading = true;
                                    if (dialog.player.downloadRemoteSubtitle(modelData.id))
                                        dialog.close();
                                    else {
                                        dialog.downloading = false;
                                        dialog.downloadFailed = true;
                                    }
                                }
                            }
                        }
                    }
                }

                Controls.BusyIndicator {
                    objectName: "subtitleSearchBusy"
                    anchors.centerIn: parent
                    running: dialog.player ? dialog.player.remoteSubtitleSearching : false
                    visible: running
                    Accessible.name: i18n("Searching")
                    FadeInOnShow {}
                }
                Kirigami.PlaceholderMessage {
                    objectName: "subtitleSearchEmpty"
                    anchors.centerIn: parent
                    width: parent.width - Kirigami.Units.gridUnit * 2
                    visible: dialog.searched && !dialog.player?.remoteSubtitleSearching
                             && dialog.errorText().length === 0
                             && dialog.player?.remoteSubtitles.length === 0
                    icon.name: "edit-find"
                    text: i18n("No subtitles found")
                    explanation: i18n("Try another language")
                }
            }

            Kirigami.Separator {
                Layout.fillWidth: true
            }
        }
    }
}
