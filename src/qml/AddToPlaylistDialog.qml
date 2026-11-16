// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// The playlist a video goes into: New Playlist… first, then the user's playlists by name, read again whenever the dialog opens. A pick closes the dialog; the add skips a video already in the playlist and its outcome is a notification
Kirigami.Dialog {
    id: dialog
    objectName: "addToPlaylistDialog"

    required property ConnectionController connection
    // The window counts the readers of the playlists list and shows the notifications
    required property var shell
    property string itemId: ""

    readonly property PlaylistsController playlists: connection.playlists
    readonly property bool loading: playlists.status === PlaylistsController.Loading
    readonly property bool failed: playlists.status === PlaylistsController.Failed

    signal newPlaylistRequested(string itemId)

    title: i18nc("@title:window", "Add to Playlist")
    preferredWidth: Kirigami.Units.gridUnit * 22
    maximumHeight: absoluteMaximumHeight - Kirigami.Units.gridUnit * 4
    standardButtons: Kirigami.Dialog.Cancel
    padding: 0

    // Whether this opening counted the dialog among the readers of the list: a dialog closed while it still opens may close without having opened
    property bool counted: false
    onOpened: {
        if (!counted) {
            counted = true;
            shell.playlistsPages++;
        }
        playlists.reload();
        list.currentIndex = -1;
        list.forceActiveFocus(Qt.PopupFocusReason);
    }
    onClosed: {
        if (!counted)
            return;
        counted = false;
        shell.playlistsPageGone();
    }

    function pick(playlistId) {
        close();
        // Refused at once: another add is on its way, or the session is gone
        if (!playlists.add(playlistId, itemId))
            shell.showPassiveNotification(i18n("The video could not be added to the playlist: wait for the changes on their way and try again"));
    }

    function requestNewPlaylist() {
        close();
        newPlaylistRequested(itemId);
    }

    ListView {
        id: list
        objectName: "addToPlaylistList"

        implicitWidth: Kirigami.Units.gridUnit * 22
        implicitHeight: Math.max(contentHeight, Kirigami.Units.gridUnit * 8)
        clip: true
        model: dialog.playlists.list
        keyNavigationEnabled: true
        activeFocusOnTab: true
        Accessible.role: Accessible.List
        Accessible.name: dialog.title

        // Up from the first playlist reaches New Playlist…, the header the list marks with no current row
        Keys.onUpPressed: event => {
            if (list.currentIndex <= 0)
                list.currentIndex = -1;
            else
                event.accepted = false;
        }
        // Like a click, nothing while a write is on its way (the rows are disabled then)
        function activateCurrent() {
            if (dialog.playlists.writing)
                return;
            if (list.currentItem)
                dialog.pick(list.currentItem.itemId);
            else
                dialog.requestNewPlaylist();
        }
        Keys.onReturnPressed: activateCurrent()
        Keys.onEnterPressed: activateCurrent()

        header: ColumnLayout {
            width: ListView.view.width
            spacing: 0

            // The rows shown are those of the last read that answered
            MessageBox {
                objectName: "addToPlaylistStale"
                Layout.fillWidth: true
                Layout.margins: Kirigami.Units.largeSpacing
                visible: dialog.failed && list.count > 0
                type: Kirigami.MessageType.Warning
                text: i18n("The playlists could not be read again: the list may be out of date")
            }
            Controls.ItemDelegate {
                objectName: "addToNewPlaylist"
                Layout.fillWidth: true
                text: i18nc("@action:button", "New Playlist…")
                icon.name: "list-add"
                focusPolicy: Qt.NoFocus
                highlighted: list.activeFocus && list.currentIndex < 0
                onClicked: dialog.requestNewPlaylist()
            }
        }

        delegate: Controls.ItemDelegate {
            id: row

            required property int index
            required property string itemId
            required property string name

            objectName: "addToPlaylistRow_" + index
            width: ListView.view.width
            // The list is the one tab stop, the arrow keys move inside it
            focusPolicy: Qt.NoFocus
            highlighted: ListView.isCurrentItem && ListView.view.activeFocus
            enabled: !dialog.playlists.writing
            Accessible.name: name
            onClicked: dialog.pick(itemId)

            // The playlist name comes from the server: plain text only
            contentItem: Controls.Label {
                elide: Text.ElideRight
                maximumLineCount: 1
                textFormat: Text.PlainText
                text: row.name
            }
        }

        Controls.BusyIndicator {
            FadeInOnShow {}
            anchors.centerIn: parent
            running: visible
            visible: dialog.loading && list.count === 0
        }

        Kirigami.PlaceholderMessage {
            FadeInOnShow {}
            objectName: "addToPlaylistNotLoaded"
            anchors.centerIn: parent
            anchors.verticalCenterOffset: list.headerItem ? list.headerItem.height / 2 : 0
            width: parent.width - Kirigami.Units.gridUnit * 2
            visible: dialog.failed && list.count === 0
            text: i18n("Playlists could not be loaded")
            helpfulAction: Kirigami.Action {
                text: i18n("Retry")
                icon.name: "view-refresh"
                onTriggered: dialog.playlists.reload()
            }
        }

        Kirigami.PlaceholderMessage {
            FadeInOnShow {}
            objectName: "addToPlaylistEmpty"
            anchors.centerIn: parent
            anchors.verticalCenterOffset: list.headerItem ? list.headerItem.height / 2 : 0
            width: parent.width - Kirigami.Units.gridUnit * 2
            visible: dialog.playlists.status === PlaylistsController.Ready && list.count === 0
            text: i18n("No playlists")
        }
    }
}
