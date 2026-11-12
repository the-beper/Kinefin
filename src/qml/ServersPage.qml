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

    // The row toolbar is ToolBar everywhere (Main.qml): the pre-signin screens keep their identity in the top bar instead
    globalToolBarStyle: Kirigami.ApplicationHeaderStyle.None

    readonly property bool busy: connection.state === ConnectionController.Connecting
    readonly property bool confirming: connection.state === ConnectionController.ConfirmInsecure
    readonly property bool splitLayout: width >= Kirigami.Units.gridUnit * 40
    // Keep entry controls usable even when the page spans a very wide monitor.
    readonly property real maxContentWidth: Kirigami.Units.gridUnit * 82
    readonly property string formUrl: {
        const host = hostField.text.trim();
        if (!host || /[\s@?#]/.test(host) || host.includes("/") || host.includes("\\") || host.includes("://"))
            return "";
        // Literal IPv6 hosts use URL brackets, not a host:port ambiguity.
        const address = host.includes(":") && !host.startsWith("[") ? "[" + host + "]" : host;
        return scheme.currentValue + "://" + address + ":" + portField.value;
    }
    readonly property var newServerStatus: connection.serverReachability[formUrl] ?? ({})

    title: i18n("Servers")

    // The keyboard starts in the address field: what a fresh visitor fills in first. While the connection is being made or its warning is up, the field is not the control the user needs
    function focusPrimary() {
        if (!page.busy && !page.confirming)
            hostField.forceActiveFocus(Qt.TabFocusReason);
    }

    // The tests query the exact URL that is displayed; switching the form or
    // opening another server cannot show a stale successful probe for a new URL.
    Component.onCompleted: connection.testSavedServers()

    function connectAddress(url, tryFallback) {
        if (!url || busy || confirming)
            return;
        page.Window.window?.beginServerSelection();
        if (tryFallback)
            connection.connectToWithFallback(url);
        else
            connection.connectTo(url);
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
        spacing: Kirigami.Units.largeSpacing * 2

        MessageBox {
            Layout.fillWidth: true
            visible: page.connection.errorText.length > 0
            type: Kirigami.MessageType.Error
            text: page.connection.errorText
        }
        MessageBox {
            Layout.fillWidth: true
            visible: page.confirming
            type: Kirigami.MessageType.Warning
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

        GridLayout {
            Layout.fillWidth: true
            Layout.maximumWidth: page.maxContentWidth
            Layout.alignment: Qt.AlignHCenter
            columns: page.splitLayout ? 2 : 1
            columnSpacing: Kirigami.Units.largeSpacing * 2
            rowSpacing: Kirigami.Units.largeSpacing * 2

            Kirigami.AbstractCard {
                id: addCard
                objectName: "addServerCard"
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignTop
                Layout.preferredWidth: page.splitLayout ? Math.min(page.width, page.maxContentWidth) * 0.54
                                                       : Math.max(0, Math.min(page.width - Kirigami.Units.largeSpacing * 2, page.maxContentWidth))
                contentItem: ColumnLayout {
                    spacing: Kirigami.Units.largeSpacing
                    Kirigami.Heading { level: 2; text: i18n("Add a Server") }
                    Controls.Label {
                        Layout.fillWidth: true
                        text: i18n("Enter a domain or IP address and select the connection protocol")
                        wrapMode: Text.Wrap
                        opacity: 0.8
                    }
                    GridLayout {
                        Layout.fillWidth: true
                        // On a medium desktop, the add-server card is only ~half the window.
                        columns: page.splitLayout && page.width >= Kirigami.Units.gridUnit * 44 ? 3 : 1
                        columnSpacing: Kirigami.Units.smallSpacing
                        rowSpacing: Kirigami.Units.smallSpacing
                        enabled: !page.busy && !page.confirming
                        Controls.ComboBox {
                            id: scheme
                            objectName: "serverProtocolSelector"
                            Layout.preferredWidth: Kirigami.Units.gridUnit * 6
                            Layout.fillWidth: !page.splitLayout
                            model: [{ text: "HTTP", value: "http" }, { text: "HTTPS", value: "https" }]
                            textRole: "text"
                            valueRole: "value"
                            onActivated: {
                                if (currentValue === "https" && portField.value === 8096)
                                    portField.value = 8920;
                                else if (currentValue === "http" && portField.value === 8920)
                                    portField.value = 8096;
                            }
                            Accessible.name: i18n("Protocol")
                        }
                        Controls.TextField {
                            id: hostField
                            objectName: "serverHostField"
                            Layout.fillWidth: true
                            Layout.minimumWidth: Kirigami.Units.gridUnit * 7
                            placeholderText: i18n("Domain or IP address")
                            inputMethodHints: Qt.ImhUrlCharactersOnly | Qt.ImhNoPredictiveText
                            Accessible.name: i18n("Domain or IP address")
                            onAccepted: page.connectAddress(page.formUrl, true)
                        }
                        Controls.SpinBox {
                            id: portField
                            objectName: "serverPortField"
                            Layout.preferredWidth: Kirigami.Units.gridUnit * 6
                            from: 1
                            to: 65535
                            editable: true
                            value: 8096
                            Accessible.name: i18n("Port")
                        }
                    }
                    Controls.Label {
                        Layout.fillWidth: true
                        text: i18n("Only standard ports on the same protocol are tried automatically if a connection fails")
                        wrapMode: Text.Wrap
                        font.pointSize: Kirigami.Theme.smallFont.pointSize
                        opacity: 0.75
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Kirigami.Units.smallSpacing
                        Controls.Button {
                            id: testNewServer
                            objectName: "testNewServerButton"
                            text: i18n("Test")
                            icon.name: "network-server"
                            enabled: page.formUrl.length > 0 && !page.newServerStatus.testing && !page.busy && !page.confirming
                            onClicked: page.connection.testServer(page.formUrl, true)
                        }
                        Controls.Button {
                            id: connectNewServer
                            objectName: "connectNewServerButton"
                            Layout.fillWidth: true
                            text: i18n("Connect")
                            icon.name: "network-connect"
                            enabled: page.formUrl.length > 0 && !page.busy && !page.confirming
                            onClicked: page.connectAddress(page.formUrl, true)
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        visible: page.newServerStatus.message !== undefined
                        Controls.BusyIndicator {
                            running: !!page.newServerStatus.testing
                            visible: running
                            Layout.preferredWidth: Kirigami.Units.iconSizes.smallMedium
                            Layout.preferredHeight: width
                        }
                        Kirigami.Icon {
                            source: page.newServerStatus.reachable ? "emblem-checked" : "dialog-warning"
                            visible: !page.newServerStatus.testing
                            Layout.preferredWidth: Kirigami.Units.iconSizes.smallMedium
                            Layout.preferredHeight: width
                        }
                        Controls.Label {
                            Layout.fillWidth: true
                            text: page.newServerStatus.message ?? ""
                            wrapMode: Text.Wrap
                        }
                    }
                    RowLayout {
                        visible: page.busy
                        Controls.BusyIndicator { running: page.busy }
                        Controls.Label { text: i18n("Connecting…") }
                        Controls.Button {
                            text: i18n("Cancel")
                            onClicked: page.connection.cancel()
                        }
                    }
                }
            }

            Kirigami.AbstractCard {
                objectName: "savedServerCard"
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignTop
                Layout.preferredWidth: page.splitLayout ? Math.min(page.width, page.maxContentWidth) * 0.42
                                                       : Math.max(0, Math.min(page.width - Kirigami.Units.largeSpacing * 2, page.maxContentWidth))
                contentItem: ColumnLayout {
                    spacing: Kirigami.Units.largeSpacing
                    Kirigami.Heading { level: 2; text: i18n("Saved Servers") }
                    Kirigami.PlaceholderMessage {
                        Layout.fillWidth: true
                        visible: page.connection.servers.length === 0
                        text: i18n("No servers yet")
                        icon.name: "network-server"
                    }
                    Repeater {
                        model: page.connection.servers
                        delegate: ColumnLayout {
                            id: saved
                            required property var modelData
                            readonly property var status: page.connection.serverReachability[modelData.url] ?? ({})
                            objectName: "savedServerEntry"
                            Layout.fillWidth: true
                            spacing: Kirigami.Units.smallSpacing
                            Controls.Label {
                                Layout.fillWidth: true
                                text: saved.modelData.url
                                textFormat: Text.PlainText
                                elide: Text.ElideMiddle
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: Kirigami.Units.smallSpacing
                                Controls.BusyIndicator {
                                    visible: !!saved.status.testing
                                    running: visible
                                    Layout.preferredWidth: Kirigami.Units.iconSizes.small
                                    Layout.preferredHeight: width
                                }
                                Kirigami.Icon {
                                    visible: !saved.status.testing
                                    source: saved.status.reachable ? "emblem-checked" : "dialog-warning"
                                    Layout.preferredWidth: Kirigami.Units.iconSizes.small
                                    Layout.preferredHeight: width
                                }
                                Controls.Label {
                                    Layout.fillWidth: true
                                    text: saved.status.message ?? i18n("Not tested")
                                    elide: Text.ElideRight
                                    textFormat: Text.PlainText
                                    opacity: 0.75
                                }
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: Kirigami.Units.smallSpacing
                                enabled: !page.busy && !page.confirming
                                Controls.Button {
                                    text: i18n("Connect")
                                    icon.name: "network-connect"
                                    Layout.fillWidth: true
                                    onClicked: page.connectAddress(saved.modelData.url, false)
                                }
                                Controls.Button {
                                    text: i18n("Test")
                                    icon.name: "network-server"
                                    enabled: !saved.status.testing
                                    onClicked: page.connection.testServer(saved.modelData.url)
                                }
                                Controls.Button {
                                    text: i18n("Delete")
                                    icon.name: "edit-delete-remove"
                                    Accessible.name: i18n("Delete server %1", saved.modelData.url)
                                    onClicked: {
                                        removeDialog.serverId = saved.modelData.serverId;
                                        removeDialog.host = saved.modelData.url;
                                        removeDialog.open();
                                    }
                                }
                            }
                            Kirigami.Separator {
                                Layout.fillWidth: true
                                Layout.topMargin: Kirigami.Units.smallSpacing
                            }
                        }
                    }
                }
            }
        }
    }
}
