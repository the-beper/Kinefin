// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Kirigami.ApplicationWindow {
    id: window

    // The connection to the Jellyfin server, created by the application
    required property ConnectionController connection
    required property ClientSettings settings
    // Provided by the executable; optional for shell tests that have no mpv renderer.
    property var player: null
    // Watching together (SyncPlayController); null when the application gives none
    property var syncPlay: null
    readonly property bool groupJoined: syncPlay?.status === SyncPlayController.Joined
    // The player's play actions go to the group: joined, following and online
    readonly property bool groupRouting: groupJoined && syncPlay.following && !syncPlay.offline
    // The remote-control socket, provided by the executable; null in shell tests
    property var remoteInput: null
    // SDL3 gamepads, provided by the executable; null in shell tests and when SDL is off
    property var gamepad: null
    // "fullscreen" or "windowed" when a command line flag decides this run; empty to follow the setting. Changing the setting from the Settings page ends the override
    property string startupWindowMode: ""
    // "settings" or "about": the Settings page or the About dialog to open right after the shell loads (the smoke test uses it to render what needs no server)
    property string startupPage: ""
    // What About names this build: a development build's time, else empty for the version
    property string displayVersion: ""
    // An item link from the command line: opened once Home shows, then cleared
    property string requestedItemId: ""
    // Invalidates deferred link navigation when an account changes or a newer link arrives
    property int itemRequestGeneration: 0
    readonly property bool signedIn: connection.state === ConnectionController.Authenticated
    // A saved owner's downloads, shown without a session; only the Downloads tab and the player work
    readonly property bool offline: connection.state === ConnectionController.Offline
    // A server is chosen while its sign-in or a session is in progress: the main menu offers Switch Server for it
    readonly property bool serverChosen: connection.state !== ConnectionController.Idle
    // The modes in pixels: Compact below the width where the sidebar's rail leaves a page as wide as the smallest window (the sidebar gives way to the header and its drawer), Wide from 36 grid units; tests derive their window sizes from these
    readonly property int compactLimit: Math.ceil(shellSidebar.railWidth + minimumWidth)
    // The smallest window: the Compact header with its navigation button fits, and the pages keep room to show something
    minimumWidth: Kirigami.Units.gridUnit * 24
    minimumHeight: Kirigami.Units.gridUnit * 20
    readonly property int wideLimit: Kirigami.Units.gridUnit * 36
    // The widest a block of reading content (item header, queue rows) gets before it is centred: the pages read it from here so they agree
    readonly property real readingWidth: Kirigami.Units.gridUnit * 70
    // The width decides the modes, read here where the width lives: a header-side binding on the
    // attached Window.window kept its load-time value and never followed a resize
    readonly property bool compactHeader: width < compactLimit
    readonly property bool wideHeader: width >= wideLimit
    // The tab the first page of the stack stands for: "home", "movies", "tvshows", "music", "audiobooks", "favorites", "playlists" or "downloads"; empty on the sign-in and server pages and for a library opened from a tile. The depth and the current item change with every push and clear, and the first page is read after them
    readonly property string rootTab: pageStack.depth > 0 && pageStack.currentItem ? tabOfPage(pageStack.get(0)) : ""
    // The scroll position and the current tile of each tab left behind, so a tab comes back as the user left it; dropped with the session
    readonly property var tabState: ({})
    // The type tabs showing a Recently Added list: what the tab saved on leaving belongs to that list, not to the stored view it comes back to
    readonly property var oneOffTabs: ({})
    // The Continue Watching pages alive: the list is closed when the last one goes, not by a page that outlives its successor
    property int resumePages: 0
    // The Favorites pages alive, counted the same way: the scan stops when the last one goes, not by a page that outlives its successor
    property int favoritesPages: 0
    // The Playlists and Playlist pages alive (one deleted late may outlive the next one): the shared models stop reading with the last
    property int playlistsPages: 0
    property int playlistPages: 0
    // A playlist on its way into the queue, and the item it starts at (its first when empty); the queue answers here whatever page asked
    property bool playlistWaiting: false
    property string playlistStartAt: ""
    property int playlistQueueBefore: 0
    // Numbers the waits, so the end of one never ends the next
    property int playlistRequest: 0
    // Another session's PlayNow on its way into the queue: the item it starts at, where, and the queue's size before (an Undo is offered when it held something)
    property bool remotePlayWaiting: false
    property string remoteStartAt: ""
    property real remoteStartTicks: 0
    property int remoteQueueBefore: 0
    property int remotePlayRequest: 0
    // Another session's Play commands not yet run, oldest first (the queue takes one operation at a time): {now, itemIds, next, startIndex, startTicks}
    property var remotePending: []

    property bool fullscreenSetting: settings.value("main", "fullscreen")
    property string fullscreenOutput: settings.value("main", "forceFSScreen")
    property bool newBadgeEnabled: settings.value("library", "newBadge")
    property bool homeLibrariesEnabled: settings.value("library", "home.libraries")
    property bool homeFavoritesEnabled: settings.value("library", "home.favorites")
    // Which library sub-tabs are switched on, by setting id without the "tabs." prefix (e.g. "movies.favorites"); replaced as a whole on a change so bindings re-evaluate
    property var subTabs: readSubTabs()
    readonly property bool wantsFullscreen: startupWindowMode === "fullscreen" || (startupWindowMode !== "windowed" && fullscreenSetting)

    width: 1000
    height: 640
    visible: true
    title: "Kinefin"
    flags: Qt.Window

    // Every page turns Kirigami's title row off and puts its own row under the header (PageToolbar, the style of the Movies and Shows tabs); the global style stays pinned to ToolBar so that it never follows the stack depth: Auto would resolve to the Breadcrumb header on mobile in narrow mode, and a style that follows the depth resizes the columns while a page slides out (Kirigami then reads the resize as a layout pass, stops the exit slide without finishing it, and the page it was removing is never destroyed)
    pageStack.globalToolBar.style: Kirigami.ApplicationHeaderStyle.ToolBar
    // One page at a time: the default switches to side-by-side columns from 40 grid units, which pushes the pages beyond the window
    pageStack.columnView.columnResizeMode: Kirigami.ColumnView.SingleColumn
    // The player page reaches up out of its column into the header band (PlayerPage's topPadding); no other page draws outside its column
    pageStack.columnView.clip: false
    // The tab switch slides the whole row: Kirigami binds the transform and the opacity of every page, so a page is never the target
    pageStack.transform: Translate { id: tabShift }
    pageStack.globalToolBar.showNavigationButtons: Kirigami.ApplicationHeaderStyle.ShowBackButton
    // The sidebar takes the leading edge of the window: the page row starts after it. A change of this margin resizes the row, which ends a slide in progress, so it follows the window mode only
    pageStack.anchors.leftMargin: shellSidebar.inlineWidth

    // The width the sidebar takes inline (0 in a Compact window and while there is nothing to choose from), which the player page reaches out over
    readonly property real sidebarWidth: shellSidebar.inlineWidth
    // Where the header's menu button stands while the sidebar shows, and the SyncPlay group menu the sidebar's entry opens: both belong to the header
    readonly property Item sidebarUserSlot: shellSidebar.userSlot
    readonly property bool groupMenuOpened: shellHeader.groupMenuOpened
    function openGroupMenu(button) {
        shellHeader.openGroupMenu(button);
    }

    // The sections of the shell with their actions and shortcuts; the sidebar draws them
    ShellNavigation {
        id: navigation

        shell: window
    }

    // The bar of a window without the sidebar (Compact, or offline); with the sidebar showing, and signed out, it is hidden and takes no height, and the pages start at the top. It spans the window above the pages
    header: ShellHeader {
        id: shellHeader
        // It never leaves the layout while it shows, since any resize of the page row ends Kirigami's slide at once: the player page reaches up under it, and it fades with the player page's slide and lets the clicks through to it while the player is in front
        opacity: window.headerOpacity
        enabled: !window.playbackPageShowing
        // Covered by the player, it is not read out over it either
        Accessible.ignored: window.playbackPageShowing
        // Signed out (the server and sign-in pages) there is nothing for it to hold: no sections to open, no search, no queue
        visible: !window.startupVisible && window.sidebarWidth === 0 && (navigation.navigable || window.offline)

        shell: window
        navigation: navigation
        onNavigationRequested: shellSidebar.openDrawer()
    }

    // The rail down the leading edge of the window, from its top, and in a Compact window the drawer the header's button opens. It fades out with the header while the player is in front
    ShellSidebar {
        id: shellSidebar

        shell: window
        navigation: navigation
        headerHeight: shellHeader.height
        onDrawerDismissed: shellHeader.focusNavigationButton()
    }

    // The initial shell never flashes an empty page. The delay is short enough
    // to feel responsive, with a finite fallback for offline/unresponsive servers.
    property bool startupVisible: startupPage.length === 0
    property bool startupMinimumElapsed: false
    property bool startupDeadlineElapsed: false
    function settleStartup() {
        if (!startupVisible || !startupMinimumElapsed || !pageStack.currentItem)
            return;
        if (!startupDeadlineElapsed && (connection.state === ConnectionController.Connecting
                || (signedIn && !connection.librariesLoaded)))
            return;
        startupVisible = false;
    }
    // The cover hides the Servers page and its Cancel while a Connect is under way: it carries its own, or a silent host would hold the user until the cover's deadline
    property bool connectCover: false
    onStartupVisibleChanged: {
        if (!startupVisible)
            connectCover = false;
    }
    // A Connect from the Servers page shows the cover again, by the same rules as at startup; nothing happens unless the connection is under way
    function showConnectCover() {
        if (connection.state !== ConnectionController.Connecting)
            return;
        connectCover = true;
        startupMinimumElapsed = false;
        startupDeadlineElapsed = false;
        startupVisible = true;
        startupMinimumTimer.restart();
        startupDeadlineTimer.restart();
    }
    Timer {
        id: startupMinimumTimer
        interval: 500
        running: window.startupVisible
        repeat: false
        onTriggered: { window.startupMinimumElapsed = true; window.settleStartup(); }
    }
    Timer {
        id: startupDeadlineTimer
        interval: 8000
        running: window.startupVisible
        repeat: false
        onTriggered: { window.startupDeadlineElapsed = true; window.settleStartup(); }
    }
    Connections {
        target: window.connection
        function onStateChanged() { Qt.callLater(window.settleStartup); }
        function onHomeProjectionChanged() { Qt.callLater(window.settleStartup); }
    }
    Rectangle {
        id: startupCover
        objectName: "startupCover"
        anchors.fill: parent
        z: 1000
        color: Kirigami.Theme.backgroundColor
        opacity: window.startupVisible ? 1 : 0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: Kirigami.Units.longDuration; easing.type: Easing.OutCubic } }
        // Do not allow activating a page while the shell is still preparing.
        MouseArea { anchors.fill: parent; enabled: window.startupVisible }
        Column {
            anchors.centerIn: parent
            spacing: Kirigami.Units.largeSpacing
            Kirigami.Icon {
                anchors.horizontalCenter: parent.horizontalCenter
                source: "io.github.the_beper.Kinefin"
                width: Kirigami.Units.iconSizes.huge
                height: width
            }
            // A plain bar rather than a LoadingPlaceholder, which has no accessible name without its text
            QQC2.ProgressBar {
                anchors.horizontalCenter: parent.horizontalCenter
                width: Kirigami.Units.gridUnit * 12
                indeterminate: true
                Accessible.name: i18n("Preparing Kinefin…")
            }
            QQC2.Button {
                objectName: "startupCoverCancel"
                anchors.horizontalCenter: parent.horizontalCenter
                visible: window.connectCover
                text: i18nc("@action:button", "Cancel")
                onClicked: window.connection.cancel()
            }
        }
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
        // The page may take the back itself (unsaved changes ask first, an open entry closes), as it does for Esc: the signature is Kirigami's, PageRow.goBack
        if (leaving?.backRequested) {
            const event = { accepted: false };
            leaving.backRequested(event);
            if (event.accepted)
                return;
        }
        // The same slide as Esc: only the index moves, the page left above is taken once the stack settles
        const target = pageStack.get(index - 1);
        if (pageStack.get(index) === leaving)
            popTo(target, true);
    }

    // A back that only moves the index (Esc, the header button, the toolbar back button) leaves the page it leaves in the stack, hidden above the current one and still counted, with its models alive. Kirigami's own popHiddenPages option took such pages once the row settled, but it also ran while a push was still placing its page mid-slide, and popped the page it had just received. So the leftovers are taken here, at least a turn of the event loop later: by then every push has landed and every unwind has taken the pages it was removing, and pages above the current index can only be leftovers a back left behind. They go only once the row stopped moving: a page taken mid-slide vanishes from the crossfade that still shows it. The first check always waits once more (Kirigami starts a slide only at the next polish, which a busy turn can delay past the timer), later ones wait while the row moves, and the eighth settles whatever the row does, so a slide that never reports its end cannot keep the leftovers
    property int pageRowSettleTries: 0
    function settlePageRow() {
        ++pageRowSettleTries;
        if (pageRowSettleTries < 8 && (pageRowSettleTries === 1 || pageStack.columnView.moving)) {
            pageRowSettle.restart();
            return;
        }
        let popped = false;
        while (pageStack.currentIndex >= 0 && pageStack.currentIndex < pageStack.depth - 1) {
            pageStack.pop(pageStack.get(pageStack.currentIndex));
            popped = true;
        }
        // Kirigami hands the keyboard to its column row when it takes a page out: the page in front, which already had it, takes it back (a dialog, a popup or the header keep theirs)
        if (popped && (!activeFocusItem || (focusIsIn(pageStack.columnView) && !focusIsIn(pageStack.currentItem))))
            Qt.callLater(focusCurrentPage);
    }
    function settlePageRowLater() {
        pageRowSettleTries = 0;
        pageRowSettle.restart();
    }
    Timer {
        id: pageRowSettle
        interval: Math.max(1, window.pageStack.columnView.scrollDuration)
        onTriggered: window.settlePageRow()
    }

    // Esc ends a running autoscroll and nothing more, else it goes back like Alt+Left except on the first page; an open dialog takes the key first
    readonly property bool canEscapeBack: pageStack.currentIndex > 0 && pageStack.layers.depth <= 1
    function escapeBack() {
        if (canEscapeBack)
            pageStack.goBack();
    }
    Shortcut {
        sequences: ["Escape"]
        enabled: window.canEscapeBack || autoScroll.active
        onActivated: {
            if (autoScroll.active)
                autoScroll.stop();
            else
                window.escapeBack();
        }
    }
    // Middle-click autoscroll, one for every page and the header
    AutoScroll {
        id: autoScroll
        shell: window
    }

    // What a remote control asks for besides the keys it sends: the player commands reach the player in every mode, the full page or the mini player
    function popupOpen() {
        for (const item of window.overlay?.children ?? []) {
            // A visible tooltip counts as a popup too: QML cannot tell its popup item from a menu's, so a middle click or a remote Back while a tooltip shows does nothing
            if (item.visible && String(item).startsWith("QQuickPopupItem"))
                return true;
        }
        return false;
    }
    function remoteAction(name) {
        switch (name) {
        case "back":
            // A popup closes on a real Escape through a shortcut, which a synthesised key never reaches: an open popup keeps the page under it as it is
            if (autoScroll.active)
                autoScroll.stop();
            else if (!popupOpen())
                escapeBack();
            break;
        case "home":
            goHome();
            break;
        case "previoustab":
        case "nexttab":
            if (playbackMode === "full")
                playerAction(name === "previoustab" ? "previous" : "next");
            else
                navigation.switchRelativeTab(name === "previoustab" ? -1 : 1);
            break;
        case "previouschapter":
        case "nextchapter":
            if (playbackMode !== "none" && player)
                player.stepChapter(name === "nextchapter" ? 1 : -1);
            break;
        case "mainmenu":
            if (playbackMode === "full")
                playerAction("togglefullscreen");
            else if (!popupOpen())
                shellHeader.openMainMenu();
            break;
        case "expandplayer":
            expandPlayer();
            break;
        default:
            playerAction(name);
            // The bars of the full player show for what the remote did
            if (playbackMode === "full")
                playerPage?.wakeChrome();
        }
    }
    Connections {
        target: window.remoteInput
        function onActionRequested(action) { window.remoteAction(action); }
    }

    Shortcut {
        sequences: [StandardKey.Find]
        enabled: window.signedIn
        onActivated: window.openSearch()
    }
    // Brings the mini player or the picture-in-picture window back to the full player
    Shortcut {
        sequences: ["Ctrl+Shift+P"]
        enabled: window.playbackMode === "mini" || window.playbackMode === "pip"
        onActivated: window.expandPlayer()
    }

    // The page that comes to the front takes the keyboard focus, so Tab and the arrows work without a mouse click first; a page that already focused one of its own fields (sign-in) keeps that focus
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
    function applyPageFocus(page) {
        // Back goes to the control the page last had the focus in, a new page to its main control, any other page keeps a field it focused itself
        const remembered = lastFocus.get(page);
        if (remembered?.visible && remembered.enabled && remembered !== page.contentItem)
            remembered.forceActiveFocus(Qt.TabFocusReason);
        else if (page.focusPrimary)
            page.focusPrimary();
        else if (!focusIsIn(page))
            page.forceActiveFocus();
    }
    function focusCurrentPage() {
        forgetLeftPages();
        const page = pageStack.currentItem;
        // Typing in the search popup is never interrupted: its field and its cards hold the keyboard while it shows
        if (!page || searchPopup.visible)
            return;
        applyPageFocus(page);
    }
    Connections {
        target: window.pageStack
        function onCurrentItemChanged() {
            // A page opened above the page still waiting to fade in shows at once
            if (window.tabMotionQueued && window.pageStack.currentIndex > window.tabMotionIndex)
                window.finishTabMotion();
            // The header fades out with a player page that comes to the front and back in with the page that takes its place
            window.leftPlayerPage = window.shownPageIsPlayer;
            window.shownPageIsPlayer = window.pageStack.currentItem?.objectName === "playerPage";
            // A page a Back left hidden above the current one is taken once the stack settles
            window.settlePageRowLater();
            // A page that comes to the front (a result, Play from a card menu, a SyncPlay item) ends the search: the popup would cover it
            searchPopup.close();
            Qt.callLater(window.focusCurrentPage);
        }
    }

    Connections {
        target: window.settings
        function onValueChanged(section, key) {
            if (section === "library") {
                window.newBadgeEnabled = window.settings.value("library", "newBadge");
                window.homeLibrariesEnabled = window.settings.value("library", "home.libraries");
                window.homeFavoritesEnabled = window.settings.value("library", "home.favorites");
                if (key.startsWith("tabs."))
                    window.subTabs = window.readSubTabs();
                return;
            }
            if (section !== "main")
                return;
            window.fullscreenSetting = window.settings.value("main", "fullscreen");
            if (key === "fullscreen")
                window.startupWindowMode = "";
            window.fullscreenOutput = window.settings.value("main", "forceFSScreen");
        }
        function onSaveFailed() {
            window.showPassiveNotification(i18n("The settings could not be saved: check the free space and the permissions of the profile folder. Changes last until Kinefin closes"));
        }
    }

    function readSubTabs() {
        const ids = ["movies.suggestions", "movies.favorites", "movies.collections", "movies.genres", "tv.suggestions", "tv.favorites", "tv.collections", "tv.upcoming", "tv.genres", "tv.networks", "tv.episodes"];
        const tabs = {};
        for (const id of ids)
            tabs[id] = settings.value("library", "tabs." + id);
        return tabs;
    }

    // The sub-tabs the settings turn on go to the controller (ids without the prefix, e.g. "favorites"): the sub-tab entries of an open page follow without a restart
    function applySubTabs() {
        for (const [type, prefix] of [["movies", "movies."], ["tvshows", "tv."]])
            connection.setSubtabsEnabled(type, Object.keys(subTabs).filter(id => id.startsWith(prefix) && subTabs[id]).map(id => id.slice(prefix.length)));
    }
    onSubTabsChanged: applySubTabs()
    onHomeFavoritesEnabledChanged: connection.setHomeFavoritesEnabled(homeFavoritesEnabled)

    // Full screen goes to the output named by "Force Full Screen Display" when that output exists
    function showFullScreenOnOutput() {
        const output = Application.screens.find(screen => screen.name === fullscreenOutput);
        if (output)
            window.screen = output;
        window.visibility = Window.FullScreen;
    }
    function applyFullscreen() {
        if (wantsFullscreen)
            showFullScreenOnOutput();
        else if (window.visibility === Window.FullScreen)
            window.visibility = Window.Windowed;
    }

    // The visibility the window had when the player first switched full screen, given back when the player page goes; -1 while the player has not switched it
    property int visibilityBeforePlayer: -1
    // What the window was before the player's last switch to full screen, which leaving that full screen gives back: maximized or a window with this geometry
    property int visibilityBeforeFullscreen: Window.Windowed
    property rect geometryBeforeFullscreen
    // A full screen the player entered: Back leaves it before it leaves the player. One that came from the setting, the command line or the window manager is not the player's
    readonly property bool playerFullscreen: visibilityBeforePlayer >= 0 && visibilityBeforePlayer !== Window.FullScreen && visibility === Window.FullScreen
    function togglePlayerFullscreen() {
        if (visibilityBeforePlayer < 0)
            visibilityBeforePlayer = visibility;
        if (visibility === Window.FullScreen) {
            visibility = visibilityBeforeFullscreen;
        } else {
            visibilityBeforeFullscreen = visibility === Window.Maximized ? Window.Maximized : Window.Windowed;
            geometryBeforeFullscreen = Qt.rect(x, y, width, height);
            showFullScreenOnOutput();
        }
    }
    function restoreVisibilityAfterPlayer() {
        const before = visibilityBeforePlayer;
        visibilityBeforePlayer = -1;
        if (before === Window.FullScreen && visibility !== Window.FullScreen)
            showFullScreenOnOutput();
        else if (before >= 0 && before !== Window.FullScreen && visibility === Window.FullScreen)
            visibility = visibilityBeforeFullscreen;
        visibilityBeforeFullscreen = Window.Windowed;
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
        // The picture-in-picture window would keep the application alive: its playback stops first
        if (playbackMode === "pip") {
            stopPlayback();
            pipOpen = false;
        }
        // A full-screen or maximized window has no geometry of its own to keep; a full screen the player entered keeps the window it came from
        if (window.visibility === Window.Windowed)
            settings.setWindowGeometry(Qt.rect(x, y, width, height));
        else if (playerFullscreen && visibilityBeforeFullscreen === Window.Windowed)
            settings.setWindowGeometry(geometryBeforeFullscreen);
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
        case ConnectionController.Offline:
            page = "DownloadsPage.qml";
            break;
        }
        // A failed connection must not wipe out the form and the error
        if (page === "ServersPage.qml" && pageStack.depth === 1
            && pageStack.currentItem?.objectName === "serversPage") {
            Qt.callLater(settleStartup);
            return;
        }
        // The page of the new state fades in, as a tab does but without sliding; queued before the stack changes, so an error on the way cannot leave the row invisible
        if (prepareTabMotion(0))
            Qt.callLater(startTabMotionWhenReady, "", "");
        parkVideo();
        pageStack.clear();
        const shown = pageStack.push(Qt.resolvedUrl(page), properties);
        openRequestedItem(shown);
        Qt.callLater(settleStartup);
    }

    readonly property bool playbackPageShowing: pageStack.currentItem?.objectName === "playerPage"
    // Whether the page in front is the player page, and whether the page before it was, set as the page in front changes
    property bool shownPageIsPlayer: false
    property bool leftPlayerPage: false
    // Kirigami fades the page that comes to the front in over its slide: the header fades out against an arriving player page and in with the page that replaces one, and is opaque at rest away from the player
    readonly property real headerOpacity: {
        const page = pageStack.currentItem;
        if (!page)
            return 1;
        if (playbackPageShowing)
            return 1 - page.opacity;
        return leftPlayerPage ? page.opacity : 1;
    }

    // How the playback shows: "none" while nothing plays, "full" while the player page is in front, "mini" while it goes on under another page, "pip" while it shows in its own window. The C++ side knows nothing of it
    property string playbackMode: "none"
    // The changes of user data that a playback causes are read again once it is over
    onPlaybackModeChanged: connection.holdUserDataRefresh(playbackMode !== "none")
    // The player page pushed last; QML clears it when the page is deleted
    property Item playerPage: null
    // Whether the picture-in-picture window exists: set before the mode moves into it, cleared only after the video has left it, since the destruction of the window is what frees its render context
    property bool pipOpen: false
    readonly property var pictureInPictureWindow: pipLoader.item?.pip ?? null
    // Where the video item sits in the current mode: the slot of the player page, of the mini player or of the picture-in-picture window, none without playback
    readonly property Item videoSlot: playbackMode === "full" ? (playerPage?.videoSlot ?? null) : playbackMode === "mini" ? miniPlayer.videoSlot : playbackMode === "pip" ? (pictureInPictureWindow?.videoSlot ?? null) : null
    onVideoSlotChanged: placeVideo()

    // The one video item, built when playback starts and deleted only when it ends, so the mpv core and the session outlive every page; it is never a child of a page, only moved between the slots
    Loader {
        id: videoLoader
        objectName: "playbackVideoHolder"
        // Not hidden and not empty: the item stays in the window and keeps a surface to render in, never shown
        width: Kirigami.Units.gridUnit
        height: Kirigami.Units.gridUnit
        opacity: 0
        active: window.playbackMode !== "none"
        sourceComponent: MpvVideoItem {
            objectName: "videoRenderer"
            anchors.fill: parent
            player: window.player
            // The last frame of the local playback stays on the item while a cast plays or has failed: the black behind it shows instead; transparent rather than hidden, so that the item keeps its surface
            opacity: window.player?.casting || window.player?.castTargetId || window.player?.canContinueHere ? 0 : 1
            // The picture-in-picture window could not render the video: it goes back to the full player, playing on
            onRendererFailed: {
                if (window.playbackMode !== "pip")
                    return;
                window.showPassiveNotification(i18n("The video could not be shown in its own window"));
                window.expandPlayer();
            }

            // Moved into another window (picture in picture and back) the item has no picture until its new render context is ready and mpv drew into it: black covers it until `ready`, waits a moment for the player to give the new context its video track, then fades out, with a fallback in case `ready` never comes
            readonly property var hostWindow: Window.window
            onHostWindowChanged: {
                windowCoverFade.stop();
                windowCover.opacity = 1;
                windowCoverFallback.restart();
            }
            onReady: {
                windowCoverFallback.stop();
                windowCoverFade.restart();
            }
            Rectangle {
                id: windowCover
                anchors.fill: parent
                color: "black"
                opacity: 0
                visible: opacity > 0
            }
            SequentialAnimation {
                id: windowCoverFade
                PauseAnimation {
                    duration: Kirigami.Units.longDuration
                }
                NumberAnimation {
                    target: windowCover
                    property: "opacity"
                    to: 0
                    duration: Kirigami.Units.shortDuration
                    easing.type: Easing.OutCubic
                }
            }
            Timer {
                id: windowCoverFallback
                interval: Kirigami.Units.humanMoment
                onTriggered: windowCoverFade.restart()
            }
        }
        onLoaded: window.placeVideo()
    }

    // The loader holds the item while no slot is there
    function placeVideo() {
        const item = videoLoader.item;
        if (item)
            item.parent = videoSlot ?? videoLoader;
    }

    // Called before any change of the page stack that removes the player page while something plays: the mode moves to the mini player and the video item to its slot first, so the item never leaves its window
    function parkVideo() {
        if (playbackMode !== "full" || !playbackPageShowing || !player?.active)
            return;
        playbackMode = "mini";
        placeVideo();
    }

    // Pops to a page of the stack. With `animated`, and always when the page in front is the player's (its video parked first), only the current index moves, as Kirigami's Back does: the row slides back and the pages left above are taken once the stack settles, which deletes them (a pop of the page in front does not). Without it the pages go at once, for a push that follows
    function popTo(page, animated) {
        if (page && page !== pageStack.currentItem && (playbackPageShowing || animated)) {
            if (playbackPageShowing)
                parkVideo();
            for (let i = 0; i < pageStack.depth; ++i) {
                if (pageStack.get(i) === page) {
                    // Only ever back: a page above the current one is a leftover about to go
                    if (i < pageStack.currentIndex)
                        pageStack.currentIndex = i;
                    return;
                }
            }
        }
        pageStack.pop(page);
    }

    MiniPlayer {
        id: miniPlayer
        shell: window
    }
    // The picture-in-picture window exists only while pipOpen; a Loader loads items only, so a plain item holds it
    Loader {
        id: pipLoader
        active: window.pipOpen
        sourceComponent: Item {
            property alias pip: pipWindow
            PictureInPictureWindow {
                id: pipWindow
                shell: window
            }
        }
    }
    // The height from the bottom of the window that the docked mini player covers, for the content that ends there and has a control in the corner
    readonly property real miniPlayerSpace: playbackMode === "mini" ? miniPlayer.height + miniPlayer.margin * 2 : 0

    // A playback that fails while the mini player shows brings the page back, with its error and its Close Player button
    readonly property bool failedInMini: playbackMode === "mini" && !!player && !player.active && !!player.errorText
    // A turn later: the expansion changes the mode this property is computed from
    onFailedInMiniChanged: if (failedInMini) Qt.callLater(expandPlayer)

    // A playback that fails in the picture-in-picture window brings the full player back, with its error
    readonly property bool failedInPip: playbackMode === "pip" && !!player && !player.active && !!player.errorText
    onFailedInPipChanged: if (failedInPip) Qt.callLater(expandPlayer)

    // Moves the playback from the full player into the picture-in-picture window: the window exists and the video is in it before the page leaves, so the item never stays in a page that goes away
    function enterPictureInPicture() {
        if (playbackMode !== "full" || !playbackPageShowing || !player?.active)
            return;
        pipOpen = true;
        if (!pictureInPictureWindow) {
            pipOpen = false;
            return;
        }
        playbackMode = "pip";
        placeVideo();
        // As Kirigami's Back does: the index moves, the shell takes the page left above once the stack settles
        pageStack.currentIndex = Math.max(0, pageStack.currentIndex - 1);
    }

    // The window goes a turn after the video left it, unless the mode went back to picture in picture meanwhile
    function closePictureInPicture() {
        Qt.callLater(() => {
            if (window.playbackMode !== "pip")
                window.pipOpen = false;
        });
    }

    // A new play always opens the full player, from the mini player and from picture in picture too
    function showPlayerPage() {
        // A downloaded item plays without a session, in Offline too
        if (!(window.signedIn || window.offline) || !window.player)
            return false;
        if (playbackPageShowing)
            return true;
        finishTabMotion();
        const before = playbackMode;
        // The page of an earlier visit may still be waiting to be deleted: the video must not go into its slot
        playerPage = null;
        playbackMode = "full";
        const page = pageStack.push(Qt.resolvedUrl("PlayerPage.qml"), {
            player: window.player, connection: connection, syncPlay: window.syncPlay
        });
        if (!page) {
            playbackMode = before;
            return false;
        }
        playerPage = page;
        placeVideo();
        if (before === "pip") {
            closePictureInPicture();
            if (window.visibility === Window.Minimized)
                window.showNormal();
            window.raise();
            window.requestActivate();
        }
        return true;
    }

    function expandPlayer() {
        if (playbackMode === "mini" || playbackMode === "pip")
            showPlayerPage();
    }

    // Playback is over or was stopped: the video item goes (its player first, so the detach is at once), and the player page with it
    function endPlayback() {
        if (playbackMode === "none")
            return;
        const wasMini = playbackMode === "mini" || playbackMode === "pip";
        const wasPip = playbackMode === "pip";
        if (videoLoader.item)
            videoLoader.item.player = null;
        playbackMode = "none";
        if (wasPip)
            closePictureInPicture();
        const live = connection.state === ConnectionController.Authenticated || window.offline;
        if (playbackPageShowing && live)
            pageStack.pop();
        playerPage = null;
        // Home is stale once a playback that outlived the player page ends
        if (wasMini && signedIn)
            connection.reloadHome();
    }

    function stopPlayback() {
        if (player)
            player.stop();
        endPlayback();
    }

    function stepVolume(steps) {
        // Turning the volume up is meant to be heard
        if (steps > 0 && player.muted)
            player.toggleMute();
        player.setVolume(Math.max(0, Math.min(100, player.volume + steps * 5)));
    }

    // The player commands of the keyboard and of the remote socket, in every mode; a name that is no command does nothing
    function playerAction(name) {
        // A stop also drops another session's Play commands still on their way, also before anything plays
        if (name === "stop") {
            remotePending = [];
            remotePlayWaiting = false;
        }
        if (playbackMode === "none" || !player)
            return;
        switch (name) {
        case "playpause": player.togglePause(); break;
        case "play": if (player.paused) player.resume(); break;
        case "pause": if (!player.paused) player.pause(); break;
        case "stop": stopPlayback(); break;
        case "next": player.next(); break;
        case "previous": player.previous(); break;
        case "fastforward": player.seekRelative(10); break;
        case "rewind": player.seekRelative(-10); break;
        case "volumeup": stepVolume(1); break;
        case "volumedown": stepVolume(-1); break;
        case "togglemute": player.toggleMute(); break;
        case "togglefullscreen":
            // A full screen needs the full player
            if ((playbackMode === "mini" || playbackMode === "pip") && !showPlayerPage())
                break;
            togglePlayerFullscreen();
            break;
        }
    }

    // The player page in front gives way to the mini player, or ends the playback when there is nothing playing (an error shown, an item that ended); Home asks for its rows again once the playback is over, since positions change while something plays (a playback that goes on in the mini player or picture in picture reloads them when it ends)
    onPlaybackPageShowingChanged: {
        if (playbackPageShowing) {
            if (playbackMode === "pip")
                closePictureInPicture();
            if (playbackMode === "mini" || playbackMode === "pip")
                playbackMode = "full";
            return;
        }
        if (playbackMode === "full") {
            if (player?.active)
                playbackMode = "mini";
            else
                endPlayback();
        }
        restoreVisibilityAfterPlayer();
        if (signedIn && playbackMode === "none") {
            connection.reloadHome();
            // The details page under the player has not seen the position or the watched state change: the stop event also updates it in place, this covers a socket that was down. Other pages follow the same event in place, a reload would reset their scroll
            if (pageStack.currentItem?.objectName === "itemDetailsPage")
                pageStack.currentItem.refreshPage();
        }
    }

    // The end of the playback, in every mode; the check is made a turn later, since the next item of a queue may already be starting
    Connections {
        target: window.playbackMode !== "none" ? window.player : null
        function onFinished() {
            Qt.callLater(() => {
                if (window.player && !window.player.active)
                    window.endPlayback();
            });
        }
        // The stream kept starving: in every mode the notification says what it plays at now
        function onQualityLowered(bitsPerSecond) {
            const mbps = bitsPerSecond / 1000000;
            window.showPassiveNotification(i18nc("@info %1 is a bit rate in Mbit/s", "Slow connection: playing at a lower quality (%1 Mbps)", Number(mbps).toLocaleString(Qt.locale(), "f", Number.isInteger(mbps) ? 0 : 1)));
        }
        function onQualityUnavailable() {
            window.showPassiveNotification(i18n("No lower quality available. Playback continues at the current quality"));
        }
        function onQualityChangeFailed() {
            window.showPassiveNotification(i18n("This quality is not available: playback goes on at the current one"));
        }
        // A clip or GIF saved while the user is anywhere in the app
        function onClipSaved(fileName, gif) {
            window.showPassiveNotification(gif ? i18n("GIF saved: %1", fileName) : i18n("Clip saved: %1", fileName));
        }
        function onSubtitleBusy() {
            window.showPassiveNotification(i18n("Another subtitle is still downloading: choose this one again once it has loaded"));
        }
        function onSubtitleExtracting() {
            window.showPassiveNotification(i18n("The server is extracting these subtitles from the video: the first time it can take a few minutes"), "long");
        }
        function onClipMarkChanged(text) {
            window.showPassiveNotification(text);
        }
        function onSubtitleFileAdded(title, copy) {
            // An administrator can keep it on the server too: the offer is the notification's action, for this item and this file
            const itemId = window.player.itemId;
            const itemName = window.player.title;
            if (window.player.canUploadAddedSubtitles())
                window.showPassiveNotification(i18n("Subtitle added"), "long", i18nc("@action:button", "Upload to Server…"), () => uploadSubtitleDialog.openFor(itemId, itemName, copy, title));
            else
                window.showPassiveNotification(i18n("Subtitle added"));
        }
        function onSubtitleFileRefused(reason) {
            window.showPassiveNotification(reason, "long");
        }
        function onClipSaveFailed(noSpace) {
            window.showPassiveNotification(noSpace ? i18n("The clip could not be saved: not enough free disk space. Free some space and try again") : i18n("The clip could not be saved. Try again"));
        }
    }

    // The record of the last local playback that started, overwritten by the next one
    property var localOwner: null

    // A completed download plays from its own file: no server request, no session report
    function playDownload(serverId, userId, itemId) {
        if (!window.player)
            return false;
        const info = connection.downloads.playableDownload(serverId, userId, itemId);
        if (!info.path) {
            window.showPassiveNotification(i18n("The downloaded file is not available: download the item again"));
            return false;
        }
        const episode = info.type === "Episode" && info.seriesName.length > 0;
        const title = episode ? info.seriesName : info.name;
        const subtitle = episode && info.seasonNumber > 0 && info.episodeNumber > 0
            ? i18nc("episode label and episode title", "%1 — %2", EpisodeLabel.label(info.seasonNumber, info.episodeNumber), info.name)
            : "";
        if (!window.player.playLocalFile(info.path, itemId, title, subtitle, info.positionTicks || 0, info)) {
            window.showPassiveNotification(i18n("The downloaded file could not be played: try again, or download the item again"));
            return false;
        }
        // Set only once the file plays: the player reports the previous file's last position while it is replaced, and that one belongs to the previous owner
        window.localOwner = {serverId: serverId, userId: userId, itemId: itemId};
        if (!showPlayerPage()) {
            window.player.stop();
            return false;
        }
        return true;
    }

    // In a SyncPlay group a play is the group's request: the player opens when the group's item starts (onItemOpened below)
    function playItem(itemId, resumeTicks) {
        if (!window.signedIn || !window.player || !window.player.playItem(itemId, resumeTicks || 0))
            return false;
        if (groupRouting)
            return true;
        if (!showPlayerPage()) { window.player.stop(); return false; }
        return true;
    }

    // A details page's Play with its before-playback track choices
    function playItemWithStreams(itemId, resumeTicks, audioStreamIndex, subtitleStreamIndex) {
        if (!window.signedIn || !window.player
                || !window.player.playItemWithStreams(itemId, resumeTicks || 0, audioStreamIndex, subtitleStreamIndex))
            return false;
        if (groupRouting)
            return true;
        if (!showPlayerPage()) { window.player.stop(); return false; }
        return true;
    }

    // A KRunner Play: signed out the item opens after sign-in; a play the player refuses opens its details page
    function playRequestedItem(itemId, resumeTicks) {
        if (playItem(itemId, resumeTicks || 0))
            return;
        requestedItemId = itemId;
        openRequestedItem(pageStack.get(0));
    }

    function playQueue(index, startTicks) {
        if (!window.signedIn || !window.player || !window.player.playQueue(index, startTicks || 0))
            return false;
        if (groupRouting)
            return true;
        if (!showPlayerPage()) { window.player.stop(); return false; }
        return true;
    }

    function resumeQueue() {
        if (!window.signedIn || !window.player || !window.player.resumeQueue())
            return false;
        if (groupRouting)
            return true;
        if (!showPlayerPage()) { window.player.stop(); return false; }
        return true;
    }

    // The group starts its item: the player opens, wherever the user is
    Connections {
        target: window.syncPlay
        function onItemOpened() {
            if (!window.showPlayerPage())
                window.player?.stop();
        }
    }

    function openSyncPlay() {
        shellHeader.openSyncPlay();
    }

    // Play Next (`next`) or Add to Queue: to the SyncPlay group's queue while the group takes the play actions, else to the user's
    function queueItem(itemId, next) {
        if (groupRouting) {
            if (syncPlay.queueItems([itemId], next))
                showPassiveNotification(next ? i18n("Added to play next in the SyncPlay group") : i18n("Added to the SyncPlay group’s queue"));
            else
                showPassiveNotification(i18n("Could not add this to the SyncPlay group’s queue. Check the connection and try again"));
            return;
        }
        connection.queue.add(itemId, next ? PlayQueue.Next : PlayQueue.Last);
    }

    // The only way to the details of an item: the pages above the one asking give way, and the item already showing is not pushed twice
    function openItem(itemId, fromPage) {
        if (connection.state !== ConnectionController.Authenticated)
            return;
        if (fromPage)
            popTo(fromPage);
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
                popTo(candidate);
                return;
            }
        }
        openItem(itemId, fromPage);
    }

    // The media of one person: one page at a time, another person updates it in place
    function openPerson(personId, personName, fromPage) {
        if (connection.state !== ConnectionController.Authenticated || !personId)
            return;
        for (let i = pageStack.depth - 1; i >= 0; --i) {
            const candidate = pageStack.get(i);
            if (candidate?.objectName === "personPage") {
                candidate.personName = personName;
                connection.personItems.open(personId);
                popTo(candidate);
                candidate.resetView();
                return;
            }
        }
        if (!connection.personItems.open(personId))
            return;
        popTo(fromPage);
        pageStack.push(Qt.resolvedUrl("PersonPage.qml"), { connection: connection, personName: personName });
    }

    // The page above the first one that holds changes the user has not saved (the User entry of Settings), or null
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
    function popToBase(animated) {
        const base = pageStack.get(0);
        if (base)
            popTo(base, animated);
    }

    // Ctrl+F, the Search entry of the rail and the search button of a Compact window: the search popup opens over the page in front with the term it had, and its field takes the keyboard. A player page in front goes first, its video to the mini player, as for any page change from it
    function openSearch() {
        if (connection.state !== ConnectionController.Authenticated)
            return;
        if (searchPopup.visible) {
            searchPopup.focusSearchField();
            return;
        }
        if (playbackPageShowing) {
            if (askBeforeLeaving(() => openSearch()))
                return;
            popToBase();
            // The page change above has run its handlers by now: the popup opens after them, or the page in front would close it
            Qt.callLater(() => {
                if (signedIn)
                    searchPopup.open();
            });
            return;
        }
        searchPopup.open();
    }

    // The search of the libraries, opened only by openSearch(); a result opens its details page above the page in front
    SearchPopup {
        id: searchPopup
        connection: window.connection
        onItemChosen: itemId => {
            window.openItem(itemId);
            Qt.callLater(window.focusCurrentPage);
        }
        onFocusLost: Qt.callLater(window.focusCurrentPage)
    }

    // The genre rails of the library page that asks (its model: a tab or a collections library) go above it, and the page stays below
    function openGenres(fromPage) {
        if (connection.state !== ConnectionController.Authenticated || !fromPage)
            return;
        if (!connection.openGenreRails(fromPage.model))
            return;
        popTo(fromPage);
        pageStack.push(Qt.resolvedUrl("GenreRailsPage.qml"), { connection: connection });
    }

    // The menu of a card (a right click or the Menu key): it opens at a point of the card, below it by default; `resumeTicks` is -1 for a card without a quick play, else its saved position (0: none)
    function openCardMenu(card, itemId, type, seriesId, x, y, resumeTicks, favorite, played) {
        if (!prepareCardMenu(card, itemId, type, seriesId, resumeTicks, favorite, played))
            return;
        cardMenu.popup(card, x ?? 0, y ?? card.height);
    }
    // The menu of the More Actions button of a card: below the button, above it when there is no room below, never over it
    function openCardMenuFrom(button, card, itemId, type, seriesId, resumeTicks, favorite, played) {
        if (prepareCardMenu(card, itemId, type, seriesId, resumeTicks, favorite, played))
            cardMenu.openFor(button);
    }
    function prepareCardMenu(card, itemId, type, seriesId, resumeTicks, favorite, played) {
        if (connection.state !== ConnectionController.Authenticated)
            return false;
        cardMenu.itemId = itemId;
        cardMenu.itemType = type;
        cardMenu.seriesId = seriesId;
        cardMenu.resumeTicks = resumeTicks ?? -1;
        cardMenu.favorite = favorite ?? false;
        cardMenu.played = played ?? false;
        cardMenu.positionTicks = card.resumeTicks ?? 0;
        return true;
    }

    function downloadFailureText(reason) {
        switch (reason) {
        case "forbidden":
            return i18n("The server does not allow downloads for this user: check the permissions of the account on the server or sign in again");
        case "notfound":
            return i18n("The item is no longer on the server: refresh the page to update it");
        case "unsupported":
            return i18n("This item cannot be downloaded");
        case "network":
            return i18n("Could not reach the server. Check the connection and try again");
        case "queue":
            return i18n("The download could not be queued: try again");
        }
        return i18n("The download could not be started: try again");
    }

    // The confirmation of Delete Download, from the card menu and the details page
    function confirmRemoveDownload(itemId) {
        removeDownloadDialog.itemId = itemId;
        removeDownloadDialog.open();
    }

    Connections {
        target: window.player
        // The shell tests have no player: the handler is only meaningful once one is attached
        ignoreUnknownSignals: true
        function onLocalPositionChanged(itemId, ticks, played) {
            const owner = window.localOwner;
            if (owner && owner.itemId === itemId)
                window.connection.downloads.updateLocalProgress(owner.serverId, owner.userId, itemId, ticks, played);
        }
        // The device stopped on its own or another controller took it: the player has closed
        function onCastStoppedElsewhere(deviceName) {
            window.showPassiveNotification(i18nc("@info %1 is a device name", "Playback was stopped on %1", window.plainNotificationText(deviceName)));
        }
    }

    Connections {
        target: connection.downloads
        function onDownloadFailed(itemId, reason) {
            window.showPassiveNotification(window.downloadFailureText(reason), "long");
        }
        function onDownloadEnqueued(itemId) {
            window.showPassiveNotification(i18n("Download queued"), "long", i18n("Show Downloads"), () => window.openDownloads());
        }
        function onExportBusy(itemId) {
            window.showPassiveNotification(i18n("Another export is still running. Try again when it ends"));
        }
        function onExportFinished(itemId, ok) {
            window.showPassiveNotification(ok ? i18n("Exported to the chosen folder") : i18n("Could not export the download. Choose another folder and try again"));
        }
    }

    CardMenu {
        id: cardMenu

        connection: window.connection
        playAvailable: !!window.player
        playOnAvailable: !window.groupRouting
        // A playlist opens its own page, everything else its details
        onOpenRequested: targetId => cardMenu.itemType === "Playlist" ? window.openPlaylist(targetId, window.pageStack.currentItem) : window.openItem(targetId, window.pageStack.currentItem)
        onPlayRequested: targetId => window.playItem(targetId)
        onPlayOnRequested: (targetId, ticks) => window.playOn(targetId, ticks)
        onResumeRequested: (targetId, ticks) => window.playItem(targetId, ticks)
        // A refused send means a write for the item already runs: its answer updates the card
        onFavoriteRequested: (targetId, favorite) => window.connection.setFavorite(targetId, favorite)
        onPlayedRequested: (targetId, played) => window.setItemPlayed(targetId, played, cardMenu.positionTicks, cardMenu.played)
        onDownloadRequested: targetId => window.connection.downloads.downloadItem(targetId)
        onCancelDownloadRequested: targetId => window.connection.downloads.removeCurrentDownload(targetId)
        onRemoveDownloadRequested: targetId => window.confirmRemoveDownload(targetId)
        onAddToPlaylistRequested: targetId => window.addToPlaylist(targetId)
        onQueueRequested: (targetId, next) => window.queueItem(targetId, next)
        queueAvailable: window.groupRouting || !window.connection.queue.busy
        onRenamePlaylistRequested: targetId => window.renamePlaylist(targetId, window.connection.playlists.nameOf(targetId))
        onDeletePlaylistRequested: targetId => window.askDeletePlaylist(targetId)
    }

    // A link opens above the active root tab after sign-in. Incoming links must not bypass the
    // unsaved-preferences prompt. A cancelled request is consumed, so an identical
    // link delivered later can trigger a fresh prompt.
    function openRequestedItem(home) {
        if (requestedItemId.length === 0 || connection.state !== ConnectionController.Authenticated
                || tabOfPage(home) === "")
            return;
        const itemId = requestedItemId;
        requestedItemId = "";
        const request = ++itemRequestGeneration;
        const navigate = () => {
            if (request !== itemRequestGeneration || connection.state !== ConnectionController.Authenticated)
                return;
            const base = pageStack.get(0);
            if (tabOfPage(base) !== "")
                openItem(itemId, base);
        };
        if (askBeforeLeaving(navigate))
            return;
        navigate();
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
        case ConnectionController.Offline:
            return 4;
        default:
            return 0;
        }
    }
    onPageGroupChanged: {
        // An earlier link awaiting an unsaved-changes decision cannot cross users.
        // Do not invalidate an item still queued for the initial sign-in.
        if (pageGroup !== 2)
            itemRequestGeneration++;
        // A link waiting for a sign-in belongs to the server the user is leaving
        if (pageGroup === 0)
            requestedItemId = "";
        for (const tab of Object.keys(tabState))
            delete tabState[tab];
        for (const tab of Object.keys(oneOffTabs))
            delete oneOffTabs[tab];
        showPageForState();
        Qt.callLater(settleStartup);
    }

    // A link that arrives while Home is already showing (a second launch) opens at once
    onRequestedItemIdChanged: openRequestedItem(pageStack.get(0))

    // The section the page in front belongs to: the header marks it (a tab, the main menu button, the queue button); a page opened above a type tab belongs to that tab, and one opened from a collections tile to that library; the Settings page belongs to no tab, and the header search is global there and on the Favorites and Playlists tabs. Settings needs no session, so it is marked whatever the connection is
    readonly property string currentSection: {
        // The search popup, while it shows, is the section the rail marks
        if (searchPopup.visible)
            return "search";
        if (pageStack.currentItem?.objectName === "settingsPage")
            return "settings";
        // The automatic downloads page opened from Settings belongs to it
        if (pageStack.currentItem?.objectName === "downloadRulesPage" && pageStack.currentIndex > 0 && pageStack.get(pageStack.currentIndex - 1)?.objectName === "settingsPage")
            return "settings";
        if (!signedIn)
            return "";
        for (let i = 0; i <= pageStack.currentIndex; ++i) {
            const page = pageStack.get(i);
            if (page?.objectName === "libraryPage" && page.tabType === "")
                return "library:" + connection.library.libraryId;
        }
        if (pageStack.currentItem?.objectName === "queuePage")
            return "queue";
        return rootTab !== "" ? rootTab : "home";
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
        case "favoritesPage":
            return "favorites";
        case "playlistsPage":
            return "playlists";
        case "downloadsPage":
            return "downloads";
        case "placeholderSectionPage":
            return page.section;
        }
        return "";
    }

    // Whether the server has a library of the type: the tab of a type it lacks is not offered
    function hasTypeTab(name) {
        return name === "movies" ? connection.movieLibraries.length > 0 : connection.showLibraries.length > 0;
    }

    // The tab becomes the first page of the stack: what was there goes, the tab it leaves is remembered, and the one it comes back to is restored. A type tab opens its list (the stored library when `libraryId` is empty, else that library, which a Home tile persists); the tab already in front only drops the pages above it. Favorites, Playlists, Music and Audiobooks (a placeholder page) are root tabs too and open at the top. `recentlyAdded` opens the type tab on the one-off Recently Added view instead of the stored one
    function openTab(name, libraryId) {
        openTabScope(name, libraryId, false);
    }
    function openTabScope(name, libraryId, recentlyAdded) {
        if ((!signedIn && !offline) || (offline && name !== "downloads")
            || askBeforeLeaving(() => openTabScope(name, libraryId, recentlyAdded)))
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
            // Back to the tab's own page: the row slides back to it
            popToBase(true);
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
        // Queued before the stack changes, so an error on the way cannot leave the row invisible
        if (leavingTab !== "" && prepareTabMotion())
            Qt.callLater(startTabMotionWhenReady, leavingTab, name);
        parkVideo();
        pageStack.clear();
        // Leaving the type pages gives the sub-tab's data and channels back
        if (!isTypeTab(name))
            connection.closeSubtabs();
        let page = null;
        if (name === "home")
            page = pageStack.push(Qt.resolvedUrl("HomePage.qml"), { connection: connection });
        else if (name === "favorites")
            page = pageStack.push(Qt.resolvedUrl("FavoritesPage.qml"), { connection: connection, shell: window });
        else if (name === "playlists")
            page = pageStack.push(Qt.resolvedUrl("PlaylistsPage.qml"), { connection: connection, shell: window });
        else if (name === "downloads")
            page = pageStack.push(Qt.resolvedUrl("DownloadsPage.qml"), { connection: connection });
        else if (name === "music" || name === "audiobooks")
            page = pageStack.push(Qt.resolvedUrl("PlaceholderSectionPage.qml"), { section: name });
        else
            page = pageStack.push(Qt.resolvedUrl("LibraryPage.qml"), { model: name === "movies" ? connection.movies : connection.shows, tabType: name, connection: connection });
        if (tabState[name] && page?.restoreViewPosition)
            page.restoreViewPosition(tabState[name]);
    }

    // The visible order of the sections, which decides the side the new one slides in from
    function tabOrder(name) {
        return navigation.entries.findIndex(entry => entry.section === name);
    }

    // The new tab enters from the side it has in the sidebar; the row stays invisible from before the push until the motion starts, so the new page never shows one frame at rest
    property bool tabMotionQueued: false
    // The index of the page the queued motion shows (0 for a tab, the page of a header section above it): a page pushed above that one ends the motion
    property int tabMotionIndex: 0
    readonly property bool tabSwitching: tabMotionQueued || tabMotion.running
    ParallelAnimation {
        id: tabMotion
        OpacityAnimator { target: pageStack; from: 0; to: 1; duration: Kirigami.Units.longDuration; easing.type: Easing.InOutCubic }
        NumberAnimation { target: tabShift; property: "x"; to: 0; duration: Kirigami.Units.longDuration; easing.type: Easing.OutCubic }
    }
    // A running or waiting motion ends at once, with the row fully visible and in place; true when a new one is queued (no animation speed, no motion). `index` is the page the new motion is for
    function prepareTabMotion(index) {
        finishTabMotion();
        tabMotionIndex = index ?? 0;
        if (Kirigami.Units.longDuration === 0)
            return false;
        pageStack.opacity = 0;
        tabMotionQueued = true;
        return true;
    }
    function finishTabMotion() {
        tabMotionQueued = false;
        tabReadyWait.stop();
        // stop, not complete: a group with an animator may still count as running after complete, and the next start would then do nothing
        tabMotion.stop();
        pageStack.opacity = 1;
        tabShift.x = 0;
    }
    // The new page fades in whole: a library tab lays its toolbar out and builds its grid over a few frames, and the motion waits for both (`contentReady`), never longer than the wait's cap
    function startTabMotionWhenReady(fromTab, toTab) {
        if (!tabMotionQueued)
            return;
        if (pageStack.currentItem?.contentReady ?? true) {
            startTabMotion(fromTab, toTab);
            return;
        }
        tabReadyWait.fromTab = fromTab;
        tabReadyWait.toTab = toTab;
        tabReadyWait.since = Date.now();
        tabReadyWait.restart();
    }
    Timer {
        id: tabReadyWait
        objectName: "tabReadyWait"
        property string fromTab
        property string toTab
        property double since: 0
        // The longest the new page stays hidden, in milliseconds
        property int limit: 500
        interval: 16
        repeat: true
        onTriggered: {
            if ((window.pageStack.currentItem?.contentReady ?? true) || Date.now() - since >= limit) {
                stop();
                window.startTabMotion(fromTab, toTab);
            }
        }
    }
    function startTabMotion(fromTab, toTab) {
        if (!tabMotionQueued)
            return;
        // The same place (a state change, a page brought back) only fades
        const fromRight = (tabOrder(toTab) > tabOrder(fromTab)) !== (Qt.application.layoutDirection === Qt.RightToLeft);
        tabShift.x = fromTab === toTab ? 0 : (fromRight ? 1 : -1) * Kirigami.Units.gridUnit * 2;
        tabMotion.restart();
        tabMotionQueued = false;
    }

    // Show All of a Recently Added row: the type tab opens from the top on the library's items by date added, newest first, and the sort saved for the tab stays as it was
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

    // Show All of Continue Watching goes above Home: a page already open is brought back as it is
    function openResumeItems() {
        if (!signedIn)
            return;
        for (let i = 0; i <= pageStack.currentIndex; ++i) {
            const page = pageStack.get(i);
            if (page?.objectName === "resumeItemsPage") {
                popTo(page);
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

    // Show All of a Favorites row goes above the Favorites page that asks, which stays below as it is; the page of that type already open is brought back instead. It reads the same snapshot, so no scan starts here
    function openFavoritesSection(type, title, fromPage) {
        if (!signedIn)
            return;
        for (let i = 0; i <= pageStack.currentIndex; ++i) {
            const page = pageStack.get(i);
            if (page?.objectName === "favoritesSectionPage" && page.type === type) {
                popTo(page);
                return;
            }
        }
        if (fromPage)
            popTo(fromPage);
        pageStack.push(Qt.resolvedUrl("FavoritesSectionPage.qml"), { connection: connection, shell: window, type: type, title: title });
    }

    // The Playlists tab and the Home tile of the server's Playlists view: the page becomes the root of the stack
    function openPlaylists() {
        if (!signedIn)
            return;
        openTab("playlists");
    }

    // The Downloads tab: the queue and what is available on this device
    function openDownloads() {
        if (!signedIn && !offline)
            return;
        hideDownloadsNotices();
        openTab("downloads");
    }

    // The "Download queued" notice offers this tab, so it goes as the tab opens, whatever opened it (its button, the rail, a shortcut). The oldest notice hides first and at most four stack, so the loop leaves none
    function hideDownloadsNotices() {
        for (let i = 0; i < 4; ++i)
            hidePassiveNotification();
    }
    onRootTabChanged: {
        if (rootTab === "downloads")
            hideDownloadsNotices();
    }

    // The automatic download rules open above the Downloads tab, which a page elsewhere gives way to after the question about unsaved changes; from Settings they open above it; a page of them already open is brought back
    function openDownloadRules() {
        if (!signedIn && !offline)
            return;
        for (let i = 0; i <= pageStack.currentIndex; ++i) {
            const page = pageStack.get(i);
            if (page?.objectName === "downloadRulesPage") {
                popTo(page);
                return;
            }
        }
        // From Settings the page opens above it, and Back returns there
        if (pageStack.currentItem?.objectName === "settingsPage") {
            pageStack.push(Qt.resolvedUrl("DownloadRulesPage.qml"), { connection: connection });
            return;
        }
        if (askBeforeLeaving(() => openDownloadRules()))
            return;
        if (rootTab !== "downloads")
            openTab("downloads");
        else
            popToBase();
        pageStack.push(Qt.resolvedUrl("DownloadRulesPage.qml"), { connection: connection });
    }

    // A playlist opens above the page asking (the pages above it give way); `name`, when the caller knows it, titles the page
    function openPlaylist(playlistId, fromPage, name) {
        if (connection.state !== ConnectionController.Authenticated)
            return;
        if (fromPage)
            popTo(fromPage);
        pageStack.push(Qt.resolvedUrl("PlaylistPage.qml"), { connection: connection, shell: window, playlistId: playlistId, name: name ?? "" });
    }

    PlaylistDialogs {
        id: playlistDialogs
        connection: window.connection
        shell: window
    }

    CollectionDialogs {
        id: collectionDialogs
        connection: window.connection
        shell: window
    }

    // Keeps a subtitle file added to the playing item on the server (the notification's Upload…)
    UploadSubtitleDialog {
        id: uploadSubtitleDialog
        connection: window.connection
        shell: window
    }

    // Cast… of a video: with an item (the card menu, the details page) every choice plays that item there, This Computer included; without one (the OSD) the playing video moves to the choice
    CastDialog {
        id: castDialog
        player: window.player
        onTargetChosen: (udn, name) => {
            if (!window.player)
                return;
            let started = false;
            if (castDialog.itemId.length > 0) {
                started = window.player.playItemOn(udn, castDialog.itemId, castDialog.startTicks);
                if (started && !groupRouting && !showPlayerPage()) {
                    window.player.stop();
                    started = false;
                }
            } else {
                started = window.player.castTo(udn);
            }
            if (!started)
                window.showPassiveNotification(udn === "local" ? i18n("Could not start playback: try again") : i18nc("@info %1 is a device name", "Could not start playback on %1: open Cast again to search for it", window.plainNotificationText(name)));
        }
    }

    // The notification label reads its text as HTML when it holds a tag or an escaped one in its first line, and a device names itself: its name must not be markup there
    function plainNotificationText(text) {
        return text.replace(/</g, "\u2039").replace(/&(?=lt;)/gi, "&\u200b");
    }

    function playOn(itemId, ticks) {
        if (!signedIn || !window.player || groupRouting)
            return;
        castDialog.itemId = itemId;
        castDialog.startTicks = ticks || 0;
        castDialog.open();
    }

    // The OSD's Cast: the playing video moves to the chosen device, or back to this computer
    function castCurrent() {
        if (!window.player || groupRouting)
            return;
        castDialog.itemId = "";
        castDialog.startTicks = 0;
        castDialog.open();
    }

    // Add to Playlist… of a video, from its card menu or its details page
    function addToPlaylist(itemId) {
        if (signedIn)
            playlistDialogs.addTo(itemId);
    }
    // Add to Collection… of a movie or a show
    function addToCollection(itemId, itemName) {
        if (signedIn)
            collectionDialogs.addTo(itemId, itemName);
    }
    // New Playlist… of the Playlists page: an empty playlist
    function newPlaylist() {
        if (signedIn)
            playlistDialogs.create([]);
    }
    // Rename… and Delete… of a playlist, from its card menu or its page; `name` fills the field
    function renamePlaylist(playlistId, name) {
        if (signedIn)
            playlistDialogs.rename(playlistId, name);
    }
    function askDeletePlaylist(playlistId) {
        if (signedIn)
            playlistDialogs.askDelete(playlistId);
    }

    // The page of a deleted playlist closes, with the pages above it
    Connections {
        target: window.connection.playlists
        function onDeleted(playlistId) {
            const key = playlistId.replace(/-/g, "").toLowerCase();
            for (let i = 1; i < window.pageStack.depth; ++i) {
                const page = window.pageStack.get(i);
                if (page?.objectName === "playlistPage" && page.playlistId.replace(/-/g, "").toLowerCase() === key) {
                    window.popTo(window.pageStack.get(i - 1), true);
                    return;
                }
            }
        }
    }

    function playlistsPageGone() {
        if (--playlistsPages === 0)
            connection.playlists.close();
    }
    function playlistPageGone() {
        if (--playlistPages === 0)
            connection.playlistEntries.close();
    }

    // Play and Shuffle of a playlist: the saved queue becomes it (one step its Undo takes back) and plays from `startAt`, its first item when empty
    function playPlaylist(playlistId, startAt, shuffled) {
        if (!signedIn || !window.player || playlistWaiting || remotePlayWaiting)
            return false;
        playlistWaiting = true;
        playlistRequest++;
        playlistStartAt = startAt;
        playlistQueueBefore = connection.queue.count;
        if (!connection.queue.replace(playlistId, shuffled)) {
            playlistWaiting = false;
            return false;
        }
        return true;
    }

    Connections {
        target: window.connection.queue
        enabled: window.playlistWaiting
        function onReplaced(count, truncated) {
            window.playlistWaiting = false;
            const index = window.playlistStartAt.length > 0 ? window.connection.queue.indexOf(window.playlistStartAt) : 0;
            // The entry asked for did not fit in the queue (its cap, or it is not a video): nothing starts in its place
            if (index < 0) {
                window.showPassiveNotification(i18np("The queue holds the first %1 item of the playlist: this entry is not among them", "The queue holds the first %1 items of the playlist: this entry is not among them", count));
                return;
            }
            window.playQueue(index);
            const message = truncated ? i18np("Only the first %1 item went into the queue", "Only the first %1 items went into the queue", count) : i18n("The queue now holds the playlist");
            if (window.playlistQueueBefore > 0)
                window.showPassiveNotification(message, "long", i18nc("@action:button takes the queue back", "Undo"), () => window.connection.queue.undo());
            else if (truncated)
                window.showPassiveNotification(message);
        }
        // The queue's own messages tell a failure or an empty playlist; an operation the queue dropped (a sign-out, a clear) sends nothing, so the wait ends with it
        function onBusyChanged() {
            if (window.connection.queue.busy)
                return;
            const request = window.playlistRequest;
            Qt.callLater(() => {
                if (window.playlistRequest === request)
                    window.playlistWaiting = false;
            });
        }
    }

    // What another Jellyfin session asks of this one; its keys and player commands take the remote-control socket's path (main.cpp), the rest comes here
    Connections {
        target: window.connection.remoteCommands
        function onSeekRequested(ticks) {
            if (window.playbackMode === "none" || !window.player)
                return;
            window.player.seek(ticks);
            if (window.playbackMode === "full")
                window.playerPage?.wakeChrome();
        }
        function onVolumeRequested(level) {
            window.player?.setVolume(level);
        }
        function onMuteRequested(muted) {
            if (window.player && window.player.muted !== muted)
                window.player.toggleMute();
        }
        function onAudioStreamRequested(index) {
            if (window.playbackMode !== "none")
                window.player?.selectAudioStreamIndex(index);
        }
        function onSubtitleStreamRequested(index) {
            if (window.playbackMode !== "none")
                window.player?.selectSubtitleStreamIndex(index);
        }
        function onQualityRequested(bitsPerSecond) {
            if (window.playbackMode !== "none")
                window.player?.setQualityBitrate(bitsPerSecond);
        }
        function onRepeatModeRequested(mode) {
            window.player?.setRepeatMode(mode);
        }
        function onShuffleRequested(shuffled) {
            window.player?.setShuffle(shuffled);
        }
        function onPlayNowRequested(itemIds, startIndex, startTicks) {
            window.requestRemotePlay({ now: true, itemIds: itemIds, next: false, startIndex: startIndex, startTicks: startTicks });
        }
        function onQueueRequested(itemIds, next) {
            window.requestRemotePlay({ now: false, itemIds: itemIds, next: next, startIndex: 0, startTicks: 0 });
        }
    }

    // The waits ending let the next of another session's Play commands run
    onRemotePlayWaitingChanged: if (!remotePlayWaiting) Qt.callLater(runRemotePlay)
    onPlaylistWaitingChanged: if (!playlistWaiting) Qt.callLater(runRemotePlay)

    // Another session's Play commands run in the order they came, each once the queue is free; a few wait at most (the newest are kept)
    function requestRemotePlay(command) {
        if (!signedIn)
            return;
        remotePending = remotePending.concat([command]).slice(-8);
        runRemotePlay();
    }
    function runRemotePlay() {
        if (remotePending.length === 0 || !signedIn || connection.queue.busy || playlistWaiting || remotePlayWaiting)
            return;
        const command = remotePending[0];
        remotePending = remotePending.slice(1);
        const started = command.now ? playRemote(command.itemIds, command.startIndex, command.startTicks)
                                    : connection.queue.addFromRemote(command.itemIds, command.next ? PlayQueue.Next : PlayQueue.Last);
        // A queue that refuses (it could not be read back) refuses the rest too: its own message says so once
        if (!started)
            remotePending = [];
    }
    Connections {
        target: window.connection.queue
        enabled: window.remotePending.length > 0
        function onBusyChanged() {
            if (!window.connection.queue.busy)
                Qt.callLater(window.runRemotePlay);
        }
    }

    // Another session's PlayNow: the items become the queue (one step its Undo takes back) and play from the one it named; a single video plays on its own, as from its details page
    function playRemote(itemIds, startIndex, startTicks) {
        if (!signedIn || !window.player || playlistWaiting || remotePlayWaiting)
            return false;
        remotePlayWaiting = true;
        remotePlayRequest++;
        remoteStartAt = itemIds[Math.min(startIndex, itemIds.length - 1)];
        remoteStartTicks = startTicks;
        remoteQueueBefore = connection.queue.count;
        if (!connection.queue.replaceWith(itemIds)) {
            remotePlayWaiting = false;
            return false;
        }
        return true;
    }

    Connections {
        target: window.connection.queue
        enabled: window.remotePlayWaiting
        function onReplaced(count, truncated) {
            window.remotePlayWaiting = false;
            // The item named may have been left out (not a video, past the cap): the queue then starts at its top
            const index = window.connection.queue.indexOf(window.remoteStartAt);
            window.playQueue(Math.max(0, index), index >= 0 ? window.remoteStartTicks : 0);
            if (window.remoteQueueBefore > 0)
                window.showPassiveNotification(i18n("Another device replaced the queue"), "long", i18nc("@action:button takes the queue back", "Undo"), () => window.connection.queue.undo());
        }
        function onVideoAlone(itemId) {
            window.remotePlayWaiting = false;
            window.playItem(itemId, window.remoteStartTicks);
        }
        // A failure or nothing to play ends the wait with the queue's own message
        function onBusyChanged() {
            if (window.connection.queue.busy)
                return;
            const request = window.remotePlayRequest;
            Qt.callLater(() => {
                if (window.remotePlayRequest === request)
                    window.remotePlayWaiting = false;
            });
        }
    }

    // The socket was down long enough for a change to be missed: Home and the page in front read their data again (not under the full player: the end of the playback reloads Home and the page it returns to)
    Connections {
        target: window.connection
        function onServerEventsMissed() {
            if (!window.signedIn || window.playbackMode === "full")
                return;
            window.connection.reloadHome();
            const page = window.pageStack.currentItem;
            if (typeof page?.refreshPage === "function")
                page.refreshPage();
        }
    }

    // A session that ends takes what it had asked for
    Connections {
        target: window.connection
        function onStateChanged() {
            if (window.connection.state !== ConnectionController.Authenticated) {
                window.remotePending = [];
                window.remotePlayWaiting = false;
            }
        }
    }

    // The Favorites tab and the Favorites row of Home: the page becomes the root of the stack (it starts the scan when it is created)
    function openFavorites() {
        if (!signedIn)
            return;
        openTab("favorites");
    }

    // The Home tab; before a session it is the page the state wants
    function goHome() {
        if (signedIn)
            openTab("home");
        else if (offline)
            openTab("downloads");
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
                popTo(page);
                return;
            }
        }
        const home = pageStack.get(0);
        if (home)
            popTo(home);
        if (connection.openLibrary(libraryId))
            pageStack.push(Qt.resolvedUrl("LibraryPage.qml"), { model: connection.library, libraryName: libraryName, connection: connection });
    }

    // A type that vanishes from the server while its tab shows sends the user home
    Connections {
        target: window.connection
        // A menu left open over a session that ended would act on an item of the server the user left
        function onStateChanged() {
            if (window.connection.state !== ConnectionController.Authenticated) {
                cardMenu.close();
                window.endPlayback();
            }
        }
        function onLibrariesChanged() {
            if (window.signedIn && window.connection.state === ConnectionController.Authenticated && window.isTypeTab(window.rootTab) && !window.hasTypeTab(window.rootTab))
                window.goHome();
        }
    }

    // A favorite or watched-state write started from a card that the server refused
    Connections {
        target: window.connection
        function onFavoriteUpdateFailed() {
            window.showPassiveNotification(i18n("Could not update Favorites. Check the connection and try again"));
        }
        function onPlayedUpdateFailed(itemId) {
            delete window.watchedUndo[itemId];
            window.showPassiveNotification(i18n("Could not change the watched state. Check the connection and try again"));
        }
        // A Mark as Watched from a card took the item off Continue Watching: the notification offers it back, with its resume point
        function onPlayedUpdated(itemId, played) {
            if (!played || !(itemId in window.watchedUndo))
                return;
            const undo = window.watchedUndo[itemId];
            delete window.watchedUndo[itemId];
            window.showPassiveNotification(i18n("Marked as watched"), "long", i18nc("@action:button", "Undo"), () => {
                // The notification can outlive the session it was for: another user's data is never touched
                if (!window.signedIn || window.connection.currentUserId !== undo.userId)
                    return;
                if (!window.connection.undoPlayed(itemId, undo.positionTicks, undo.played))
                    window.showPassiveNotification(i18n("Could not change the watched state. Check the connection and try again"));
            });
        }
    }

    // The saved position and watched state of each item a card marked watched, until the server confirms the write
    readonly property var watchedUndo: ({})

    // Mark as Watched or Unwatched from a card: the card menu and Continue Watching's button; a write still on its way for the item refuses another; `wasPlayed` is the server's Played before the write, which a rewatch in progress has and its Undo keeps
    function setItemPlayed(itemId, played, positionTicks, wasPlayed) {
        if (!signedIn)
            return;
        if (!connection.setPlayed(itemId, played)) {
            showPassiveNotification(i18n("Could not change the watched state: another change to this item is still being saved. Try again in a moment"));
            return;
        }
        if (played)
            watchedUndo[itemId] = {
                positionTicks: positionTicks > 0 ? positionTicks : 0,
                played: !!wasPlayed,
                userId: connection.currentUserId
            };
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
        let existing = -1;
        for (let i = 0; i <= pageStack.currentIndex && existing < 0; ++i) {
            if (pageStack.get(i)?.objectName === name)
                existing = i;
        }
        if (existing >= 0 && pageStack.get(existing) === pageStack.currentItem)
            return;
        if (existing < 0 && askBeforeLeaving(() => openSection(name, url, properties)))
            return;
        // The section fades in over Kirigami's own slide of the push (only the fade: a second slide would fight it; the slide's duration must stay, a zero one leaves the focus behind)
        if (prepareTabMotion(existing >= 0 ? existing : 1))
            Qt.callLater(startTabMotionWhenReady, name, name);
        if (existing >= 0) {
            popTo(pageStack.get(existing));
        } else {
            popToBase();
            pageStack.push(Qt.resolvedUrl(url), properties);
        }
    }

    // Pushing before the window's page row is complete leaves the page outside the scene
    function openStartupPage() {
        if (startupPage === "about") {
            shellHeader.showAboutDialog();
        } else if (startupPage === "settings") {
            pageStack.push(Qt.resolvedUrl("SettingsPage.qml"), { settings: settings, connection: connection });
        } else if (startupPage === "login" || startupPage === "home") {
            // The pages of a session, shown without one: the smoke test loads them in the real binary
            pageStack.push(Qt.resolvedUrl(startupPage === "login" ? "LoginPage.qml" : "HomePage.qml"), { connection: connection });
        }
    }

    Kirigami.PromptDialog {
        id: removeDownloadDialog
        objectName: "removeDownloadDialog"

        property string itemId

        title: i18n("Delete Download")
        preferredWidth: Kirigami.Units.gridUnit * 26
        standardButtons: Kirigami.Dialog.Cancel
        customFooterActions: [
            Kirigami.Action {
                objectName: "removeDownloadConfirm"
                text: i18n("Delete Download")
                icon.name: "edit-delete"
                onTriggered: {
                    window.connection.downloads.removeCurrentDownload(removeDownloadDialog.itemId);
                    removeDownloadDialog.close();
                }
            }
        ]

        QQC2.Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
            text: i18n("Delete this download? The file stays on the server")
        }
    }

    Component.onCompleted: {
        applySubTabs();
        connection.setHomeFavoritesEnabled(homeFavoritesEnabled);
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
