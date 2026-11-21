// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// The collection dialogs of the window: the Add to Collection picker and the name of a new collection holding one item
Item {
    id: root

    required property ConnectionController connection
    // The window: it shows the notifications
    required property var shell

    readonly property bool anyOpen: nameDialog.visible || addDialog.visible

    function addTo(itemId, itemName) {
        if (anyOpen)
            return;
        addDialog.itemId = itemId;
        addDialog.itemName = itemName;
        addDialog.open();
    }

    function openNameDialog(itemId, itemName) {
        nameDialog.itemId = itemId;
        nameDialog.itemName = itemName;
        nameField.text = itemName;
        nameDialog.open();
    }

    Kirigami.PromptDialog {
        id: nameDialog
        objectName: "collectionNameDialog"

        property string itemId: ""
        property string itemName: ""
        readonly property string newName: nameField.text.trim()
        readonly property bool acceptable: newName.length > 0

        function accept() {
            if (!acceptable)
                return;
            if (!connection.createCollection(newName, itemId)) {
                root.shell.showPassiveNotification(i18n("The collection could not be created: wait for the changes on their way and try again"));
                return;
            }
            close();
        }

        title: i18nc("@title:window", "New Collection")
        standardButtons: Kirigami.Dialog.Cancel
        customFooterActions: [
            Kirigami.Action {
                text: i18nc("@action:button", "Create")
                icon.name: "list-add"
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
            objectName: "collectionNameField"
            Layout.fillWidth: true
            maximumLength: 128
            Accessible.name: i18nc("@label:textbox", "Name")
            onAccepted: nameDialog.accept()
        }
        MessageBox {
            objectName: "collectionNameInvalid"
            Layout.fillWidth: true
            Layout.topMargin: Kirigami.Units.largeSpacing
            visible: nameField.text.length > 0 && nameDialog.newName.length === 0
            type: Kirigami.MessageType.Warning
            text: i18n("The name needs at least one visible character")
        }
    }

    AddToCollectionDialog {
        id: addDialog
        connection: root.connection
        shell: root.shell
        maximumWidth: Math.min(Kirigami.Units.gridUnit * 26, root.shell.width - Kirigami.Units.largeSpacing * 2)
        onNewCollectionRequested: (itemId, itemName) => root.openNameDialog(itemId, itemName)
    }

    Connections {
        target: root.connection
        function onCollectionCreated(collectionId, name) {
            root.shell.showPassiveNotification(i18n("Added to the new collection"));
        }
        function onCollectionCreateFailed(status) {
            if (status === 403)
                root.shell.showPassiveNotification(i18n("The server did not allow the creation of collections"));
            else
                root.shell.showPassiveNotification(i18n("The collection could not be created: check the connection to the server and try again"));
        }
        function onCollectionItemsAdded(collectionId, itemId) {
            root.shell.showPassiveNotification(i18n("Added to the collection"));
        }
        function onCollectionItemsAddFailed(collectionId, itemId, status) {
            if (status === 403)
                root.shell.showPassiveNotification(i18n("The server did not allow changes to collections"));
            else
                root.shell.showPassiveNotification(i18n("The item could not be added to the collection: check the connection to the server and try again"));
        }
    }
}
