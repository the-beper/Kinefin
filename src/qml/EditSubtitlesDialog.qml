// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// The external subtitles of one item: upload a local srt, ass, ssa or vtt file or delete one. Embedded tracks are read-only here
Kirigami.Dialog {
    id: dialog
    objectName: "editSubtitlesDialog"

    required property ConnectionController connection
    required property ItemDetailsModel details
    // The window shows the notifications
    required property var shell

    readonly property var subtitles: details.mediaSources.length > 0 ? details.mediaSources[0].subtitles : []
    property int deleteIndex: -1
    // Empty means undetermined; the client refuses anything but two or three letters
    readonly property bool languageValid: languageField.length === 0 || languageField.acceptableInput

    // One size in every state: the tracks take the room the upload leaves, and the dialog must not move when a track goes or a warning shows. The room the window leaves is a limit too, and the opening animation starts the dialog two grid units lower, so it must not reach the window edge while it moves
    readonly property real totalHeight: Math.min(Kirigami.Units.gridUnit * 28, absoluteMaximumHeight - Kirigami.Units.gridUnit * 4)
    readonly property real bodyHeight: Math.max(Kirigami.Units.gridUnit * 8, totalHeight - implicitHeaderHeight - implicitFooterHeight)

    title: i18n("Edit Subtitles")
    preferredWidth: Kirigami.Units.gridUnit * 30
    standardButtons: Kirigami.Dialog.Cancel
    customFooterActions: [
        Kirigami.Action {
            objectName: "subtitleUpload"
            text: i18n("Upload…")
            icon.name: "document-open"
            enabled: dialog.languageValid
            onTriggered: subtitleDialog.open()
        }
    ]

    header: DialogTitleBar {
        title: dialog.title
        onCloseRequested: dialog.close()
    }

    function confirmDelete(track) {
        // One dialog at a time: the confirmation takes the edit dialog's place
        dialog.close();
        deleteIndex = track.index;
        deleteDialog.open();
    }

    function removeSubtitle() {
        deleteDialog.close();
        if (!dialog.connection.deleteSubtitle(dialog.details.itemId, deleteIndex))
            dialog.shell.showPassiveNotification(i18n("The subtitle could not be deleted: wait for the changes on their way and try again"));
    }

    function trackLabel(track) {
        if (track.title.length > 0)
            return track.title;
        if (track.language.length > 0)
            return track.language;
        return i18nc("@label subtitle track", "Subtitle %1", track.index);
    }

    // The title of a track is the server's own description (language, codec and flags): only a track without one gets its details written out
    function trackText(track) {
        if (track.title.length > 0)
            return track.title;
        const parts = [trackLabel(track)];
        if (track.codec.length > 0)
            parts.push(track.codec);
        parts.push(track.isExternal ? i18n("External") : i18n("Embedded"));
        if (track.isForced)
            parts.push(i18n("Forced"));
        return parts.join(" · ");
    }

    Connections {
        target: dialog.connection
        function onItemSubtitleUpdated(itemId) {
            if (itemId !== dialog.details.itemId)
                return;
            dialog.details.reload();
            dialog.shell.showPassiveNotification(i18n("Subtitles updated"));
        }
        function onItemSubtitleUpdateFailed(itemId, status) {
            if (itemId !== dialog.details.itemId)
                return;
            dialog.shell.showPassiveNotification(i18n("The subtitle could not be changed: check the file and the connection to the server"));
        }
    }

    FileDialog {
        id: subtitleDialog
        objectName: "editSubtitleFileDialog"
        title: i18n("Choose a Subtitle")
        nameFilters: [i18n("Subtitles (*.srt *.ass *.ssa *.vtt)")]
        onAccepted: {
            // The formats of JellyfinClient::uploadSubtitle, which answers false alike for an empty, oversized, unreadable or non-UTF-8 file, an upload already on its way and a lost session
            if (!/\.(srt|ass|ssa|vtt)$/i.test(selectedFile.toString()))
                dialog.shell.showPassiveNotification(i18n("The file is not an srt, ass, ssa, or vtt subtitle"));
            else if (!dialog.connection.uploadSubtitle(dialog.details.itemId, selectedFile,
                                                       languageField.text, forcedSwitch.checked))
                dialog.shell.showPassiveNotification(i18n("The subtitle could not be uploaded: check the file (not empty, under 2 MiB, in UTF-8) and wait for any upload on its way"));
        }
    }

    Kirigami.PromptDialog {
        id: deleteDialog
        objectName: "deleteSubtitleDialog"

        title: i18n("Delete Subtitle")
        standardButtons: Kirigami.Dialog.Cancel
        customFooterActions: [
            Kirigami.Action {
                objectName: "confirmDeleteSubtitle"
                text: i18n("Delete")
                icon.name: "edit-delete"
                onTriggered: dialog.removeSubtitle()
            }
        ]

        Controls.Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
            text: i18n("Delete this external subtitle? The file is removed from the server and cannot be recovered")
        }
    }

    Item {
        implicitHeight: dialog.bodyHeight

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            // The tracks, in the room the upload under them leaves
            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true

                ListView {
                    id: list
                    objectName: "editSubtitlesList"

                    // The room of the vertical scroll bar, so that it does not cover the Delete button of a row
                    readonly property real barRoom: bar.size < 1 ? bar.implicitWidth : 0

                    anchors.fill: parent
                    visible: list.count > 0
                    clip: true
                    model: dialog.subtitles
                    activeFocusOnTab: true
                    Accessible.role: Accessible.List
                    Accessible.name: i18n("Subtitles")
                    Controls.ScrollBar.vertical: Controls.ScrollBar {
                        id: bar
                    }

                    delegate: Controls.ItemDelegate {
                        id: row

                        required property int index
                        required property var modelData

                        objectName: "subtitleRow_" + index
                        width: list.width - list.barRoom
                        focusPolicy: Qt.NoFocus

                        PlainToolTip {
                            visible: row.hovered && trackLabelText.truncated
                            text: trackLabelText.text
                        }

                        contentItem: RowLayout {
                            Controls.Label {
                                id: trackLabelText
                                Layout.fillWidth: true
                                elide: Text.ElideRight
                                maximumLineCount: 1
                                textFormat: Text.PlainText
                                text: dialog.trackText(row.modelData)
                            }
                            Controls.Button {
                                objectName: "subtitleDelete_" + row.modelData.index
                                visible: row.modelData.isExternal && row.modelData.index >= 0
                                text: i18n("Delete…")
                                icon.name: "edit-delete"
                                display: Controls.AbstractButton.IconOnly
                                Accessible.name: i18nc("@action:button", "Delete subtitle %1", dialog.trackLabel(row.modelData))
                                Controls.ToolTip.visible: hovered || visualFocus
                                Controls.ToolTip.text: text
                                Controls.ToolTip.delay: Kirigami.Units.toolTipDelay
                                onClicked: dialog.confirmDelete(row.modelData)
                            }
                        }
                    }
                }

                Kirigami.PlaceholderMessage {
                    anchors.centerIn: parent
                    width: parent.width - Kirigami.Units.gridUnit * 2
                    visible: list.count === 0
                    text: i18nc("@info:placeholder", "No subtitles")
                }
            }

            Kirigami.Separator {
                Layout.fillWidth: true
            }

            // What the next upload carries: the footer's Upload… asks for the file
            ColumnLayout {
                Layout.fillWidth: true
                Layout.margins: Kirigami.Units.largeSpacing * 2
                spacing: Kirigami.Units.largeSpacing

                Controls.Label {
                    Layout.fillWidth: true
                    opacity: 0.7
                    wrapMode: Text.Wrap
                    textFormat: Text.PlainText
                    text: i18n("srt, ass, ssa, or vtt, UTF-8, at most 2 MiB")
                }

                GridLayout {
                    Layout.fillWidth: true
                    columns: 2
                    columnSpacing: Kirigami.Units.largeSpacing
                    rowSpacing: Kirigami.Units.smallSpacing

                    Controls.Label {
                        Layout.columnSpan: 2
                        text: i18nc("@label:textbox", "Language:")
                    }
                    Controls.TextField {
                        id: languageField
                        objectName: "subtitleLanguage"
                        Layout.fillWidth: true
                        placeholderText: i18n("eng")
                        maximumLength: 3
                        validator: RegularExpressionValidator {
                            regularExpression: /[A-Za-z]{2,3}/
                        }
                        Accessible.name: i18n("Language")
                        Accessible.description: i18n("Language code of two or three letters, empty for undetermined")
                    }
                    Controls.CheckBox {
                        id: forcedSwitch
                        objectName: "subtitleForced"
                        text: i18n("Forced")
                    }
                }

                MessageBox {
                    objectName: "subtitleLanguageInvalid"
                    Layout.fillWidth: true
                    Layout.topMargin: Kirigami.Units.largeSpacing
                    visible: !dialog.languageValid && (languageField.length >= 2 || !languageField.activeFocus)
                    type: Kirigami.MessageType.Warning
                    text: i18n("Use a language code of two or three letters, or leave the field empty")
                }
            }
        }
    }
}
