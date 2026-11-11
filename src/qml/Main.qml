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
    // The tab the first page of the stack stands for: "home", "movies" or "tvshows"; empty on the sign-in and server pages and for a library opened from a tile. The depth and the current item change with every push and clear, and the first page is read after them
    readonly property string rootTab: pageStack.depth > 0 && pageStack.currentItem ? tabOfPage(pageStack.get(0)) : ""
    // The scroll position and the current tile of each tab left behind, so a tab comes back as the user left it; dropped with the session
    readonly property var tabState: ({})
    // The type tabs showing a Recently Added list: what the tab saved on leaving belongs to that list, not to the stored view it comes back to
    readonly property var oneOffTabs: ({})
    // The Continue Watching pages alive: the list is closed when the last one goes, not by a page that outlives its successor
    property int resumePages: 0
    // The Favorites pages alive, counted the same way: the scan stops when the last one goes, not by a page that outlives its successor
    property int favoritesPages: 0

    property bool fullscreenSetting: settings.value("main", "fullscreen")
    property bool alwaysOnTopSetting: settings.value("main", "alwaysOnTop")
    property string fullscreenOutput: settings.value("main", "forceFSScreen")
    property bool newBadgeEnabled: settings.value("library", "newBadge")
    // Which library sub-tabs are switched on, by setting id without the "tabs." prefix (e.g. "movies.favorites"); replaced as a whole on a change so bindings re-evaluate
    property var subTabs: readSubTabs()
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

    // The back button of the mouse goes back like Esc; the missing forward button is a deliberate exception
    MouseNavigation {
        objectName: "mouseNavigation"
        target: window
        onBackRequested: window.goBackFromMouse()
    }
    function goBackFromMouse() {
        const index = pageStack.currentIndex;
        if (index < 1 || pageStack.layers.depth > 1)
            return;
        const leaving = pageStack.get(index);
        // The page leaves the stack at once: going back by moving the index leaves it there until a scroll animation ends
        const target = pageStack.get(index - 1);
        const leave = () => {
            if (pageStack.get(index) === leaving)
                pageStack.pop(target);
        };
        // A page with unsaved changes asks first, as it does for Esc
        if (leaving?.hasUnsavedChanges)
            leaving.askBeforeLeaving(leave);
        else
            leave();
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
            if (section === "library") {
                window.newBadgeEnabled = window.settings.value("library", "newBadge");
                if (key.startsWith("tabs."))
                    window.subTabs = window.readSubTabs();
                return;
            }
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

    function readSubTabs() {
        const ids = ["movies.suggestions", "movies.favorites", "movies.collections", "movies.genres", "tv.suggestions", "tv.upcoming", "tv.genres", "tv.networks", "tv.episodes"];
        const tabs = {};
        for (const id of ids)
            tabs[id] = settings.value("library", "tabs." + id);
        return tabs;
    }

    // The sub-tabs the settings turn on go to the controller (ids without the prefix, e.g. "favorites"): the sub-tab bar of an open page follows without a restart
    function applySubTabs() {
        for (const [type, prefix] of [["movies", "movies."], ["tvshows", "tv."]])
            connection.setSubtabsEnabled(type, Object.keys(subTabs).filter(id => id.startsWith(prefix) && subTabs[id]).map(id => id.slice(prefix.length)));
    }
    onSubTabsChanged: applySubTabs()

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

    onClosing: close => {
        // Unsaved preferences ask first; the window closes again once they are settled
        if (askBeforeLeaving(() => window.close())) {
            close.accepted = false;
            return;
        }
        // A full-screen or maximized window has no geometry of its own to keep
        if (window.visibility === Window.Windowed)
            settings.setWindowGeometry(Qt.rect(x, y, width, height));
    }

    // The page follows the connection state: Servers until a server is chosen, the user picker while one of several saved users is to be chosen, sign-in until there is a session, Home after
    function showPageForState() {
        let page = "ServersPage.qml";
        let properties = { connection: connection };
        switch (connection.state) {
        case ConnectionController.NeedsLogin:
        case ConnectionController.SigningIn:
        case ConnectionController.QuickConnectPending:
            page = "LoginPage.qml";
            break;
        case ConnectionController.ChoosingUser:
            page = "UserPickerPage.qml";
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

    // The page above the first one that holds changes the user has not saved (the user preferences), or null
    function unsavedPage() {
        for (let i = 1; i < pageStack.depth; ++i) {
            const page = pageStack.get(i);
            if (page?.hasUnsavedChanges)
                return page;
        }
        return null;
    }

    // True when a page holds unsaved changes: it asks the user, and `again` runs once they are saved or discarded; the caller stops there. False, with nothing asked, when no page does
    function askBeforeLeaving(again) {
        const page = unsavedPage();
        if (!page)
            return false;
        page.askBeforeLeaving(again);
        return true;
    }

    // Everything above the first page goes
    function popToBase() {
        const base = pageStack.get(0);
        if (base)
            pageStack.pop(base);
    }

    // The first keystroke of the header box on a type tab pops the pages above it: the box keeps the keyboard, which the page it pops back to would take
    function popToBaseWhileTyping() {
        const wasTyping = shellHeader.searchFieldHasFocus;
        popToBase();
        if (wasTyping)
            Qt.callLater(shellHeader.refocusSearchField);
    }

    // Ctrl+F: a search page already in the stack is brought back as it is, a new one starts from an empty term above Home.
    // The header box takes the focus (the page's own field in Compact mode, where the box hides)
    function openSearch() {
        if (connection.state !== ConnectionController.Authenticated)
            return;
        // A search page already in the stack is brought back without dropping what lies below it
        if ((isTypeTab(rootTab) || searchPageIndex() < 0) && askBeforeLeaving(() => openSearch()))
            return;
        if (isTypeTab(rootTab)) {
            // On a type tab the box searches that tab's own list: the pages above the tab give way, and no search page opens
            popToBase();
            const tabPage = pageStack.get(0);
            Qt.callLater(() => {
                if (compactHeader)
                    tabPage?.focusSearchField();
                else
                    shellHeader.focusSearchField();
            });
            return;
        }
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
        else if (page?.objectName === "libraryPage" && page.tabType !== "")
            page.focusPrimary();
    }

    // The genre rails of the library page that asks (its model: a tab or a collections library) go above it, and the page stays below
    function openGenres(fromPage) {
        if (connection.state !== ConnectionController.Authenticated || !fromPage)
            return;
        if (!connection.openGenreRails(fromPage.model))
            return;
        pageStack.pop(fromPage);
        pageStack.push(Qt.resolvedUrl("GenreRailsPage.qml"), { connection: connection });
    }

    // The menu of a card (a right click, the Menu key or the button of the card): it opens at a point of the card, below it by default
    function openCardMenu(card, itemId, type, seriesId, x, y) {
        if (connection.state !== ConnectionController.Authenticated)
            return;
        cardMenu.itemId = itemId;
        cardMenu.itemType = type;
        cardMenu.seriesId = seriesId;
        cardMenu.popup(card, x ?? 0, y ?? card.height);
    }

    CardMenu {
        id: cardMenu

        connection: window.connection
        onOpenRequested: targetId => window.openItem(targetId, window.pageStack.currentItem)
    }

    // A link opens its item above Home once signed in, whether it came before or after the sign-in
    function openRequestedItem(home) {
        if (requestedItemId.length === 0 || connection.state !== ConnectionController.Authenticated || home?.objectName !== "homePage")
            return;
        const itemId = requestedItemId;
        requestedItemId = "";
        openItem(itemId, home);
    }

    // The sign-in page keeps its place while the state moves between its own steps, and so does the user picker
    readonly property int pageGroup: {
        switch (connection.state) {
        case ConnectionController.NeedsLogin:
        case ConnectionController.SigningIn:
        case ConnectionController.QuickConnectPending:
            return 1;
        case ConnectionController.Authenticated:
            return 2;
        case ConnectionController.ChoosingUser:
            return 3;
        default:
            return 0;
        }
    }
    onPageGroupChanged: {
        // A link waiting for a sign-in belongs to the server the user is leaving
        if (pageGroup === 0)
            requestedItemId = "";
        for (const tab of Object.keys(tabState))
            delete tabState[tab];
        for (const tab of Object.keys(oneOffTabs))
            delete oneOffTabs[tab];
        showPageForState();
    }

    // A link that arrives while Home is already showing (a second launch) opens at once
    onRequestedItemIdChanged: openRequestedItem(pageStack.get(0))

    // The section the page in front belongs to: the header marks it (a tab, the main menu button, the queue button); a page opened above a type tab belongs to that tab, and one opened from a collections tile to that library; the user preferences page and the Favorites page (with what opens above it) belong to no tab, and the header search is global there. Settings and About need no session, so they are marked whatever the connection is
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
            const page = pageStack.get(i);
            if (page?.objectName === "libraryPage" && page.tabType === "")
                return "library:" + connection.library.libraryId;
        }
        switch (pageStack.currentItem?.objectName) {
        case "searchPage":
            return "search";
        case "queuePage":
            return "queue";
        case "userPreferencesPage":
            return "userPreferences";
        }
        // The Favorites page and what opens above it (a group, an item) belong to the star: no tab is marked
        if (favoritesPageIndex() >= 0)
            return "favorites";
        return rootTab !== "" ? rootTab : "home";
    }

    // The stack position of the Favorites page up to the one in front, -1 when it is not there; the current item is read for the dependency: the stack changes with every push and pop
    function favoritesPageIndex() {
        if (!pageStack.currentItem)
            return -1;
        for (let i = 1; i <= pageStack.currentIndex; ++i) {
            if (pageStack.get(i)?.objectName === "favoritesPage")
                return i;
        }
        return -1;
    }

    // True while the user preferences page or the Favorites page is in the stack, whatever page lies above it (the search page the header box opens included): the box stays on the global search then, or the page it opens would turn it back to the list of the tab at the root and pop everything
    readonly property bool globalSearchPage: {
        // The current item is read for the dependency: the stack changes with every push and pop
        if (!pageStack.currentItem)
            return false;
        for (let i = 1; i < pageStack.depth; ++i) {
            const name = pageStack.get(i)?.objectName;
            if (name === "userPreferencesPage" || name === "favoritesPage")
                return true;
        }
        return false;
    }

    function isTypeTab(name) {
        return name === "movies" || name === "tvshows";
    }

    function tabOfPage(page) {
        switch (page?.objectName) {
        case "homePage":
            return "home";
        case "libraryPage":
            return page.tabType;
        }
        return "";
    }

    // Whether the server has a library of the type: the tab of a type it lacks is not offered
    function hasTypeTab(name) {
        return name === "movies" ? connection.movieLibraries.length > 0 : connection.showLibraries.length > 0;
    }

    // The tab becomes the first page of the stack: what was there goes, the tab it leaves is remembered, and the one it comes back to is restored. A type tab opens its list (the stored library when `libraryId` is empty, else that library, which a Home tile persists); the tab already in front only drops the pages above it. `recentlyAdded` opens the type tab on the one-off Recently Added view instead of the stored one
    function openTab(name, libraryId) {
        openTabScope(name, libraryId, false);
    }
    function openTabScope(name, libraryId, recentlyAdded) {
        if (!signedIn || askBeforeLeaving(() => openTabScope(name, libraryId, recentlyAdded)))
            return;
        const openList = () => recentlyAdded ? connection.openRecentlyAdded(name, libraryId ?? "") : connection.openType(name, libraryId ?? "");
        const leaving = pageStack.get(0);
        const leavingTab = rootTab;
        if (name === leavingTab) {
            if (isTypeTab(name) && (libraryId || recentlyAdded)) {
                openList();
                if (recentlyAdded)
                    oneOffTabs[name] = true;
                else
                    delete oneOffTabs[name];
            }
            popToBase();
            return;
        }
        if (isTypeTab(name) && !hasTypeTab(name))
            return;
        const model = name === "movies" ? connection.movies : connection.shows;
        const scopeBefore = isTypeTab(name) ? model.libraryId : "";
        if (isTypeTab(name) && !openList())
            return;
        // A position saved on another library of the type, or on a Recently Added list, means nothing on what opens now
        if (isTypeTab(name) && (model.libraryId !== scopeBefore || (!recentlyAdded && oneOffTabs[name])))
            delete tabState[name];
        if (isTypeTab(name)) {
            if (recentlyAdded)
                oneOffTabs[name] = true;
            else
                delete oneOffTabs[name];
        }
        if (leavingTab !== "" && leaving?.viewPosition)
            tabState[leavingTab] = leaving.viewPosition();
        pageStack.clear();
        // Leaving the type pages gives the sub-tab's data and channels back
        if (name === "home")
            connection.closeSubtabs();
        let page = null;
        if (name === "home")
            page = pageStack.push(Qt.resolvedUrl("HomePage.qml"), { connection: connection });
        else
            page = pageStack.push(Qt.resolvedUrl("LibraryPage.qml"), { model: name === "movies" ? connection.movies : connection.shows, tabType: name, connection: connection });
        if (tabState[name] && page?.restoreViewPosition)
            page.restoreViewPosition(tabState[name]);
    }

    // See All of a Recently Added row: the type tab opens from the top on the library's items by date added, newest first, and the sort saved for the tab stays as it was
    function openRecentlyAdded(name, libraryId) {
        if (!signedIn || !isTypeTab(name))
            return;
        delete tabState[name];
        openTabScope(name, libraryId, true);
    }

    // A Continue Watching page is gone: the list is closed with the last one
    function resumePageGone() {
        if (--resumePages === 0)
            connection.resumeItems.close();
    }

    // See All of Continue Watching goes above Home: a page already open is brought back as it is
    function openResumeItems() {
        if (!signedIn)
            return;
        for (let i = 0; i <= pageStack.currentIndex; ++i) {
            const page = pageStack.get(i);
            if (page?.objectName === "resumeItemsPage") {
                pageStack.pop(page);
                return;
            }
        }
        popToBase();
        if (!connection.openResumeItems())
            return;
        // Counted before the page exists and released when it is deleted: a page popped a moment ago may be deleted after this one is pushed
        resumePages++;
        if (!pageStack.push(Qt.resolvedUrl("ResumeItemsPage.qml"), { connection: connection, shell: window }))
            resumePages--;
    }

    // A Favorites page is gone: the scan stops with the last one
    function favoritesPageGone() {
        if (--favoritesPages === 0)
            connection.favorites.close();
    }

    // The star of the header: the Favorites page opens above the first page, and one already in the stack is brought back as it is (the page starts the scan when it is created)
    function openFavorites() {
        if (!signedIn)
            return;
        openSection("favoritesPage", "FavoritesPage.qml", { connection: connection, shell: window });
    }

    // The Home tab; before a session it is the page the state wants
    function goHome() {
        if (signedIn)
            openTab("home");
        else
            showPageForState();
    }

    // A library opened from a tile that is no tab (collections) goes above Home: one already open gives way to it, and is popped before the shared model switches to the new library
    function openLibrary(libraryId, libraryName) {
        if (!signedIn)
            return;
        // The library already open is not opened again: filters, scroll and the pages above it would be lost
        for (let i = 0; i <= pageStack.currentIndex; ++i) {
            const page = pageStack.get(i);
            if (page?.objectName === "libraryPage" && page.tabType === "" && connection.library.libraryId === libraryId) {
                pageStack.pop(page);
                return;
            }
        }
        const home = pageStack.get(0);
        if (home)
            pageStack.pop(home);
        if (connection.openLibrary(libraryId))
            pageStack.push(Qt.resolvedUrl("LibraryPage.qml"), { model: connection.library, libraryName: libraryName, connection: connection });
    }

    // A type that vanishes from the server while its tab shows sends the user home
    Connections {
        target: window.connection
        // A menu left open over a session that ended would act on an item of the server the user left
        function onStateChanged() {
            if (window.connection.state !== ConnectionController.Authenticated)
                cardMenu.close();
        }
        function onLibrariesChanged() {
            if (window.signedIn && window.connection.state === ConnectionController.Authenticated && window.isTypeTab(window.rootTab) && !window.hasTypeTab(window.rootTab))
                window.goHome();
        }
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
        if (askBeforeLeaving(() => openSection(name, url, properties)))
            return;
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
        applySubTabs();
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
