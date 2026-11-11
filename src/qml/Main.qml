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
    // A server is chosen while its sign-in or a session is in progress: the account menu shows for it
    readonly property bool serverChosen: connection.state !== ConnectionController.Idle
    // The header modes in pixels, from grid units: tests derive their window sizes from these (Compact below 24, Wide from 36)
    readonly property int compactLimit: Kirigami.Units.gridUnit * 24
    readonly property int wideLimit: Kirigami.Units.gridUnit * 36
    // The width decides the modes, read here where the width lives: a header-side binding on the
    // attached Window.window kept its load-time value and never followed a resize
    readonly property bool compactHeader: width < compactLimit
    readonly property bool wideHeader: width >= wideLimit

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

    // The bar with the menus and the Home tab; it spans the window above the pages
    header: ShellHeader {
        id: shellHeader

        shell: window
    }

    // Esc goes back like Alt+Left except on the first page; an open dialog takes the key first
    Shortcut {
        sequences: ["Escape"]
        enabled: window.pageStack.currentIndex > 0 && window.pageStack.layers.depth <= 1
        onActivated: window.pageStack.goBack()
    }
    Shortcut {
        sequences: [StandardKey.Find]
        enabled: window.signedIn
        onActivated: window.openSearch()
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
        if (!page || !activeFocusItem || openingSearchFromHeader || !focusIsIn(page))
            return;
        // The tab stop that holds the focus (a list, a grid, a field), not the tile inside it that comes and goes
        let control = activeFocusItem;
        while (control && control !== page && !control.activeFocusOnTab)
            control = control.parent;
        if (control && control !== page)
            lastFocus.set(page, control);
    }
    // True while the search page the header box opened takes its place: Kirigami's ScrollablePage
    // focuses its own scroll view while it is created, and neither that nor the page it belongs to may keep or remember the keyboard
    property bool openingSearchFromHeader: false
    function focusCurrentPage() {
        forgetLeftPages();
        const page = pageStack.currentItem;
        // Typing in the header box is never interrupted, not even by the page the typing itself opened
        if (!page || shellHeader.searchFieldHasFocus)
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
            // The search page leaving the stack takes its term with it: the header box empties with the page.
            // A page Back just left stays above the current one until the slide ends, and is not a page in the stack any more
            if (window.searchPageIndex() < 0 && window.connection.search.term.length > 0)
                window.connection.search.term = "";
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

    // The stack position of the search page up to the one in front, -1 when it is not there
    function searchPageIndex() {
        for (let i = 0; i <= pageStack.currentIndex; ++i) {
            if (pageStack.get(i)?.objectName === "searchPage")
                return i;
        }
        return -1;
    }

    // Everything above the first page goes
    function popToBase() {
        const base = pageStack.get(0);
        if (base)
            pageStack.pop(base);
    }

    // Ctrl+F: a search page already in the stack is brought back as it is, a new one starts from an empty term above Home.
    // The header box takes the focus (the page's own field in Compact mode, where the box hides)
    function openSearch() {
        if (connection.state !== ConnectionController.Authenticated)
            return;
        let page = null;
        const index = searchPageIndex();
        if (index >= 0) {
            page = pageStack.get(index);
            if (index < pageStack.currentIndex)
                pageStack.pop(page);
        } else {
            popToBase();
            connection.search.clear();
            page = pageStack.push(Qt.resolvedUrl("SearchPage.qml"), { connection: connection });
        }
        Qt.callLater(() => {
            if (compactHeader)
                page?.focusSearchField();
            else
                shellHeader.focusSearchField();
        });
    }

    // The first keystroke in the header box: the page opens above the current one, and the term the keystroke already set stays
    function openSearchIfMissing() {
        if (connection.state !== ConnectionController.Authenticated)
            return;
        const index = searchPageIndex();
        if (index >= 0) {
            if (index < pageStack.currentIndex)
                pageStack.pop(pageStack.get(index));
            return;
        }
        // While the box holds the keyboard, the page it opens must not take it: Kirigami's
        // ScrollablePage focuses its own scroll view while it is created, so the box takes it back after
        const wasTyping = shellHeader.searchFieldHasFocus;
        if (wasTyping) {
            openingSearchFromHeader = true;
            pageStack.push(Qt.resolvedUrl("SearchPage.qml"), { connection: connection });
            Qt.callLater(() => {
                shellHeader.refocusSearchField();
                openingSearchFromHeader = false;
            });
            return;
        }
        popToBase();
        pageStack.push(Qt.resolvedUrl("SearchPage.qml"), { connection: connection });
    }

    // Down from the header box goes to the first row of the results that has cards
    function searchResultsDown() {
        const page = pageStack.currentItem;
        if (page?.objectName === "searchPage")
            page.focusFirstResults();
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

    // The section the page in front belongs to: the header marks it (the Home tab, the main menu button, the queue button); a page opened from a library belongs to that library.
    // Settings and About need no session, so they are marked whatever the connection is
    readonly property string currentSection: {
        switch (pageStack.currentItem?.objectName) {
        case "settingsPage":
        case "settingsSectionPage":
            return "settings";
        case "aboutPage":
            return "about";
        }
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

    // A section opens above the first page: one already in the stack is brought back (what is above it goes), so the stack never grows with every click
    function openSection(name, url, properties) {
        for (let i = 0; i <= pageStack.currentIndex; ++i) {
            if (pageStack.get(i)?.objectName === name) {
                pageStack.pop(pageStack.get(i));
                return;
            }
        }
        popToBase();
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
