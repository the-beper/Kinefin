// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// The dialogs of the playlists, one set for the window: the name of a new or renamed playlist, the delete question and the Add to Playlist picker (the card menu, the details page and the playlist pages open them). The answers come back through PlaylistsController; neither the dialogs nor the notifications quote a playlist name
Item {
    id: root

    required property ConnectionController connection
    // The window: it shows the notifications
    required property var shell

    readonly property PlaylistsController playlists: connection.playlists
    // A second request while one dialog is open (F2 again) would reset what it holds
    readonly property bool anyOpen: nameDialog.visible || deleteDialog.visible || addDialog.visible

    // `name` fills the field, empty when the caller does not know it
    function rename(playlistId, name) {
        if (!anyOpen)
            openNameDialog(false, playlistId, name ?? "", []);
    }

    // A new playlist holding `itemIds` (an empty list for an empty playlist)
    function create(itemIds) {
        if (!anyOpen)
            openNameDialog(true, "", "", itemIds);
    }

    function askDelete(playlistId) {
        if (anyOpen)
            return;
        deleteDialog.playlistId = playlistId;
        deleteDialog.open();
    }

    // The picker of the playlist `itemId` goes into
    function addTo(itemId) {
        if (anyOpen)
            return;
        addDialog.itemId = itemId;
        addDialog.open();
    }

    function openNameDialog(creating, playlistId, name, itemIds) {
        nameDialog.creating = creating;
        nameDialog.playlistId = playlistId;
        nameDialog.originalName = name;
        nameDialog.itemIds = itemIds;
        nameField.text = name;
        nameDialog.open();
    }

    Kirigami.PromptDialog {
        id: nameDialog
        objectName: "playlistNameDialog"

        // A new playlist, else a rename of `playlistId`
        property bool creating: false
        property string playlistId: ""
        property string originalName: ""
        property var itemIds: []
        readonly property string newName: root.playlists.validName(nameField.text)
        readonly property bool acceptable: newName.length > 0 && (creating || newName !== originalName) && !root.playlists.writing

        function accept() {
            if (!acceptable)
                return;
            close();
            // Refused at once: a write of the playlist is already on its way, or the session is gone
            if (creating && !root.playlists.create(nameField.text, itemIds))
                root.shell.showPassiveNotification(i18n("The playlist could not be created: wait for the changes on their way and try again"));
            else if (!creating && !root.playlists.rename(playlistId, nameField.text))
                root.shell.showPassiveNotification(i18n("The playlist could not be renamed: wait for the changes on their way and try again"));
        }

        title: creating ? i18nc("@title:window", "New Playlist") : i18nc("@title:window", "Rename Playlist")
        standardButtons: Kirigami.Dialog.Cancel
        customFooterActions: [
            Kirigami.Action {
                text: nameDialog.creating ? i18nc("@action:button", "Create") : i18nc("@action:button", "Rename")
                icon.name: nameDialog.creating ? "list-add" : "edit-rename"
                enabled: nameDialog.acceptable
                onTriggered: nameDialog.accept()
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
            onAccepted: nameDialog.accept()
        }
        // Text that is only spaces or invisible characters
        MessageBox {
            objectName: "playlistNameInvalid"
            Layout.fillWidth: true
            Layout.topMargin: Kirigami.Units.largeSpacing
            visible: nameField.text.length > 0 && nameDialog.newName.length === 0
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

    AddToPlaylistDialog {
        id: addDialog
        connection: root.connection
        shell: root.shell
        maximumWidth: Math.min(Kirigami.Units.gridUnit * 26, root.shell.width - Kirigami.Units.largeSpacing * 2)
        // The picker gives way to the name of the new playlist, which then holds the item
        onNewPlaylistRequested: itemId => root.openNameDialog(true, "", "", [itemId])
    }

    Connections {
        target: root.playlists
        function onRenameFailed(playlistId, forbidden) {
            root.shell.showPassiveNotification(forbidden ? i18n("Missing permission to change this playlist") : i18n("The playlist could not be renamed: check the connection to the server and try again"));
        }
        function onDeleteFailed(playlistId, forbidden) {
            root.shell.showPassiveNotification(forbidden ? i18n("Missing permission to delete this playlist") : i18n("The playlist could not be deleted: check the connection to the server and try again"));
        }
        // An empty playlist shows up in the list; one made from an item has no other sign of it
        function onCreated(playlistId, withItems) {
            if (withItems)
                root.shell.showPassiveNotification(i18n("Added to the new playlist"));
        }
        function onCreateFailed(forbidden) {
            root.shell.showPassiveNotification(forbidden ? i18n("Missing permission to create playlists") : i18n("The playlist could not be created: check the connection to the server and try again"));
        }
        function onAdded(playlistId) {
            root.shell.showPassiveNotification(i18n("Added to the playlist"));
        }
        function onAlreadyInPlaylist(playlistId) {
            root.shell.showPassiveNotification(i18n("Already in the playlist"));
        }
        function onAddBusy(playlistId) {
            root.shell.showPassiveNotification(i18n("The video could not be added to the playlist: wait for the changes on their way and try again"));
        }
        function onAddFailed(playlistId, forbidden) {
            root.shell.showPassiveNotification(forbidden ? i18n("Missing permission to change this playlist") : i18n("The video could not be added to the playlist: check the connection to the server and try again"));
        }
    }
}
