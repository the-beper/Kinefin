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

    title: i18n("Edit Subtitles")
    preferredWidth: Kirigami.Units.gridUnit * 30 - Kirigami.Units.largeSpacing * 2
    maximumHeight: absoluteMaximumHeight - Kirigami.Units.gridUnit * 4
    standardButtons: Kirigami.Dialog.Cancel
    padding: Kirigami.Units.largeSpacing
    customFooterActions: [
        Kirigami.Action {
            objectName: "subtitleUpload"
            text: i18n("Upload…")
            icon.name: "document-open"
            enabled: dialog.languageValid
            onTriggered: subtitleDialog.open()
        }
    ]

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

    function trackDetail(track) {
        const parts = [];
        if (track.language.length > 0)
            parts.push(track.language);
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

    ColumnLayout {
        Layout.fillWidth: true

        Controls.Label {
            Layout.fillWidth: true
            opacity: 0.7
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
            text: i18n("srt, ass, ssa, or vtt, UTF-8, at most 2 MiB")
        }

        ListView {
            id: list
            objectName: "editSubtitlesList"
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(contentHeight, Kirigami.Units.gridUnit * 12)
            visible: list.count > 0
            clip: true
            model: dialog.subtitles
            activeFocusOnTab: true
            Accessible.role: Accessible.List
            Accessible.name: i18n("Subtitles")

            delegate: Controls.ItemDelegate {
                id: row

                required property int index
                required property var modelData

                objectName: "subtitleRow_" + index
                width: ListView.view.width
                focusPolicy: Qt.NoFocus

                PlainToolTip {
                    visible: row.hovered && trackText.truncated
                    text: trackText.text
                }

                contentItem: RowLayout {
                    Controls.Label {
                        id: trackText
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                        maximumLineCount: 1
                        textFormat: Text.PlainText
                        text: dialog.trackLabel(row.modelData) + " (" + dialog.trackDetail(row.modelData) + ")"
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

        Kirigami.FormLayout {
            Layout.fillWidth: true

            Controls.TextField {
                id: languageField
                objectName: "subtitleLanguage"
                Kirigami.FormData.label: i18nc("@label:textbox", "Language:")
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
                Kirigami.FormData.label: i18n("Forced:")
                Accessible.name: i18n("Forced")
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
