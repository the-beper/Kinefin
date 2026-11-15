// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as QQC2
import org.kde.kirigami as Kirigami

Kirigami.ApplicationWindow {
    id: window

    // The connection to the Jellyfin server, created by the application
    required property ConnectionController connection
    required property ClientSettings settings
    // Provided by the executable; optional for shell tests that have no mpv renderer.
    property var player: null
    // The remote-control socket, provided by the executable; null in shell tests
    property var remoteInput: null
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
    // A server is chosen while its sign-in or a session is in progress: the account menu shows for it
    readonly property bool serverChosen: connection.state !== ConnectionController.Idle
    // The header modes in pixels, from grid units: tests derive their window sizes from these (Compact below 24, Wide from 36)
    readonly property int compactLimit: Kirigami.Units.gridUnit * 24
    readonly property int wideLimit: Kirigami.Units.gridUnit * 36
    // The widest a block of reading content (item header, queue rows) gets before it is centred: the pages read it from here so they agree
    readonly property real readingWidth: Kirigami.Units.gridUnit * 70
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
    // The Playlists and Playlist pages alive (one deleted late may outlive the next one): the shared models stop reading with the last
    property int playlistsPages: 0
    property int playlistPages: 0
    // A playlist on its way into the queue, and the item it starts at (its first when empty); the queue answers here whatever page asked
    property bool playlistWaiting: false
    property string playlistStartAt: ""
    property int playlistQueueBefore: 0
    // Numbers the waits, so the end of one never ends the next
    property int playlistRequest: 0

    property bool fullscreenSetting: settings.value("main", "fullscreen")
    property bool alwaysOnTopSetting: settings.value("main", "alwaysOnTop")
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
    flags: alwaysOnTopSetting ? (Qt.Window | Qt.WindowStaysOnTopHint) : Qt.Window

    // Auto resolves to the Breadcrumb header only on mobile in narrow mode, and that one renders titles without plain-text formatting: the pin keeps server-supplied page titles in the toolbar title delegate, which is plain text
    // The row keeps one toolbar style for every page of its life: a style that follows the stack depth resizes the columns while a page slides out (the toolbar row above them bounces the column height), and Kirigami then reads the resize as a layout pass, stops the exit slide without finishing it, and the page it was removing is never destroyed. So the style stays ToolBar, and the pages that don't put actions in the toolbar turn their own toolbar off (the root tabs, the pre-signin screens, the settings)
    pageStack.globalToolBar.style: Kirigami.ApplicationHeaderStyle.ToolBar
    // One page at a time: the default switches to side-by-side columns from 40 grid units, which pushes the pages beyond the window
    pageStack.columnView.columnResizeMode: Kirigami.ColumnView.SingleColumn
    // The tab switch slides the whole row: Kirigami binds the transform and the opacity of every page, so a page is never the target
    pageStack.transform: Translate { id: tabShift }
    pageStack.globalToolBar.showNavigationButtons: Kirigami.ApplicationHeaderStyle.ShowBackButton

    // The bar with the menus and the Home tab; it spans the window above the pages
    header: ShellHeader {
        id: shellHeader
        visible: !window.playbackPageShowing && !window.startupVisible

        shell: window
    }

    // A visible connection target stays on screen for at least one second after
    // selection. This does not delay the request or keep a failed connection busy.
    property bool serverSelectionPending: false
    Timer {
        id: serverSelectionTimer
        interval: 1000
        repeat: false
        onTriggered: {
            window.serverSelectionPending = false;
            window.showPageForState();
        }
    }
    function beginServerSelection() {
        serverSelectionPending = true;
        serverSelectionTimer.restart();
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
    Timer {
        interval: 500
        running: window.startupVisible
        repeat: false
        onTriggered: { window.startupMinimumElapsed = true; window.settleStartup(); }
    }
    Timer {
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
        // The page leaves the stack at once: going back by moving the index leaves it there until a scroll animation ends
        const target = pageStack.get(index - 1);
        if (pageStack.get(index) === leaving)
            popTo(target);
    }

    // A back that only moves the index (Esc, the header button, the toolbar back button) leaves the
    // page it leaves in the stack, hidden above the current one and still counted, with its models
    // alive. Kirigami's own popHiddenPages option took such pages once the row settled, but it also
    // ran while a push was still placing its page mid-slide, and popped the page it had just
    // received. So the leftovers are taken here, deferred to the next turn of the event loop: by
    // then every push has landed and every unwind has taken the pages it was removing, and pages
    // above the current index can only be leftovers a back left behind
    function popLeftoverPages() {
        Qt.callLater(() => {
            while (pageStack.currentIndex >= 0 && pageStack.currentIndex < pageStack.depth - 1)
                pageStack.pop(pageStack.get(pageStack.currentIndex));
        });
    }

    // Esc goes back like Alt+Left except on the first page; an open dialog takes the key first
    readonly property bool canEscapeBack: pageStack.currentIndex > 0 && pageStack.layers.depth <= 1
    function escapeBack() {
        if (canEscapeBack)
            pageStack.goBack();
    }
    Shortcut {
        sequences: ["Escape"]
        enabled: window.canEscapeBack
        onActivated: window.escapeBack()
    }

    // What a remote control asks for besides the keys it sends: the player commands reach the player in every mode, the full page or the mini player
    function popupOpen() {
        for (const item of window.overlay?.children ?? []) {
            if (item.visible && String(item).startsWith("QQuickPopupItem"))
                return true;
        }
        return false;
    }
    function remoteAction(name) {
        switch (name) {
        case "back":
            // A popup closes on a real Escape through a shortcut, which a synthesised key never reaches: an open popup keeps the page under it as it is
            if (!popupOpen())
                escapeBack();
            break;
        case "home":
            goHome();
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
    // Brings the mini player back to the full player
    Shortcut {
        sequences: ["Ctrl+Shift+P"]
        enabled: window.playbackMode === "mini"
        onActivated: window.expandPlayer()
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
        // Typing in the header box is never interrupted, not even by the page the typing itself opened
        if (!page || shellHeader.searchFieldHasFocus)
            return;
        applyPageFocus(page);
    }
    Connections {
        target: window.pageStack
        function onCurrentItemChanged() {
            // A page opened above a tab still waiting to fade in shows at once
            if (window.tabMotionQueued && window.pageStack.currentIndex > 0)
                window.finishTabMotion();
            // A page a Back left hidden above the current one is taken once the stack settles
            window.popLeftoverPages();
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
            window.alwaysOnTopSetting = window.settings.value("main", "alwaysOnTop");
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
        // A full-screen or maximized window has no geometry of its own to keep; a full screen the player entered keeps the window it came from
        if (window.visibility === Window.Windowed)
            settings.setWindowGeometry(Qt.rect(x, y, width, height));
        else if (playerFullscreen && visibilityBeforeFullscreen === Window.Windowed)
            settings.setWindowGeometry(geometryBeforeFullscreen);
    }

    // The page follows the connection state: Servers until a server is chosen, the user picker while one of several saved users is to be chosen, sign-in until there is a session, Home after
    function showPageForState() {
        if (serverSelectionPending && pageGroup !== 0)
            return;
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
        // A failed connection must not wipe out the form/error while the
        // minimum selection interval elapses.
        if (page === "ServersPage.qml" && pageStack.depth === 1
            && pageStack.currentItem?.objectName === "serversPage") {
            Qt.callLater(settleStartup);
            return;
        }
        finishTabMotion();
        parkVideo();
        pageStack.clear();
        const shown = pageStack.push(Qt.resolvedUrl(page), properties);
        openRequestedItem(shown);
        Qt.callLater(settleStartup);
    }

    readonly property bool playbackPageShowing: pageStack.currentItem?.objectName === "playerPage"

    // How the playback shows: "none" while nothing plays, "full" while the player page is in front, "mini" while it goes on under another page. The C++ side knows nothing of it
    property string playbackMode: "none"
    // The player page pushed last; QML clears it when the page is deleted
    property Item playerPage: null
    // Where the video item sits in the current mode: the slot of the player page or of the mini player, none without playback
    readonly property Item videoSlot: playbackMode === "full" ? (playerPage?.videoSlot ?? null) : playbackMode === "mini" ? miniPlayer.videoSlot : null
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

    // Pops to a page of the stack; the video is parked first when the page in front is the player's, and then, as Kirigami's Back does, only the current index moves: the pages left above are taken once the stack settles, which deletes them (a pop of the page in front does not)
    function popTo(page) {
        if (page && page !== pageStack.currentItem && playbackPageShowing) {
            parkVideo();
            for (let i = 0; i < pageStack.depth; ++i) {
                if (pageStack.get(i) === page) {
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
    // The height from the bottom of the window that the docked mini player covers, for the content that ends there and has a control in the corner
    readonly property real miniPlayerSpace: playbackMode === "mini" ? miniPlayer.height + miniPlayer.margin * 2 : 0

    // A playback that fails while the mini player shows brings the page back, with its error and its Close Player button
    readonly property bool failedInMini: playbackMode === "mini" && !!player && !player.active && !!player.errorText
    // A turn later: the expansion changes the mode this property is computed from
    onFailedInMiniChanged: if (failedInMini) Qt.callLater(expandPlayer)

    // A new play always opens the full player, from the mini player too
    function showPlayerPage() {
        if (!window.signedIn || !window.player)
            return false;
        if (playbackPageShowing)
            return true;
        finishTabMotion();
        const before = playbackMode;
        // The page of an earlier visit may still be waiting to be deleted: the video must not go into its slot
        playerPage = null;
        playbackMode = "full";
        const page = pageStack.push(Qt.resolvedUrl("PlayerPage.qml"), {
            player: window.player, connection: connection
        });
        if (!page) {
            playbackMode = before;
            return false;
        }
        playerPage = page;
        placeVideo();
        return true;
    }

    function expandPlayer() {
        if (playbackMode === "mini")
            showPlayerPage();
    }

    // Playback is over or was stopped: the video item goes (its player first, so the detach is at once), and the player page with it
    function endPlayback() {
        if (playbackMode === "none")
            return;
        const wasMini = playbackMode === "mini";
        if (videoLoader.item)
            videoLoader.item.player = null;
        playbackMode = "none";
        const live = connection.state === ConnectionController.Authenticated;
        if (playbackPageShowing && live)
            pageStack.pop();
        playerPage = null;
        // Home is stale once a playback that outlived the player page ends
        if (wasMini && live)
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
            if (playbackMode === "mini" && !showPlayerPage())
                break;
            togglePlayerFullscreen();
            break;
        }
    }

    // The player page in front gives way to the mini player, or ends the playback when there is nothing playing (an error shown, an item that ended); Home asks for its rows again once the page is gone, since positions change while something plays
    onPlaybackPageShowingChanged: {
        if (playbackPageShowing) {
            if (playbackMode === "mini")
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
        if (signedIn)
            connection.reloadHome();
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
    }

    function playItem(itemId, resumeTicks) {
        if (!window.signedIn || !window.player || !window.player.playItem(itemId, resumeTicks || 0))
            return false;
        if (!showPlayerPage()) { window.player.stop(); return false; }
        return true;
    }

    function playQueue(index) {
        if (!window.signedIn || !window.player || !window.player.playQueue(index))
            return false;
        if (!showPlayerPage()) { window.player.stop(); return false; }
        return true;
    }

    function resumeQueue() {
        if (!window.signedIn || !window.player || !window.player.resumeQueue())
            return false;
        if (!showPlayerPage()) { window.player.stop(); return false; }
        return true;
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

    // The stack position of the search page up to the one in front, -1 when it is not there
    function searchPageIndex() {
        for (let i = 0; i <= pageStack.currentIndex; ++i) {
            if (pageStack.get(i)?.objectName === "searchPage")
                return i;
        }
        return -1;
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
    function popToBase() {
        const base = pageStack.get(0);
        if (base)
            popTo(base);
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
                popTo(page);
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
                popTo(pageStack.get(index));
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
        popTo(fromPage);
        pageStack.push(Qt.resolvedUrl("GenreRailsPage.qml"), { connection: connection });
    }

    // The menu of a card (a right click, the Menu key or the button of the card): it opens at a point of the card, below it by default
    // `resumeTicks` is -1 for a card without a quick play, else its saved position (0: none)
    function openCardMenu(card, itemId, type, seriesId, x, y, resumeTicks, favorite) {
        if (connection.state !== ConnectionController.Authenticated)
            return;
        cardMenu.itemId = itemId;
        cardMenu.itemType = type;
        cardMenu.seriesId = seriesId;
        cardMenu.resumeTicks = resumeTicks ?? -1;
        cardMenu.favorite = favorite ?? false;
        cardMenu.popup(card, x ?? 0, y ?? card.height);
    }

    CardMenu {
        id: cardMenu

        connection: window.connection
        playAvailable: !!window.player
        // A playlist opens its own page, everything else its details
        onOpenRequested: targetId => cardMenu.itemType === "Playlist" ? window.openPlaylist(targetId, window.pageStack.currentItem) : window.openItem(targetId, window.pageStack.currentItem)
        onPlayRequested: targetId => window.playItem(targetId)
        onResumeRequested: (targetId, ticks) => window.playItem(targetId, ticks)
        // A refused send means a write for the item already runs: its answer updates the card
        onFavoriteRequested: (targetId, favorite) => window.connection.setFavorite(targetId, favorite)
    }

    // A link opens above the active root tab after sign-in. Incoming links must not bypass the
    // unsaved-preferences prompt. A cancelled request is consumed, so an identical
    // link delivered later can trigger a fresh prompt.
    function openRequestedItem(home) {
        if (requestedItemId.length === 0 || connection.state !== ConnectionController.Authenticated
                || (home?.objectName !== "homePage" && home?.objectName !== "libraryPage"))
            return;
        const itemId = requestedItemId;
        requestedItemId = "";
        const request = ++itemRequestGeneration;
        const navigate = () => {
            if (request !== itemRequestGeneration || connection.state !== ConnectionController.Authenticated)
                return;
            const base = pageStack.get(0);
            if (base?.objectName === "homePage" || base?.objectName === "libraryPage")
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

    // The section the page in front belongs to: the header marks it (a tab, the main menu button, the queue button); a page opened above a type tab belongs to that tab, and one opened from a collections tile to that library; the Settings page and the Favorites page (with what opens above it) belong to no tab, and the header search is global there. Settings needs no session, so it is marked whatever the connection is
    readonly property string currentSection: {
        if (pageStack.currentItem?.objectName === "settingsPage")
            return "settings";
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
        }
        // The Favorites page and what opens above it (a group, an item) belong to the star: no tab is marked; so do the Playlists page and what opens above it to the main menu
        if (favoritesPageIndex() >= 0)
            return "favorites";
        for (let i = 1; i <= pageStack.currentIndex; ++i) {
            if (pageStack.get(i)?.objectName === "playlistsPage")
                return "playlists";
        }
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

    // True while the Settings page or the Favorites page is in the stack, whatever page lies above it (the search page the header box opens included): the box stays on the global search then, or the page it opens would turn it back to the list of the tab at the root and pop everything
    readonly property bool globalSearchPage: {
        // The current item is read for the dependency: the stack changes with every push and pop
        if (!pageStack.currentItem)
            return false;
        for (let i = 1; i < pageStack.depth; ++i) {
            const name = pageStack.get(i)?.objectName;
            if (name === "settingsPage" || name === "favoritesPage")
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
        // Queued before the stack changes, so an error on the way cannot leave the row invisible
        if (leavingTab !== "" && prepareTabMotion())
            Qt.callLater(startTabMotionWhenReady, leavingTab, name);
        parkVideo();
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

    // The visible order of the tabs, which decides the side the new one slides in from
    function tabOrder(name) {
        return name === "home" ? 0 : name === "movies" ? 1 : 2;
    }

    // The new tab enters from the side it has in the tab bar; the row stays invisible from before the push until the motion starts, so the new page never shows one frame at rest
    property bool tabMotionQueued: false
    readonly property bool tabSwitching: tabMotionQueued || tabMotion.running
    ParallelAnimation {
        id: tabMotion
        OpacityAnimator { target: pageStack; from: 0; to: 1; duration: Kirigami.Units.longDuration; easing.type: Easing.InOutCubic }
        NumberAnimation { target: tabShift; property: "x"; to: 0; duration: Kirigami.Units.longDuration; easing.type: Easing.OutCubic }
    }
    // A running or waiting motion ends at once, with the row fully visible and in place; true when a new one is queued (no animation speed, no motion)
    function prepareTabMotion() {
        finishTabMotion();
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
        if (pageStack.get(0)?.contentReady ?? true) {
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
            if ((window.pageStack.get(0)?.contentReady ?? true) || Date.now() - since >= limit) {
                stop();
                window.startTabMotion(fromTab, toTab);
            }
        }
    }
    function startTabMotion(fromTab, toTab) {
        if (!tabMotionQueued)
            return;
        const fromRight = (tabOrder(toTab) > tabOrder(fromTab)) !== (Qt.application.layoutDirection === Qt.RightToLeft);
        tabShift.x = (fromRight ? 1 : -1) * Kirigami.Units.gridUnit * 2;
        tabMotion.restart();
        tabMotionQueued = false;
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

    // The main menu's Playlists and the Home tile of the server's Playlists view: the page opens above the first page, and one already in the stack is brought back
    function openPlaylists() {
        if (!signedIn)
            return;
        openSection("playlistsPage", "PlaylistsPage.qml", { connection: connection, shell: window });
    }

    // A playlist opens above the page asking (the pages above it give way); `name`, when the caller knows it, titles the page
    function openPlaylist(playlistId, fromPage, name) {
        if (connection.state !== ConnectionController.Authenticated)
            return;
        if (fromPage)
            popTo(fromPage);
        pageStack.push(Qt.resolvedUrl("PlaylistPage.qml"), { connection: connection, shell: window, playlistId: playlistId, name: name ?? "" });
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
        if (!signedIn || !window.player || playlistWaiting)
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

    // A favorite write started from a card menu that the server refused
    Connections {
        target: window.connection
        function onFavoriteUpdateFailed() {
            window.showPassiveNotification(i18n("Could not update Favorites. Check the connection and try again"));
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
                popTo(pageStack.get(i));
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
            shellHeader.showAboutDialog();
        } else if (startupPage === "settings") {
            pageStack.push(Qt.resolvedUrl("SettingsPage.qml"), { settings: settings, connection: connection });
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
