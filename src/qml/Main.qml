// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import org.kde.kirigami as Kirigami

Kirigami.ApplicationWindow {
    id: window

    // The connection to the Jellyfin server, created by the application
    required property ConnectionController connection
    required property ClientSettings settings
    // "fullscreen" or "windowed" when a command line flag decides this run; empty to follow the setting. Changing the setting from the Settings page ends the override
    property string startupWindowMode: ""
    // "settings" or "about": a page to open right after the shell loads (the smoke test uses it to render pages no server is needed for)
    property string startupPage: ""
    property string requestedItemId: ""
    readonly property bool signedIn: connection.state === ConnectionController.Authenticated

    property bool fullscreenSetting: settings.value("main", "fullscreen")
    property bool alwaysOnTopSetting: settings.value("main", "alwaysOnTop")
    property string fullscreenOutput: settings.value("main", "forceFSScreen")
    readonly property bool wantsFullscreen: startupWindowMode === "fullscreen" || (startupWindowMode !== "windowed" && fullscreenSetting)

    width: 1000
    height: 640
    visible: true
    title: "Kinefin"
    flags: alwaysOnTopSetting ? (Qt.Window | Qt.WindowStaysOnTopHint) : Qt.Window

    Connections {
        target: window.settings
        function onValueChanged(section, key) {
            if (section !== "main")
                return;
            window.fullscreenSetting = window.settings.value("main", "fullscreen");
            if (key === "fullscreen")
                window.startupWindowMode = "";
            window.alwaysOnTopSetting = window.settings.value("main", "alwaysOnTop");
            window.fullscreenOutput = window.settings.value("main", "forceFSScreen");
        }
        function onSaveFailed() {
            window.showPassiveNotification(i18n("The settings could not be saved: check the free space and the permissions of the profile folder. Changes last until Kinefin closes"));
        }
    }

    // Full screen goes to the output named by "Force Full Screen Display" when that output exists
    function applyFullscreen() {
        if (wantsFullscreen) {
            const output = Application.screens.find(screen => screen.name === fullscreenOutput);
            if (output)
                window.screen = output;
            window.visibility = Window.FullScreen;
        } else if (window.visibility === Window.FullScreen) {
            window.visibility = Window.Windowed;
        }
    }
    onWantsFullscreenChanged: applyFullscreen()
    onFullscreenOutputChanged: if (wantsFullscreen) applyFullscreen()

    // The last window size comes back always; the position only where the compositor lets a client place its window (not Wayland) and only while it still lies on a screen
    function restoreGeometry() {
        const saved = settings.windowGeometry();
        if (saved.width <= 0)
            return;
        // A size saved on a larger screen must not outgrow this one
        window.width = Math.min(saved.width, window.screen.desktopAvailableWidth);
        window.height = Math.min(saved.height, window.screen.desktopAvailableHeight);
        if (Qt.platform.pluginName === "wayland")
            return;
        const onScreen = Application.screens.some(screen => saved.x >= screen.virtualX && saved.y >= screen.virtualY && saved.x < screen.virtualX + screen.width && saved.y < screen.virtualY + screen.height);
        if (onScreen) {
            window.x = saved.x;
            window.y = saved.y;
        }
    }

    onClosing: {
        // A full-screen or maximized window has no geometry of its own to keep
        if (window.visibility === Window.Windowed)
            settings.setWindowGeometry(Qt.rect(x, y, width, height));
    }

    // The page follows the connection state: Servers until a server is chosen, sign-in until there is a session, Home after
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

    // The sign-in page keeps its place while the state moves between its own steps
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

    // A link that arrives while Home is already showing (a second launch) updates that page too
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
                text: i18n("Open Server Web Interface")
                icon.name: "internet-services"
                visible: window.signedIn
                onTriggered: {
                    if (!window.connection.openServerWebInterface())
                        window.showPassiveNotification(i18n("The server web interface could not be opened: check the default browser"));
                }
            },
            Kirigami.Action {
                text: i18n("Settings")
                icon.name: "configure"
                onTriggered: window.pushOnce("settingsPage", "SettingsPage.qml", { settings: window.settings })
            },
            Kirigami.Action {
                text: i18n("About Kinefin")
                icon.name: "help-about"
                onTriggered: window.pushOnce("aboutPage", "AboutPage.qml", {})
            },
            Kirigami.Action {
                text: i18n("Quit")
                icon.name: "application-exit"
                shortcut: StandardKey.Quit
                onTriggered: window.close()
            }
        ]
    }

    // Opens a page above the current one unless that page is already showing
    function pushOnce(name, url, properties) {
        if (pageStack.currentItem?.objectName !== name)
            pageStack.push(Qt.resolvedUrl(url), properties);
    }

    // Pushing before the window's page row is complete leaves the page outside the scene
    function openStartupPage() {
        if (startupPage === "about") {
            pushOnce("aboutPage", "AboutPage.qml", {});
        } else if (startupPage === "settings") {
            const page = pageStack.push(Qt.resolvedUrl("SettingsPage.qml"), { settings: settings });
            for (const entry of page.sections)
                pageStack.push(Qt.resolvedUrl("SettingsSectionPage.qml"), { settings: settings, section: entry.section, title: entry.title });
        }
    }

    Component.onCompleted: {
        if (wantsFullscreen)
            applyFullscreen();
        else
            restoreGeometry();
        Qt.callLater(() => {
            showPageForState();
            connection.start();
            openStartupPage();
        });
    }
}
