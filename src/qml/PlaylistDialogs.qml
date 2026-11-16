// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// The rename and delete dialogs of a playlist, one pair for the window (the card menu and the Playlist page open them). The answers come back through PlaylistsController; neither the dialogs nor the notifications quote a playlist name
Item {
    id: root

    required property ConnectionController connection
    // The window: it shows the notifications
    required property var shell

    readonly property PlaylistsController playlists: connection.playlists

    // `name` fills the field, empty when the caller does not know it
    function rename(playlistId, name) {
        // F2 again while a dialog is open would reset what was typed
        if (renameDialog.visible || deleteDialog.visible)
            return;
        renameDialog.playlistId = playlistId;
        renameDialog.originalName = name ?? "";
        nameField.text = renameDialog.originalName;
        renameDialog.open();
    }

    function askDelete(playlistId) {
        if (renameDialog.visible || deleteDialog.visible)
            return;
        deleteDialog.playlistId = playlistId;
        deleteDialog.open();
    }

    Kirigami.PromptDialog {
        id: renameDialog
        objectName: "renamePlaylistDialog"

        property string playlistId: ""
        property string originalName: ""
        readonly property string newName: root.playlists.validName(nameField.text)
        readonly property bool acceptable: newName.length > 0 && newName !== originalName && !root.playlists.writing

        function accept() {
            if (!acceptable)
                return;
            close();
            // Refused at once: a write of the playlist is already on its way, or the session is gone
            if (!root.playlists.rename(playlistId, nameField.text))
                root.shell.showPassiveNotification(i18n("The playlist could not be renamed: wait for the changes on their way and try again"));
        }

        title: i18nc("@title:window", "Rename Playlist")
        standardButtons: Kirigami.Dialog.Cancel
        customFooterActions: [
            Kirigami.Action {
                text: i18nc("@action:button", "Rename")
                icon.name: "edit-rename"
                enabled: renameDialog.acceptable
                onTriggered: renameDialog.accept()
            }
        ]
        onOpened: {
            nameField.forceActiveFocus(Qt.PopupFocusReason);
            nameField.selectAll();
        }

        Controls.Label {
            Layout.fillWidth: true
            text: i18nc("@label:textbox", "Name:")
        }
        // Server text in an editable field: a TextField shows it as plain text
        Controls.TextField {
            id: nameField
            objectName: "playlistNameField"
            Layout.fillWidth: true
            maximumLength: root.playlists.maxNameLength
            Accessible.name: i18nc("@label:textbox", "Name")
            onAccepted: renameDialog.accept()
        }
        // Text that is only spaces or invisible characters
        MessageBox {
            objectName: "playlistNameInvalid"
            Layout.fillWidth: true
            Layout.topMargin: Kirigami.Units.largeSpacing
            visible: nameField.text.length > 0 && renameDialog.newName.length === 0
            type: Kirigami.MessageType.Warning
            text: i18n("The name needs at least one visible character")
        }
    }

    Kirigami.PromptDialog {
        id: deleteDialog
        objectName: "deletePlaylistDialog"

        property string playlistId: ""

        function confirm() {
            close();
            if (!root.playlists.deletePlaylist(playlistId))
                root.shell.showPassiveNotification(i18n("The playlist could not be deleted: wait for the changes on their way and try again"));
        }

        title: i18nc("@title:window", "Delete Playlist")
        subtitle: i18n("Delete this playlist from the server? The videos in it stay in the library")
        standardButtons: Kirigami.Dialog.Cancel
        customFooterActions: [
            Kirigami.Action {
                text: i18nc("@action:button", "Delete")
                icon.name: "edit-delete"
                enabled: !root.playlists.writing
                onTriggered: deleteDialog.confirm()
            }
        ]
    }

    Connections {
        target: root.playlists
        function onRenameFailed(playlistId, forbidden) {
            root.shell.showPassiveNotification(forbidden ? i18n("Missing permission to change this playlist") : i18n("The playlist could not be renamed: check the connection to the server and try again"));
        }
        function onDeleteFailed(playlistId, forbidden) {
            root.shell.showPassiveNotification(forbidden ? i18n("Missing permission to delete this playlist") : i18n("The playlist could not be deleted: check the connection to the server and try again"));
        }
    }
}
