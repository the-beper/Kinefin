// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQml.Models
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
    // One page at a time: the default switches to side-by-side columns from 40 grid units, which pushes the pages beyond the window
    pageStack.columnView.columnResizeMode: Kirigami.ColumnView.SingleColumn
    // Back removes the page it leaves, so its models give their listing channels back
    pageStack.popHiddenPages: true
    pageStack.globalToolBar.showNavigationButtons: Kirigami.ApplicationHeaderStyle.ShowBackButton

    // Esc goes back like Alt+Left except on the first page; a dialog or an open modal drawer takes the key first
    readonly property bool modalDrawerOpen: !!globalDrawer && globalDrawer.modal && globalDrawer.drawerOpen
    Shortcut {
        sequences: ["Escape"]
        enabled: window.pageStack.currentIndex > 0 && window.pageStack.layers.depth <= 1 && !window.modalDrawerOpen
        onActivated: window.pageStack.goBack()
    }

    // The page that comes to the front takes the keyboard focus, so Tab and the arrows work without a mouse click first; a page that already focused one of its own fields (sign-in, search) keeps that focus
    function focusIsIn(page) {
        for (let item = window.activeFocusItem; item; item = item.parent) {
            if (item === page)
                return true;
        }
        return false;
    }
    // The control each page last had the focus in: Back returns to it, on the tile that was opened; entries of pages that left the stack are dropped
    readonly property var lastFocus: new Map()
    function forgetLeftPages() {
        const pages = new Set();
        for (let i = 0; i < pageStack.depth; ++i)
            pages.add(pageStack.get(i));
        for (const page of Array.from(lastFocus.keys())) {
            if (!pages.has(page))
                lastFocus.delete(page);
        }
    }
    onActiveFocusItemChanged: {
        const page = pageStack.currentItem;
        if (!page || !activeFocusItem || !focusIsIn(page))
            return;
        // The tab stop that holds the focus (a list, a grid, a field), not the tile inside it that comes and goes
        let control = activeFocusItem;
        while (control && control !== page && !control.activeFocusOnTab)
            control = control.parent;
        if (control && control !== page)
            lastFocus.set(page, control);
    }
    function focusCurrentPage() {
        forgetLeftPages();
        const page = pageStack.currentItem;
        if (!page || modalDrawerOpen)
            return;
        // Back goes to the control the page last had the focus in, a new page to its main control, any other page keeps a field it focused itself
        const remembered = lastFocus.get(page);
        if (remembered?.visible && remembered.enabled)
            remembered.forceActiveFocus(Qt.TabFocusReason);
        else if (page.focusPrimary)
            page.focusPrimary();
        else if (!focusIsIn(page))
            page.forceActiveFocus();
    }
    Connections {
        target: window.pageStack
        function onCurrentItemChanged() {
            Qt.callLater(window.focusCurrentPage);
        }
    }

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
        // A page Back just left stays above the current one until the slide ends: it is not a page to bring back
        for (let i = 0; i <= pageStack.currentIndex; ++i) {
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

    // The section the page in front belongs to: the sidebar marks it; a page opened from a library belongs to that library
    readonly property string currentSection: {
        if (!signedIn)
            return "";
        for (let i = 0; i <= pageStack.currentIndex; ++i) {
            if (pageStack.get(i)?.objectName === "libraryPage")
                return "library:" + connection.library.libraryId;
        }
        switch (pageStack.currentItem?.objectName) {
        case "searchPage":
            return "search";
        case "queuePage":
            return "queue";
        case "settingsPage":
        case "settingsSectionPage":
            return "settings";
        case "aboutPage":
            return "about";
        default:
            return "home";
        }
    }

    // Home is the first page: everything above it is dropped, and Home keeps its scroll position and focus
    function goHome() {
        const home = pageStack.get(0);
        if (home?.objectName === "homePage")
            pageStack.pop(home);
        else
            showPageForState();
    }

    // A library above Home: one already open gives way to it, and is popped before the shared model switches to the new library
    function openLibrary(libraryId, libraryName) {
        if (!signedIn)
            return;
        // The library already open is not opened again: filters, scroll and the pages above it would be lost
        for (let i = 0; i <= pageStack.currentIndex; ++i) {
            if (pageStack.get(i)?.objectName === "libraryPage" && connection.library.libraryId === libraryId) {
                pageStack.pop(pageStack.get(i));
                return;
            }
        }
        const home = pageStack.get(0);
        if (home)
            pageStack.pop(home);
        if (connection.openLibrary(libraryId))
            pageStack.push(Qt.resolvedUrl("LibraryPage.qml"), { model: connection.library, libraryName: libraryName });
    }

    // A library name comes from the server and the sidebar renders an action's text as rich text after the mnemonic pass: a doubled ampersand shows as one, and without any "<" (shown as a single angle quotation mark) there is no tag to interpret
    function sidebarLabel(name) {
        // An ampersand that starts something like an entity would turn into a character when Alt is held and the label becomes rich text: it takes the full-width form
        return name.replace(/&(?=#|[A-Za-z0-9]+;)/g, "\uFF06").replace(/&/g, "&&").replace(/</g, "\u2039");
    }

    // What the queue did with an add, for the page that asked: the page itself shows nothing
    Connections {
        target: window.connection.queue
        function onAdded(count, truncated) {
            window.showPassiveNotification(truncated ? i18n("The queue is full: some items were left out, remove some to add more") : i18np("%1 item added to the queue", "%1 items added to the queue", count));
        }
        function onNothingToAdd() {
            window.showPassiveNotification(i18n("There is nothing to play in this item"));
        }
        function onSaveFailed() {
            window.showPassiveNotification(i18n("The queue could not be saved: check the free space and the permissions of the profile folder. It lasts until Kinefin closes"));
        }
        function onAddFailed() {
            window.showPassiveNotification(i18n("Could not add this to the queue: check the connection to the server and try again"));
        }
    }

    Shortcut {
        sequences: [StandardKey.Quit]
        onActivated: window.close()
    }

    Kirigami.Action {
        id: homeAction
        text: i18n("Home")
        icon.name: "go-home"
        enabled: window.signedIn
        checked: window.currentSection === "home"
        onTriggered: window.goHome()
    }
    Kirigami.Action {
        id: searchAction
        text: i18n("Search")
        icon.name: "system-search"
        shortcut: StandardKey.Find
        enabled: window.signedIn
        checked: window.currentSection === "search"
        onTriggered: window.openSearch()
    }
    Kirigami.Action {
        id: queueAction
        text: i18n("Queue")
        icon.name: "view-media-playlist"
        enabled: window.signedIn
        checked: window.currentSection === "queue"
        onTriggered: window.openSection("queuePage", "QueuePage.qml", { connection: window.connection })
    }
    // One entry per library of the server, below Home and Search
    property var libraryActions: []
    // The drawer drops its rows for the old actions first and the actions are destroyed a moment later: an action that dies while a row still shows it makes Kirigami's row read a null
    property var sidebarLibraries: []
    readonly property var librariesNow: signedIn ? connection.libraries : []
    onLibrariesNowChanged: {
        // The same list again (a Home retry) keeps the rows and the focus in them
        if (JSON.stringify(librariesNow) === JSON.stringify(sidebarLibraries))
            return;
        libraryActions = [];
        libraryTimer.restart();
    }
    Timer {
        id: libraryTimer
        interval: 100
        onTriggered: window.sidebarLibraries = window.librariesNow
    }
    Instantiator {
        model: window.sidebarLibraries
        delegate: Kirigami.Action {
            required property var modelData
            text: window.sidebarLabel(modelData.name)
            icon.name: "folder-video"
            enabled: modelData.browsable
            checked: window.currentSection === "library:" + modelData.id
            onTriggered: window.openLibrary(modelData.id, modelData.name)
        }
        onObjectAdded: (index, object) => {
            const list = window.libraryActions.slice();
            list.splice(index, 0, object);
            window.libraryActions = list;
        }
        onObjectRemoved: (index, object) => {
            const list = window.libraryActions.slice();
            list.splice(index, 1);
            window.libraryActions = list;
        }
    }
    Kirigami.Action {
        id: librariesSeparator
        separator: true
        visible: window.libraryActions.length > 0
    }
    Kirigami.Action {
        id: settingsAction
        text: i18n("Settings")
        icon.name: "configure"
        checked: window.currentSection === "settings"
        onTriggered: window.openSection("settingsPage", "SettingsPage.qml", { settings: window.settings })
    }
    Kirigami.Action {
        id: aboutAction
        text: i18n("About Kinefin")
        icon.name: "help-about"
        checked: window.currentSection === "about"
        onTriggered: window.openSection("aboutPage", "AboutPage.qml", {})
    }
    Kirigami.Action {
        id: serverSeparator
        separator: true
        visible: window.connection.state !== ConnectionController.Idle
    }
    Kirigami.Action {
        id: adminDashboardAction
        text: i18n("Admin Dashboard")
        icon.name: "preferences-system-network"
        visible: window.signedIn
        onTriggered: {
            if (!window.connection.openAdminDashboard())
                window.showPassiveNotification(i18n("The admin dashboard could not be opened: check the default browser"));
        }
    }
    Kirigami.Action {
        id: switchServerAction
        text: i18n("Switch Server")
        icon.name: "network-server"
        visible: window.connection.state !== ConnectionController.Idle
        onTriggered: window.connection.cancel()
    }
    Kirigami.Action {
        id: signOutAction
        text: i18n("Sign Out")
        icon.name: "system-log-out"
        visible: window.signedIn
        onTriggered: window.connection.signOut()
    }

    // The sections sit in a sidebar next to the pages while the window is wide, and in a drawer behind a handle when it is narrow; the width alone decides, so a window launched narrow and one shrunk to it look the same
    readonly property bool sidebarFits: width >= Kirigami.Units.gridUnit * 36
    pageStack.leftSidebar: globalDrawer
    globalDrawer: Kirigami.GlobalDrawer {
        title: "Kinefin"
        // Wide enough for the longest entry without eliding it
        width: Kirigami.Units.gridUnit * 15
        modal: !window.sidebarFits
        // Kirigami leaves the focus on the page when a modal drawer opens, and Esc would never reach the drawer
        focus: modal && drawerOpen
        // Kirigami's own binding of drawerOpen to the mode is lost once the drawer was opened or closed by hand: the sidebar is open, the drawer closed
        onModalChanged: {
            drawerOpen = !modal;
            Qt.callLater(window.focusCurrentPage);
        }
        actions: [homeAction, searchAction, queueAction, librariesSeparator, ...window.libraryActions, settingsAction, aboutAction, serverSeparator, adminDashboardAction, switchServerAction, signOutAction]
    }

    // A section opens above the first page: one already in the stack is brought back (what is above it goes), so the stack never grows with every click
    function openSection(name, url, properties) {
        for (let i = 0; i <= pageStack.currentIndex; ++i) {
            if (pageStack.get(i)?.objectName === name) {
                pageStack.pop(pageStack.get(i));
                return;
            }
        }
        const base = pageStack.get(0);
        if (base)
            pageStack.pop(base);
        pageStack.push(Qt.resolvedUrl(url), properties);
    }

    // Pushing before the window's page row is complete leaves the page outside the scene
    function openStartupPage() {
        if (startupPage === "about") {
            openSection("aboutPage", "AboutPage.qml", {});
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
