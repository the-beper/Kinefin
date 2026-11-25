// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Kirigami.ScrollablePage {
    id: page
    objectName: "serversPage"

    // The darker View background on every page: a page without a list (a plain flickable) would get the lighter Window one, and the pages would not match
    Kirigami.Theme.colorSet: Kirigami.Theme.View
    Kirigami.Theme.inherit: false
    // The lone menu button stands in the top corner over the page: the content starts below it
    topPadding: Kirigami.Units.gridUnit * 2

    required property ConnectionController connection

    // The row toolbar is ToolBar everywhere (Main.qml): the pre-signin screens keep their identity in the top bar instead
    globalToolBarStyle: Kirigami.ApplicationHeaderStyle.None

    readonly property bool busy: connection.state === ConnectionController.Connecting
    readonly property bool confirming: connection.state === ConnectionController.ConfirmInsecure
    readonly property bool splitLayout: width >= Kirigami.Units.gridUnit * 40
    // Keep entry controls usable even when the page spans a very wide monitor.
    readonly property real maxContentWidth: Kirigami.Units.gridUnit * 82
    // The host of the form as the address takes it, empty when it is not one (a scheme, a path, credentials, spaces); two colons or more is a literal IPv6 host, which takes brackets, one is a host with its port
    readonly property string formHost: {
        const host = hostField.text.trim();
        if (!host || /[\s@?#]/.test(host) || host.includes("/") || host.includes("\\") || host.includes("://"))
            return "";
        return (host.match(/:/g) ?? []).length >= 2 && !host.startsWith("[") ? "[" + host + "]" : host;
    }
    // The port field: empty (automatic) or a number from 1 to 65535
    readonly property bool formPortValid: {
        const port = portField.text.trim();
        return port.length === 0 || (/^\d+$/.test(port) && parseInt(port, 10) >= 1 && parseInt(port, 10) <= 65535);
    }
    // host or host:port without a scheme (the core tries HTTPS, then HTTP); empty when the host or the port is not valid
    readonly property string formAddress: {
        if (!formHost || !formPortValid)
            return "";
        const port = portField.text.trim();
        return port.length === 0 ? formHost : formHost + ":" + parseInt(port, 10);
    }
    // The connection's error stays after a failed connect: one message box at a time, and the live check of the address is the one the user can act on, so it takes the place of the stale error. The insecure-connection warning comes from a connect that may not have used the form, so the address problem waits for it
    readonly property bool addressProblemShown: !confirming && ((hostField.text.trim().length > 0 && formHost.length === 0) || !formPortValid)
    readonly property var newServerStatus: connection.serverReachability[formAddress] ?? ({})

    title: i18n("Servers")

    // The keyboard starts in the address field: what a fresh visitor fills in first. While the connection is being made or its warning is up, the field is not the control the user needs
    function focusPrimary() {
        if (!page.busy && !page.confirming)
            hostField.forceActiveFocus(Qt.TabFocusReason);
    }

    // A server is tested only when its Test button asks, and the answer is a passive notification naming the address; the address is the user's own input, still kept from being read as markup
    Connections {
        target: page.connection
        function onServerTested(address, reachable, message) {
            const window = page.Window.window;
            if (!window)
                return;
            // A form address that answered is named in the message already, with the endpoint that answered
            const named = reachable && !address.includes("://");
            // A failure stays longer: its message says what to check
            window.showPassiveNotification(named ? window.plainNotificationText(message) : i18nc("@info %1 is a server address, %2 the result of its test", "%1: %2", window.plainNotificationText(address), window.plainNotificationText(message)), reachable ? "short" : "long");
        }
    }

    // Whether the connection under way came from the form without a scheme: an HTTP server it reached means HTTPS did not answer, which the warning says
    property bool automaticScheme: false

    function connectAddress(url, tryFallback) {
        if (!url || busy || confirming)
            return;
        automaticScheme = tryFallback && !url.includes("://");
        if (tryFallback)
            connection.connectToWithFallback(url);
        else
            connection.connectTo(url);
        page.Window.window?.showConnectCover();
    }

    Kirigami.PromptDialog {
        id: removeDialog
        property string serverId
        property string host
        title: i18n("Remove Server")
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

        // The subtitle would render the address as rich text: it goes into a plain-text label
        Controls.Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
            text: i18n("Remove %1 from the saved servers? You will have to sign in again to use it", removeDialog.host)
        }
    }

    ColumnLayout {
        // Side by side, the columns and the line between them reach the bottom of the page
        height: page.splitLayout ? Math.max(implicitHeight, page.flickable.height - page.topPadding - page.bottomPadding) : implicitHeight
        spacing: Kirigami.Units.largeSpacing * 2

        MessageBox {
            Layout.fillWidth: true
            visible: page.connection.errorText.length > 0 && !page.addressProblemShown
            type: Kirigami.MessageType.Error
            text: page.connection.errorText
        }
        Controls.Button {
            Layout.fillWidth: true
            visible: page.connection.offlineAvailable
            text: i18n("Work Offline")
            icon.name: "network-disconnect"
            onClicked: page.connection.workOffline()
        }
        MessageBox {
            Layout.fillWidth: true
            Layout.topMargin: Kirigami.Units.largeSpacing
            visible: page.confirming
            type: Kirigami.MessageType.Warning
            // The address is the one the user typed, as it answered: it passed the form's checks and QUrl, so it carries no markup the box could render
            text: page.automaticScheme
                  ? i18nc("@info %1 is the HTTP address that answered", "HTTPS did not answer, only %1 did: your username, password, and session would travel unencrypted", page.connection.serverUrl)
                  : i18n("This server does not use HTTPS: your username, password, and session would travel unencrypted")
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
            Layout.fillHeight: page.splitLayout
            Layout.maximumWidth: page.maxContentWidth
            Layout.alignment: Qt.AlignHCenter
            columns: page.splitLayout ? 3 : 1
            columnSpacing: Kirigami.Units.largeSpacing * 2
            rowSpacing: Kirigami.Units.largeSpacing * 2

            // The heading sits above the card, as the saved servers' heading does above theirs
            ColumnLayout {
                objectName: "addServerColumn"
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignTop
                Layout.preferredWidth: page.splitLayout ? Math.min(page.width, page.maxContentWidth) * 0.54
                                                       : Math.max(0, Math.min(page.width - Kirigami.Units.largeSpacing * 2, page.maxContentWidth))
                spacing: Kirigami.Units.largeSpacing
                Kirigami.Heading { level: 2; text: i18n("Add a Server") }
                Kirigami.AbstractCard {
                    id: addCard
                    objectName: "addServerCard"
                    Layout.fillWidth: true
                    contentItem: ColumnLayout {
                        spacing: Kirigami.Units.largeSpacing
                        GridLayout {
                            Layout.fillWidth: true
                            // A label before each field; on a medium desktop, the add-server card is only ~half the window, so the port goes under the address
                            columns: page.splitLayout && page.width >= Kirigami.Units.gridUnit * 44 ? 4 : 2
                            columnSpacing: Kirigami.Units.smallSpacing
                            rowSpacing: Kirigami.Units.smallSpacing
                            enabled: !page.busy && !page.confirming
                            Controls.Label {
                                text: i18nc("@label:textbox", "Address:")
                            }
                            Controls.TextField {
                                id: hostField
                                objectName: "serverHostField"
                                Layout.fillWidth: true
                                Layout.minimumWidth: Kirigami.Units.gridUnit * 7
                                placeholderText: i18n("Domain or IP address")
                                inputMethodHints: Qt.ImhUrlCharactersOnly | Qt.ImhNoPredictiveText
                                Accessible.name: i18n("Domain or IP address")
                                onAccepted: page.connectAddress(page.formAddress, true)
                            }
                            Controls.Label {
                                text: i18nc("@label:textbox", "Port:")
                            }
                            Controls.TextField {
                                id: portField
                                objectName: "serverPortField"
                                Layout.preferredWidth: Kirigami.Units.gridUnit * 6
                                placeholderText: i18nc("@info:placeholder the port field left empty", "Automatic")
                                validator: IntValidator { bottom: 1; top: 65535 }
                                inputMethodHints: Qt.ImhDigitsOnly
                                Accessible.name: i18n("Port")
                                onAccepted: page.connectAddress(page.formAddress, true)
                            }
                        }
                        Controls.Label {
                            Layout.fillWidth: true
                            text: i18n("HTTPS is tried before HTTP, and an empty port tries 443 and 8920, then 8096 and 80")
                            wrapMode: Text.Wrap
                            font.pointSize: Kirigami.Theme.smallFont.pointSize
                            opacity: 0.75
                        }
                        // What is wrong with the typed address, rather than a Connect that only turns grey
                        MessageBox {
                            objectName: "serverAddressProblem"
                            Layout.fillWidth: true
                            visible: page.addressProblemShown
                            type: Kirigami.MessageType.Warning
                            text: page.formPortValid ? i18n("Enter only the domain or IP address, without a scheme or a path")
                                                     : i18n("Enter a port from 1 to 65535, or leave it empty")
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Kirigami.Units.smallSpacing
                            Controls.Button {
                                id: testNewServer
                                objectName: "testNewServerButton"
                                text: i18n("Test")
                                icon.name: "network-server"
                                enabled: page.formAddress.length > 0 && !page.newServerStatus.testing && !page.busy && !page.confirming
                                onClicked: page.connection.testServer(page.formAddress, true)
                            }
                            Controls.BusyIndicator {
                                objectName: "newServerTesting"
                                visible: !!page.newServerStatus.testing
                                running: visible
                                Layout.preferredWidth: Kirigami.Units.iconSizes.smallMedium
                                Layout.preferredHeight: width
                                Accessible.name: i18nc("@info %1 is a server address", "Testing %1…", page.formAddress)
                            }
                            Controls.Button {
                                id: connectNewServer
                                objectName: "connectNewServerButton"
                                Layout.fillWidth: true
                                text: i18n("Connect")
                                icon.name: "network-connect"
                                enabled: page.formAddress.length > 0 && !page.busy && !page.confirming
                                onClicked: page.connectAddress(page.formAddress, true)
                            }
                        }
                        RowLayout {
                            visible: page.busy
                            FadeInOnShow {}
                            Controls.BusyIndicator { running: page.busy }
                            Controls.Label { text: i18n("Connecting…") }
                            Controls.Button {
                                text: i18n("Cancel")
                                onClicked: page.connection.cancel()
                            }
                        }
                    }
                }
            }

            Kirigami.Separator {
                objectName: "serversColumnSeparator"
                Layout.fillWidth: !page.splitLayout
                Layout.fillHeight: page.splitLayout
            }

            ColumnLayout {
                objectName: "savedServersColumn"
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignTop
                Layout.preferredWidth: page.splitLayout ? Math.min(page.width, page.maxContentWidth) * 0.42
                                                       : Math.max(0, Math.min(page.width - Kirigami.Units.largeSpacing * 2, page.maxContentWidth))
                spacing: Kirigami.Units.largeSpacing
                Kirigami.Heading { level: 2; text: i18n("Saved Servers") }
                Kirigami.PlaceholderMessage {
                    FadeInOnShow {}
                    Layout.fillWidth: true
                    visible: page.connection.servers.length === 0
                    text: i18n("No servers yet")
                    icon.name: "network-server"
                }
                Repeater {
                    model: page.connection.servers
                    delegate: Kirigami.AbstractCard {
                        id: saved
                        required property var modelData
                        readonly property var status: page.connection.serverReachability[modelData.url] ?? ({})
                        objectName: "savedServerEntry"
                        Layout.fillWidth: true
                        contentItem: ColumnLayout {
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
                                enabled: !page.busy && !page.confirming
                                Controls.Button {
                                    text: i18n("Connect")
                                    icon.name: "network-connect"
                                    Layout.fillWidth: true
                                    Accessible.name: i18nc("@action:button %1 is a server address", "Connect to %1", saved.modelData.url)
                                    onClicked: page.connectAddress(saved.modelData.url, false)
                                }
                                Controls.Button {
                                    objectName: "testSavedServerButton"
                                    text: i18n("Test")
                                    icon.name: "network-server"
                                    Accessible.name: i18nc("@action:button %1 is a server address", "Test %1", saved.modelData.url)
                                    enabled: !saved.status.testing
                                    onClicked: page.connection.testServer(saved.modelData.url)
                                }
                                Controls.BusyIndicator {
                                    visible: !!saved.status.testing
                                    running: visible
                                    Layout.preferredWidth: Kirigami.Units.iconSizes.smallMedium
                                    Layout.preferredHeight: width
                                    Accessible.name: i18nc("@info %1 is a server address", "Testing %1…", saved.modelData.url)
                                }
                                Controls.Button {
                                    // The word of the confirmation it opens
                                    text: i18n("Remove…")
                                    icon.name: "edit-delete-remove"
                                    Accessible.name: i18n("Remove server %1", saved.modelData.url)
                                    onClicked: {
                                        removeDialog.serverId = saved.modelData.serverId;
                                        removeDialog.host = saved.modelData.url;
                                        removeDialog.open();
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
