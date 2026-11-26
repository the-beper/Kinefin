// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Kirigami.ScrollablePage {
    id: page
    objectName: "loginPage"

    // The darker View background on every page: a page without a list (a plain flickable) would get the lighter Window one, and the pages would not match
    Kirigami.Theme.colorSet: Kirigami.Theme.View
    Kirigami.Theme.inherit: false
    // The lone menu button stands in the top corner over the page: the content starts below it
    topPadding: Kirigami.Units.gridUnit * 2

    required property ConnectionController connection

    // The row toolbar is ToolBar everywhere (Main.qml): the pre-signin screens keep their identity in the top bar instead
    globalToolBarStyle: Kirigami.ApplicationHeaderStyle.None

    readonly property bool signingIn: connection.state === ConnectionController.SigningIn
    readonly property bool quick: connection.state === ConnectionController.QuickConnectPending

    readonly property string usersTitle: i18nc("@title", "Users on This Server")

    title: i18n("Sign In")

    // The keyboard starts in the sign-in form: the password when the user is known from the picker, else the username. Quick Connect has no field to take
    function focusPrimary() {
        if (!page.quick)
            (username.text.length > 0 ? password : username).forceActiveFocus(Qt.TabFocusReason);
    }

    // A fresh login lists the users the server shows on its own sign-in screen
    Component.onCompleted: {
        if (page.connection.loginUserName.length === 0)
            page.connection.loadPublicUsers();
    }

    ColumnLayout {
        // One centred column of a fixed width: the heading, the tiles, the form and the actions share it
        ColumnLayout {
            id: column

            Layout.alignment: Qt.AlignHCenter | Qt.AlignTop
            Layout.fillWidth: true
            Layout.maximumWidth: Kirigami.Units.gridUnit * 26
            spacing: Kirigami.Units.smallSpacing

            MessageBox {
                Layout.bottomMargin: Kirigami.Units.largeSpacing
                visible: page.connection.errorText.length > 0
                type: Kirigami.MessageType.Error
                text: page.connection.errorText
            }

            // The heading and the address are one block, with room before what follows
            ColumnLayout {
                Layout.fillWidth: true
                Layout.bottomMargin: Kirigami.Units.gridUnit
                spacing: Kirigami.Units.smallSpacing

                Kirigami.Heading {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    level: 1
                    text: page.title
                }

                // The server address is server text: plain
                Controls.Label {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    textFormat: Text.PlainText
                    color: Kirigami.Theme.disabledTextColor
                    text: page.connection.serverUrl
                }
            }

            Kirigami.Heading {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                visible: publicUsers.visible
                level: 2
                text: page.usersTitle
            }

            // The users of the server that are not saved yet: a tile fills in the username and goes on to the password. One tab stop with the arrow keys inside, centred, wrapping when the column is narrower
            Item {
                id: publicUsers
                objectName: "publicUsersStrip"

                readonly property real gap: Kirigami.Units.largeSpacing
                readonly property real cellWidth: Kirigami.Units.gridUnit * 7 + gap
                readonly property bool rtl: Qt.application.layoutDirection === Qt.RightToLeft
                readonly property int count: publicTiles.count
                readonly property int maxColumns: Math.max(1, Math.floor((width + gap) / cellWidth))
                readonly property int columns: Math.max(1, Math.min(count, maxColumns))
                // Two rows of tiles, so that many public users do not push the form down; Show All lists the rest
                property bool expanded: false
                readonly property bool truncated: count > maxColumns * 2
                readonly property int shown: expanded ? count : Math.min(count, maxColumns * 2)
                property int currentIndex: 0
                property Item currentItem: null

                function refreshCurrent() {
                    currentItem = publicTiles.itemAt(currentIndex);
                }

                function moveTo(index) {
                    currentIndex = index;
                    refreshCurrent();
                    if (currentItem)
                        Spatial.reveal(currentItem);
                }

                Layout.fillWidth: true
                Layout.preferredHeight: publicFlow.implicitHeight
                visible: !page.quick && count > 0
                enabled: !page.signingIn
                activeFocusOnTab: true
                Accessible.role: Accessible.List
                Accessible.name: page.usersTitle
                onCurrentIndexChanged: refreshCurrent()
                // Collapsing never leaves the current tile hidden
                onShownChanged: {
                    if (shown > 0 && currentIndex >= shown)
                        moveTo(shown - 1);
                }

                // The keys act on the current tile
                Keys.onReturnPressed: currentItem?.clicked()
                Keys.onEnterPressed: currentItem?.clicked()
                Keys.onSpacePressed: currentItem?.clicked()
                Keys.onPressed: event => {
                    const last = shown - 1;
                    const row = Math.floor(currentIndex / columns);
                    const lastRow = Math.floor(last / columns);
                    let handled = last >= 0;
                    switch (event.key) {
                    case Qt.Key_Left:
                        moveTo(rtl ? Math.min(last, currentIndex + 1) : Math.max(0, currentIndex - 1));
                        break;
                    case Qt.Key_Right:
                        moveTo(rtl ? Math.max(0, currentIndex - 1) : Math.min(last, currentIndex + 1));
                        break;
                    case Qt.Key_Home:
                        moveTo(0);
                        break;
                    case Qt.Key_End:
                        moveTo(last);
                        break;
                    case Qt.Key_Up:
                        if (row > 0)
                            moveTo(currentIndex - columns);
                        else
                            handled = Spatial.move(publicUsers, Qt.Key_Up);
                        break;
                    case Qt.Key_Down:
                        // The last row leads to Show All, or the form
                        if (row < lastRow)
                            moveTo(Math.min(last, currentIndex + columns));
                        else
                            (showAll.visible ? showAll : username).forceActiveFocus(Qt.TabFocusReason);
                        break;
                    default:
                        handled = false;
                    }
                    event.accepted = handled;
                }

                Flow {
                    id: publicFlow
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: publicUsers.columns * publicUsers.cellWidth - publicUsers.gap
                    spacing: publicUsers.gap

                    Repeater {
                        id: publicTiles
                        model: page.connection.publicUsers
                        onItemAdded: Qt.callLater(publicUsers.refreshCurrent)
                        onItemRemoved: Qt.callLater(publicUsers.refreshCurrent)

                        delegate: UserTile {
                            id: userTile

                            required property int index
                            required property var modelData

                            objectName: "publicUser_" + modelData.id
                            name: modelData.name
                            imageUrl: modelData.imageUrl
                            size: Kirigami.Units.gridUnit * 7
                            visible: index < publicUsers.shown
                            accessibleLabel: i18nc("@label %1 is a user name", "Use %1", modelData.name)
                            highlighted: publicUsers.currentIndex === index && publicUsers.activeFocus

                            onPressed: {
                                publicUsers.currentIndex = userTile.index;
                                publicUsers.forceActiveFocus(Qt.MouseFocusReason);
                            }
                            onClicked: {
                                username.text = userTile.modelData.name;
                                password.forceActiveFocus(Qt.TabFocusReason);
                            }
                        }
                    }
                }
            }

            Controls.Button {
                id: showAll
                objectName: "publicUsersToggle"
                Layout.alignment: Qt.AlignHCenter
                visible: publicUsers.visible && publicUsers.truncated
                enabled: !page.signingIn
                flat: true
                text: publicUsers.expanded ? i18n("Show Less") : i18n("Show All")
                onClicked: publicUsers.expanded = !publicUsers.expanded
            }

            // The boxes and the button take the full width of the column, like the actions below; the labels hang in the free space at the left of the column, and stand above their boxes when there is not enough of it, as in a narrow form
            ColumnLayout {
                id: form

                // The room at the left of the column, labels included
                readonly property real labelRoom: (page.width - column.width) / 2 - Kirigami.Units.largeSpacing
                readonly property bool labelsFit: labelRoom >= Math.max(usernameLabel.implicitWidth, passwordLabel.implicitWidth)

                Layout.fillWidth: true
                Layout.topMargin: publicUsers.visible ? Kirigami.Units.largeSpacing : 0
                visible: !page.quick
                spacing: Kirigami.Units.largeSpacing

                // Without room at the left a label stands above its box, as in a narrow form
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Kirigami.Units.smallSpacing

                    Controls.Label {
                        objectName: "usernameAboveLabel"
                        visible: !form.labelsFit
                        text: usernameLabel.text
                    }
                    Controls.TextField {
                        id: username
                        objectName: "usernameField"
                        Layout.fillWidth: true
                        enabled: !page.signingIn
                        Accessible.name: i18n("Username")
                        onAccepted: password.forceActiveFocus()

                        Controls.Label {
                            id: usernameLabel
                            anchors.right: parent.left
                            anchors.rightMargin: Kirigami.Units.largeSpacing
                            anchors.verticalCenter: parent.verticalCenter
                            visible: form.labelsFit
                            horizontalAlignment: Text.AlignRight
                            text: i18n("Username:")
                        }
                        Keys.onUpPressed: event => {
                            event.accepted = publicUsers.visible;
                            if (event.accepted)
                                publicUsers.forceActiveFocus(Qt.BacktabFocusReason);
                        }
                        // A user known beforehand (from the picker) goes straight to the password
                        Component.onCompleted: {
                            text = page.connection.loginUserName;
                            (text.length > 0 ? password : username).forceActiveFocus();
                        }
                    }
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Kirigami.Units.smallSpacing

                    Controls.Label {
                        objectName: "passwordAboveLabel"
                        visible: !form.labelsFit
                        text: passwordLabel.text
                    }
                    Kirigami.PasswordField {
                        id: password
                        objectName: "passwordField"
                        Layout.fillWidth: true
                        enabled: !page.signingIn
                        Accessible.name: i18n("Password")
                        onAccepted: signIn.clicked()

                        Controls.Label {
                            id: passwordLabel
                            anchors.right: parent.left
                            anchors.rightMargin: Kirigami.Units.largeSpacing
                            anchors.verticalCenter: parent.verticalCenter
                            visible: form.labelsFit
                            horizontalAlignment: Text.AlignRight
                            text: i18n("Password:")
                        }
                    }
                }
                Controls.Button {
                    id: signIn
                    Layout.fillWidth: true
                    Layout.topMargin: Kirigami.Units.largeSpacing
                    text: i18n("Sign In")
                    icon.name: "user-identity"
                    // A Jellyfin account may have no password: only the name is required
                    enabled: !page.signingIn && username.text.length > 0
                    onClicked: page.connection.login(username.text, password.text)
                }
                Controls.BusyIndicator {
                    Layout.alignment: Qt.AlignHCenter
                    FadeInOnShow {}
                    visible: page.signingIn
                    running: page.signingIn
                }
            }

            // The secondary actions share the column width in equal cells; they stack when the column is too narrow for all of them
            GridLayout {
                id: secondary

                // The width the visible buttons need side by side, all as wide as the widest
                readonly property real neededWidth: {
                    let widest = 0;
                    let shown = 0;
                    for (const child of children) {
                        if (child.visible) {
                            widest = Math.max(widest, child.implicitWidth);
                            ++shown;
                        }
                    }
                    return widest * shown + Math.max(0, shown - 1) * columnSpacing;
                }

                Layout.fillWidth: true
                Layout.topMargin: Kirigami.Units.largeSpacing
                visible: !page.quick
                uniformCellWidths: true
                columns: neededWidth <= column.width ? Math.max(1, children.length) : 1
                columnSpacing: Kirigami.Units.largeSpacing
                rowSpacing: Kirigami.Units.smallSpacing

                Controls.Button {
                    Layout.fillWidth: true
                    text: i18n("Use Quick Connect")
                    icon.name: "smartphone"
                    enabled: !page.signingIn
                    onClicked: page.connection.startQuickConnect()
                }
                Controls.Button {
                    Layout.fillWidth: true
                    text: i18nc("@action:button Opens the list of the saved users of this server", "Choose Another User")
                    icon.name: "system-switch-user"
                    visible: page.connection.hasSavedUsers
                    enabled: !page.signingIn
                    onClicked: page.connection.chooseAnotherUser()
                }
                Controls.Button {
                    Layout.fillWidth: true
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
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    text: i18n("On a device that is already signed in, open Quick Connect in the Jellyfin settings and enter this code")
                }
                Kirigami.Heading {
                    Layout.alignment: Qt.AlignHCenter
                    level: 2
                    font.letterSpacing: Kirigami.Units.largeSpacing
                    textFormat: Text.PlainText
                    text: page.connection.quickConnectCode
                    Accessible.name: i18n("Quick Connect code %1", page.connection.quickConnectCode)
                }
                RowLayout {
                    Layout.alignment: Qt.AlignHCenter
                    Controls.BusyIndicator {
                        FadeInOnShow {}
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
}
