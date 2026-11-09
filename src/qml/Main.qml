// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import org.kde.kirigami as Kirigami

Kirigami.ApplicationWindow {
    id: window

    // The connection to the Jellyfin server, created by the application.
    required property ConnectionController connection
    property string requestedItemId: ""
    readonly property bool signedIn: connection.state === ConnectionController.Authenticated

    width: 1000
    height: 640
    visible: true
    title: "Kinefin"

    // The page follows the connection state: Servers until a server is chosen, sign-in until there is a session, Home after.
    function showPageForState() {
        let page = "ServersPage.qml";
        let properties = { connection: connection };
        switch (connection.state) {
        case ConnectionController.NeedsLogin:
        case ConnectionController.SigningIn:
        case ConnectionController.QuickConnectPending:
            page = "LoginPage.qml";
            break;
        case ConnectionController.Authenticated:
            page = "HomePage.qml";
            properties.requestedItemId = requestedItemId;
            break;
        }
        pageStack.clear();
        pageStack.push(Qt.resolvedUrl(page), properties);
    }

    // The sign-in page keeps its place while the state moves between its own steps.
    readonly property int pageGroup: {
        switch (connection.state) {
        case ConnectionController.NeedsLogin:
        case ConnectionController.SigningIn:
        case ConnectionController.QuickConnectPending:
            return 1;
        case ConnectionController.Authenticated:
            return 2;
        default:
            return 0;
        }
    }
    onPageGroupChanged: showPageForState()

    // A link that arrives while Home is already showing (a second launch) updates that page too.
    onRequestedItemIdChanged: {
        const home = pageStack.get(0);
        if (home && home.objectName === "homePage")
            home.requestedItemId = requestedItemId;
    }

    globalDrawer: Kirigami.GlobalDrawer {
        title: "Kinefin"
        actions: [
            Kirigami.Action {
                text: i18n("Home")
                icon.name: "go-home"
                visible: window.signedIn
                onTriggered: window.showPageForState()
            },
            Kirigami.Action {
                text: i18n("Switch Server")
                icon.name: "network-server"
                visible: window.connection.state !== ConnectionController.Idle
                onTriggered: window.connection.cancel()
            },
            Kirigami.Action {
                text: i18n("Sign Out")
                icon.name: "system-log-out"
                visible: window.signedIn
                onTriggered: window.connection.signOut()
            },
            Kirigami.Action {
                text: i18n("Settings")
                icon.name: "configure"
                onTriggered: {
                    if (window.pageStack.currentItem?.objectName !== "settingsPage")
                        window.pageStack.push(settingsPage);
                }
            }
        ]
    }

    Component {
        id: settingsPage
        Kirigami.Page {
            objectName: "settingsPage"
            title: i18n("Settings")
            Kirigami.PlaceholderMessage {
                anchors.centerIn: parent
                width: parent.width - Kirigami.Units.gridUnit * 4
                text: i18n("Settings are not available yet")
                icon.name: "configure"
            }
        }
    }

    // Pushing before the window's page row is complete leaves the page outside the scene.
    Component.onCompleted: Qt.callLater(() => {
        showPageForState();
        connection.start();
    })
}
