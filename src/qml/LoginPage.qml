// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Kirigami.ScrollablePage {
    id: page

    required property ConnectionController connection
    readonly property bool signingIn: connection.state === ConnectionController.SigningIn
    readonly property bool quick: connection.state === ConnectionController.QuickConnectPending

    title: i18n("Sign In")

    ColumnLayout {
        spacing: Kirigami.Units.largeSpacing

        MessageBox {
            visible: page.connection.errorText.length > 0
            type: Kirigami.MessageType.Error
            text: page.connection.errorText
        }

        Controls.Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
            text: i18n("Server: %1", page.connection.serverUrl)
        }

        Kirigami.FormLayout {
            Layout.fillWidth: true
            visible: !page.quick

            Controls.TextField {
                id: username
                Kirigami.FormData.label: i18n("Username:")
                enabled: !page.signingIn
                Accessible.name: i18n("Username")
                onAccepted: password.forceActiveFocus()
                Component.onCompleted: forceActiveFocus()
            }
            Kirigami.PasswordField {
                id: password
                Kirigami.FormData.label: i18n("Password:")
                enabled: !page.signingIn
                Accessible.name: i18n("Password")
                onAccepted: signIn.clicked()
            }
            RowLayout {
                Controls.Button {
                    id: signIn
                    text: i18n("Sign In")
                    icon.name: "user-identity"
                    enabled: !page.signingIn && username.text.length > 0 && password.text.length > 0
                    onClicked: page.connection.login(username.text, password.text)
                }
                Controls.BusyIndicator {
                    visible: page.signingIn
                    running: page.signingIn
                }
            }
            Controls.Button {
                text: i18n("Use Quick Connect")
                icon.name: "smartphone"
                enabled: !page.signingIn
                onClicked: page.connection.startQuickConnect()
            }
            Controls.Button {
                text: i18n("Use Another Server")
                icon.name: "go-previous"
                enabled: !page.signingIn
                onClicked: page.connection.cancel()
            }
        }

        ColumnLayout {
            visible: page.quick
            Layout.fillWidth: true
            spacing: Kirigami.Units.largeSpacing

            Controls.Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: i18n("On a device that is already signed in, open Quick Connect in the Jellyfin settings and enter this code")
            }
            Kirigami.Heading {
                Layout.alignment: Qt.AlignHCenter
                level: 1
                font.letterSpacing: Kirigami.Units.largeSpacing
                textFormat: Text.PlainText
                text: page.connection.quickConnectCode
                Accessible.name: i18n("Quick Connect code %1", page.connection.quickConnectCode)
            }
            RowLayout {
                Layout.alignment: Qt.AlignHCenter
                Controls.BusyIndicator {
                    running: page.quick
                }
                Controls.Label {
                    text: i18n("Waiting for approval…")
                }
            }
            Controls.Button {
                Layout.alignment: Qt.AlignHCenter
                text: i18n("Cancel")
                onClicked: page.connection.cancelQuickConnect()
            }
        }
    }
}
