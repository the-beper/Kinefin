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
    // An item link from the command line: opened once Home shows, then cleared
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

    // Auto resolves to the Breadcrumb header only on mobile in narrow mode, and that one renders titles without plain-text formatting: the pin keeps server-supplied page titles in the toolbar title delegate, which is plain text
    pageStack.globalToolBar.style: Kirigami.ApplicationHeaderStyle.ToolBar

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
            break;
        }
        pageStack.clear();
        const shown = pageStack.push(Qt.resolvedUrl(page), properties);
        openRequestedItem(shown);
    }

    // The only way to the details of an item: the pages above the one asking give way, and the item already showing is not pushed twice
    function openItem(itemId, fromPage) {
        if (connection.state !== ConnectionController.Authenticated)
            return;
        if (fromPage)
            pageStack.pop(fromPage);
        const current = pageStack.currentItem;
        if (current?.objectName === "itemDetailsPage" && sameItem(current.itemId, itemId))
            return;
        pageStack.push(Qt.resolvedUrl("ItemDetailsPage.qml"), { connection: connection, itemId: itemId });
    }

    // Goes back to an item page already in the stack below the one asking, else opens the item like openItem
    // The same GUID is written with and without dashes, in any case
    function sameItem(first, second) {
        return first.replace(/-/g, "").toLowerCase() === second.replace(/-/g, "").toLowerCase();
    }

    function openAncestor(itemId, fromPage) {
        if (connection.state !== ConnectionController.Authenticated)
            return;
        for (let i = 0; i < pageStack.depth; ++i) {
            const candidate = pageStack.get(i);
            if (candidate === fromPage)
                break;
            if (candidate?.objectName === "itemDetailsPage" && sameItem(candidate.itemId, itemId)) {
                pageStack.pop(candidate);
                return;
            }
        }
        openItem(itemId, fromPage);
    }

    // The search page is the one place the term lives: an open one is brought back as it is, a new one starts from an empty term above Home
    function openSearch() {
        if (connection.state !== ConnectionController.Authenticated)
            return;
        let page = null;
        for (let i = 0; i < pageStack.depth; ++i) {
            if (pageStack.get(i)?.objectName === "searchPage") {
                page = pageStack.get(i);
                break;
            }
        }
        if (page) {
            pageStack.pop(page);
        } else {
            const first = pageStack.get(0);
            if (first)
                pageStack.pop(first);
            connection.search.clear();
            page = pageStack.push(Qt.resolvedUrl("SearchPage.qml"), { connection: connection });
        }
        Qt.callLater(() => page?.focusSearchField());
    }

    // The genre rails of the open library go above the library page that asks, which stays below
    function openGenres(fromPage) {
        if (connection.state !== ConnectionController.Authenticated)
            return;
        if (!connection.openGenreRails())
            return;
        if (fromPage)
            pageStack.pop(fromPage);
        pageStack.push(Qt.resolvedUrl("GenreRailsPage.qml"), { connection: connection });
    }

    // A link opens its item above Home once signed in, whether it came before or after the sign-in
    function openRequestedItem(home) {
        if (requestedItemId.length === 0 || connection.state !== ConnectionController.Authenticated || home?.objectName !== "homePage")
            return;
        const itemId = requestedItemId;
        requestedItemId = "";
        openItem(itemId, home);
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
    onPageGroupChanged: {
        // A link waiting for a sign-in belongs to the server the user is leaving
        if (pageGroup === 0)
            requestedItemId = "";
        showPageForState();
    }

    // A link that arrives while Home is already showing (a second launch) opens at once
    onRequestedItemIdChanged: openRequestedItem(pageStack.get(0))

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
                text: i18n("Search")
                icon.name: "system-search"
                shortcut: StandardKey.Find
                visible: window.signedIn
                onTriggered: window.openSearch()
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
