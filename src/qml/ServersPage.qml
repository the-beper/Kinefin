// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Kirigami.ScrollablePage {
    id: page
    objectName: "serversPage"

    required property ConnectionController connection
    readonly property bool busy: connection.state === ConnectionController.Connecting
    readonly property bool confirming: connection.state === ConnectionController.ConfirmInsecure

    title: i18n("Servers")

    // Host and port of a server address: the only part shown in the confirmation, so no server-controlled markup can reach the dialog text
    function hostOf(url) {
        return url.replace(/^[a-z]+:\/\//i, "").split("/")[0];
    }

    Kirigami.PromptDialog {
        id: removeDialog
        property string serverId
        property string host
        title: i18n("Remove Server")
        subtitle: i18n("Remove %1 from the saved servers? You will have to sign in again to use it", host)
        standardButtons: Kirigami.Dialog.Cancel
        customFooterActions: [
            Kirigami.Action {
                text: i18n("Remove")
                icon.name: "edit-delete-remove"
                onTriggered: {
                    page.connection.removeServer(removeDialog.serverId);
                    removeDialog.close();
                }
            }
        ]
    }

    ColumnLayout {
        spacing: Kirigami.Units.largeSpacing

        MessageBox {
            visible: page.connection.errorText.length > 0
            type: Kirigami.MessageType.Error
            text: page.connection.errorText
        }

        MessageBox {
            visible: page.confirming
            type: Kirigami.MessageType.Warning
            Layout.topMargin: Kirigami.Units.largeSpacing
            text: i18n("This server does not use HTTPS: your username, password and session would travel unencrypted")
            actions: [
                Kirigami.Action {
                    text: i18n("Continue Anyway")
                    onTriggered: page.connection.confirmInsecureHttp()
                },
                Kirigami.Action {
                    text: i18n("Cancel")
                    onTriggered: page.connection.cancel()
                }
            ]
        }

        Kirigami.PlaceholderMessage {
            Layout.fillWidth: true
            visible: page.connection.servers.length === 0 && !page.busy && !page.confirming
            text: i18n("No servers yet")
            explanation: i18n("Enter the address of your Jellyfin server below")
            icon.name: "network-server"
        }

        Kirigami.Heading {
            visible: page.connection.servers.length > 0
            level: 2
            text: i18n("Saved Servers")
        }

        Repeater {
            model: page.connection.servers
            delegate: Kirigami.AbstractCard {
                id: card
                required property var modelData
                Layout.fillWidth: true
                enabled: !page.busy && !page.confirming
                contentItem: RowLayout {
                    Controls.Label {
                        Layout.fillWidth: true
                        elide: Text.ElideMiddle
                        textFormat: Text.PlainText
                        text: card.modelData.url
                    }
                    Controls.Button {
                        text: i18n("Connect")
                        icon.name: "network-connect"
                        onClicked: page.connection.connectTo(card.modelData.url)
                    }
                    Controls.Button {
                        icon.name: "edit-delete-remove"
                        text: i18n("Remove")
                        display: Controls.AbstractButton.IconOnly
                        Accessible.name: i18n("Remove server %1", card.modelData.url)
                        Controls.ToolTip.visible: hovered
                        Controls.ToolTip.text: text
                        onClicked: {
                            removeDialog.serverId = card.modelData.serverId;
                            removeDialog.host = page.hostOf(card.modelData.url);
                            removeDialog.open();
                        }
                    }
                }
            }
        }

        Kirigami.Heading {
            level: 2
            text: i18n("Add a Server")
        }

        RowLayout {
            Layout.fillWidth: true
            enabled: !page.busy && !page.confirming

            Controls.TextField {
                id: address
                Layout.fillWidth: true
                placeholderText: i18n("jellyfin.example.com or http://192.168.1.10:8096")
                inputMethodHints: Qt.ImhUrlCharactersOnly | Qt.ImhNoPredictiveText
                Accessible.name: i18n("Server address")
                onAccepted: connectButton.clicked()
                Component.onCompleted: forceActiveFocus()
            }
            Controls.Button {
                id: connectButton
                text: i18n("Connect")
                icon.name: "network-connect"
                enabled: address.text.trim().length > 0
                onClicked: page.connection.connectTo(address.text)
            }
        }

        RowLayout {
            visible: page.busy
            spacing: Kirigami.Units.largeSpacing
            Controls.BusyIndicator {
                running: page.busy
            }
            Controls.Label {
                text: i18n("Connecting…")
            }
            Controls.Button {
                text: i18n("Cancel")
                onClicked: page.connection.cancel()
            }
        }
    }
}
