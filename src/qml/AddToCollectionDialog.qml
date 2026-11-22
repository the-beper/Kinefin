// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// The collection a movie or a show goes into: New Collection… first, then the user's collections by name, read again whenever the dialog opens. A pick closes the dialog and its outcome is a notification
Kirigami.Dialog {
    id: dialog
    objectName: "addToCollectionDialog"

    required property ConnectionController connection
    // The window shows the notifications
    required property var shell
    property string itemId: ""
    property string itemName: ""

    readonly property CollectionsModel model: connection.collections

    signal newCollectionRequested(string itemId, string itemName)

    title: i18nc("@title:window", "Add to Collection")
    preferredWidth: Kirigami.Units.gridUnit * 22
    maximumHeight: absoluteMaximumHeight - Kirigami.Units.gridUnit * 4
    standardButtons: Kirigami.Dialog.Cancel
    padding: 0

    onOpened: {
        model.open();
        list.currentIndex = -1;
        list.forceActiveFocus(Qt.PopupFocusReason);
    }
    onClosed: model.close()

    function pick(collectionId) {
        close();
        if (!connection.addToCollection(collectionId, itemId))
            shell.showPassiveNotification(i18n("The item could not be added to the collection: wait for the changes on their way and try again"));
    }

    function requestNewCollection() {
        close();
        newCollectionRequested(itemId, itemName);
    }

    ListView {
        id: list
        objectName: "addToCollectionList"

        implicitWidth: Kirigami.Units.gridUnit * 22
        implicitHeight: Math.max(contentHeight, Kirigami.Units.gridUnit * 8)
        clip: true
        model: dialog.model
        keyNavigationEnabled: true
        activeFocusOnTab: true
        Accessible.role: Accessible.List
        Accessible.name: dialog.title

        // Up from the first collection reaches New Collection…, the header the list marks with no current row
        Keys.onUpPressed: event => {
            if (list.currentIndex <= 0)
                list.currentIndex = -1;
            else
                event.accepted = false;
        }
        function activateCurrent() {
            if (!list.currentItem)
                dialog.requestNewCollection();
            else if (list.currentItem.loaded)
                dialog.pick(list.currentItem.itemId);
        }
        Keys.onReturnPressed: activateCurrent()
        Keys.onEnterPressed: activateCurrent()

        header: ColumnLayout {
            width: ListView.view.width
            spacing: 0

            MessageBox {
                objectName: "addToCollectionStale"
                Layout.fillWidth: true
                Layout.margins: Kirigami.Units.largeSpacing
                visible: dialog.model.status === CollectionsModel.Failed && list.count > 0
                type: Kirigami.MessageType.Warning
                text: i18n("The collections could not be read again: the list may be out of date")
            }
            Controls.ItemDelegate {
                objectName: "addToNewCollection"
                Layout.fillWidth: true
                text: i18nc("@action:button", "New Collection…")
                icon.name: "list-add"
                focusPolicy: Qt.NoFocus
                highlighted: list.activeFocus && list.currentIndex < 0
                onClicked: dialog.requestNewCollection()
            }
        }

        delegate: Controls.ItemDelegate {
            id: row

            required property int index
            required property string itemId
            required property string name
            required property bool loaded

            objectName: "addToCollectionRow_" + index
            width: ListView.view.width
            focusPolicy: Qt.NoFocus
            enabled: loaded
            highlighted: ListView.isCurrentItem && ListView.view.activeFocus
            Accessible.name: name
            // The style shows its own tooltip, with the delegate's empty text, for a truncated label: the plain one below replaces it
            Controls.ToolTip.visible: false
            onClicked: dialog.pick(itemId)

            // The collection name comes from the server: plain text only
            contentItem: Controls.Label {
                id: collectionLabel
                elide: Text.ElideRight
                maximumLineCount: 1
                textFormat: Text.PlainText
                text: row.name
            }

            PlainToolTip {
                visible: row.hovered && collectionLabel.truncated
                text: row.name
            }
        }

        Controls.BusyIndicator {
            FadeInOnShow {}
            anchors.centerIn: parent
            running: visible
            visible: dialog.model.status === CollectionsModel.Loading && list.count === 0
        }

        Kirigami.PlaceholderMessage {
            FadeInOnShow {}
            anchors.centerIn: parent
            anchors.verticalCenterOffset: list.headerItem ? list.headerItem.height / 2 : 0
            width: parent.width - Kirigami.Units.gridUnit * 2
            visible: dialog.model.status === CollectionsModel.Failed && list.count === 0
            text: i18n("Collections could not be loaded")
            helpfulAction: Kirigami.Action {
                text: i18n("Retry")
                icon.name: "view-refresh"
                onTriggered: dialog.model.retry()
            }
        }

        Kirigami.PlaceholderMessage {
            FadeInOnShow {}
            anchors.centerIn: parent
            anchors.verticalCenterOffset: list.headerItem ? list.headerItem.height / 2 : 0
            width: parent.width - Kirigami.Units.gridUnit * 2
            visible: dialog.model.status === CollectionsModel.Ready && list.count === 0
            text: i18n("No collections")
        }
    }
}
