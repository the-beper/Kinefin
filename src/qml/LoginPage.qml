// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kirigamiaddons.components as Components

Kirigami.ScrollablePage {
    id: page
    objectName: "loginPage"

    required property ConnectionController connection
    readonly property bool signingIn: connection.state === ConnectionController.SigningIn
    readonly property bool quick: connection.state === ConnectionController.QuickConnectPending

    readonly property string usersTitle: i18nc("@title", "Users on This Server")

    title: i18n("Sign In")

    // A fresh login lists the users the server shows on its own sign-in screen
    Component.onCompleted: {
        if (page.connection.loginUserName.length === 0)
            page.connection.loadPublicUsers();
    }

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

        Kirigami.Heading {
            visible: publicUsers.visible
            level: 2
            text: page.usersTitle
        }

        // The users of the server that are not saved yet: a tile fills in the username and goes on to the password
        ListView {
            id: publicUsers
            objectName: "publicUsersStrip"

            Layout.fillWidth: true
            Layout.preferredHeight: Kirigami.Units.iconSizes.huge + Kirigami.Units.gridUnit * 3
            visible: !page.quick && count > 0
            enabled: !page.signingIn
            orientation: ListView.Horizontal
            spacing: Kirigami.Units.smallSpacing
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            model: page.connection.publicUsers
            activeFocusOnTab: true
            keyNavigationEnabled: true
            currentIndex: 0
            Accessible.role: Accessible.List
            Accessible.name: page.usersTitle

            // The tiles are not tab stops: the list is, and it takes the current user
            Keys.onReturnPressed: currentItem?.choose()
            Keys.onEnterPressed: currentItem?.choose()
            Keys.onSpacePressed: currentItem?.choose()

            delegate: Controls.ItemDelegate {
                id: userTile

                required property int index
                required property var modelData

                function choose() {
                    username.text = userTile.modelData.name;
                    password.forceActiveFocus(Qt.TabFocusReason);
                }

                objectName: "publicUser_" + modelData.id
                width: Kirigami.Units.gridUnit * 7
                height: publicUsers.height
                focusPolicy: Qt.NoFocus
                highlighted: ListView.isCurrentItem && publicUsers.activeFocus
                Accessible.name: i18nc("@label %1 is a user name", "Use %1", modelData.name)
                onPressed: {
                    publicUsers.currentIndex = userTile.index;
                    publicUsers.forceActiveFocus(Qt.MouseFocusReason);
                }
                onClicked: choose()

                contentItem: ColumnLayout {
                    spacing: Kirigami.Units.smallSpacing

                    Components.Avatar {
                        Layout.alignment: Qt.AlignHCenter
                        Layout.preferredWidth: Kirigami.Units.iconSizes.huge
                        Layout.preferredHeight: Kirigami.Units.iconSizes.huge
                        // The tile names the user to a screen reader
                        Accessible.ignored: true
                        name: userTile.modelData.name
                        // Only URLs built by JellyfinClient::userImageUrl belong here: a raw server string must never become a source
                        source: userTile.modelData.imageUrl
                        // The image is decoded at the size it shows
                        sourceSize.width: width * Screen.devicePixelRatio
                        sourceSize.height: height * Screen.devicePixelRatio
                    }

                    // User names come from the server: plain text only
                    Controls.Label {
                        Layout.fillWidth: true
                        horizontalAlignment: Text.AlignHCenter
                        elide: Text.ElideRight
                        maximumLineCount: 1
                        textFormat: Text.PlainText
                        text: userTile.modelData.name
                    }
                }
            }
        }

        Kirigami.FormLayout {
            Layout.fillWidth: true
            visible: !page.quick

            Controls.TextField {
                id: username
                objectName: "usernameField"
                Kirigami.FormData.label: i18n("Username:")
                enabled: !page.signingIn
                Accessible.name: i18n("Username")
                onAccepted: password.forceActiveFocus()
                // A user known beforehand (from the picker) goes straight to the password
                Component.onCompleted: {
                    text = page.connection.loginUserName;
                    (text.length > 0 ? password : username).forceActiveFocus();
                }
            }
            Kirigami.PasswordField {
                id: password
                objectName: "passwordField"
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
                text: i18nc("@action:button Opens the list of the saved users of this server", "Choose Another User")
                icon.name: "system-switch-user"
                visible: page.connection.hasSavedUsers
                enabled: !page.signingIn
                onClicked: page.connection.chooseAnotherUser()
            }
            Controls.Button {
                text: i18nc("@action:button Goes back to the list of servers", "Switch Server")
                icon.name: "network-server"
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
