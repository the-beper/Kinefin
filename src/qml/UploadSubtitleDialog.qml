// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// Keeps a subtitle file that was added to the playing item on the server too, as an external subtitle of the item (administrators only, the same upload as Edit Subtitles): the language and Forced are asked, the rest is the file as it is. The playback keeps the file it already shows
Kirigami.Dialog {
    id: dialog
    objectName: "uploadSubtitleDialog"

    required property ConnectionController connection
    // The window shows the notifications
    required property var shell

    // The item the file was added to (the playing one may have changed since) with its name, and the file
    property string itemId: ""
    property string itemName: ""
    property url file
    property string fileTitle: ""
    // An upload this dialog started: the server's answer is announced only then, not for an upload or a delete of Edit Subtitles
    property bool uploading: false
    // Empty means undetermined; the client refuses anything but two or three letters
    readonly property bool languageValid: languageField.length === 0 || languageField.acceptableInput

    title: i18n("Upload Subtitle")
    preferredWidth: Kirigami.Units.gridUnit * 26
    standardButtons: Kirigami.Dialog.Cancel
    padding: Kirigami.Units.largeSpacing
    customFooterActions: [
        Kirigami.Action {
            objectName: "uploadSubtitleConfirm"
            text: i18nc("@action:button", "Upload")
            icon.name: "document-send"
            enabled: dialog.languageValid
            onTriggered: dialog.upload()
        }
    ]

    header: DialogTitleBar {
        title: dialog.title
        onCloseRequested: dialog.close()
    }

    function openFor(item, itemTitle, copy, name) {
        if (connection.state !== ConnectionController.Authenticated)
            return;
        itemId = item;
        itemName = itemTitle;
        file = copy;
        fileTitle = name;
        languageField.text = "";
        forcedSwitch.checked = false;
        open();
    }

    function upload() {
        // The formats of JellyfinClient::uploadSubtitle, which answers false alike for an empty, oversized, unreadable or non-UTF-8 file, an upload already on its way and a lost session: the dialog stays for another try
        if (connection.uploadSubtitle(itemId, file, languageField.text, forcedSwitch.checked)) {
            uploading = true;
            close();
        } else {
            shell.showPassiveNotification(i18n("The subtitle could not be uploaded: check the file (not empty, at most 2 MiB, in UTF-8) and wait for any upload on its way"));
        }
    }

    onOpened: languageField.forceActiveFocus(Qt.PopupFocusReason)

    Connections {
        target: dialog.connection
        function onItemSubtitleUpdated(itemId) {
            if (dialog.uploading && itemId === dialog.itemId) {
                dialog.uploading = false;
                dialog.shell.showPassiveNotification(i18n("Subtitles updated"));
            }
        }
        function onItemSubtitleUpdateFailed(itemId, status) {
            if (dialog.uploading && itemId === dialog.itemId) {
                dialog.uploading = false;
                dialog.shell.showPassiveNotification(i18n("The subtitle could not be changed: check the file and the connection to the server"));
            }
        }
        // Another sign-in is another user: the file and the item are not offered to it
        function onStateChanged() {
            if (dialog.connection.state !== ConnectionController.Authenticated) {
                dialog.close();
                dialog.itemId = "";
                dialog.uploading = false;
            }
        }
    }

    ColumnLayout {
        spacing: Kirigami.Units.largeSpacing

        // The file name is the user's own text: plain text
        Controls.Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
            text: dialog.fileTitle
            font.weight: Font.DemiBold
        }
        // The item's name is the server's text: plain text
        Controls.Label {
            Layout.fillWidth: true
            opacity: 0.7
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
            text: i18n("This subtitle is added to “%1” on the server, for everyone who watches it", dialog.itemName)
        }

        Kirigami.FormLayout {
            Layout.fillWidth: true

            Controls.TextField {
                id: languageField
                objectName: "uploadSubtitleLanguage"
                Kirigami.FormData.label: i18nc("@label:textbox", "Language:")
                placeholderText: i18n("eng")
                maximumLength: 3
                validator: RegularExpressionValidator {
                    regularExpression: /[A-Za-z]{2,3}/
                }
                Accessible.name: i18n("Language")
                Accessible.description: i18n("Language code of two or three letters, empty for undetermined")
                onAccepted: if (dialog.languageValid) dialog.upload()
            }
            Controls.CheckBox {
                id: forcedSwitch
                objectName: "uploadSubtitleForced"
                Kirigami.FormData.label: i18n("Forced:")
                Accessible.name: i18n("Forced")
            }
        }

        MessageBox {
            objectName: "uploadSubtitleLanguageInvalid"
            Layout.fillWidth: true
            Layout.topMargin: Kirigami.Units.largeSpacing
            visible: !dialog.languageValid && (languageField.length >= 2 || !languageField.activeFocus)
            type: Kirigami.MessageType.Warning
            text: i18n("Use a language code of two or three letters, or leave the field empty")
        }
    }
}
