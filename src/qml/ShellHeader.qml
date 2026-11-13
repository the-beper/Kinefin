// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kirigamiaddons.components as Components

// The top bar: the main menu at the left, the Home, Movies and Shows tabs in the middle (a type tab only while the server has a library of it), and search, favorites, queue and the account menu at the right. The width decides the mode: Wide from 36 grid units (labelled tabs, wide search box), Medium from 24 (icon-only tabs, narrow box), Compact below 24 (search, favorites and queue fold into the main menu)
Item {
    id: root
    objectName: "shellHeader"

    // The shell window: its functions open the pages, its connection holds the session and the search
    required property var shell

    readonly property ConnectionController connection: shell.connection
    readonly property bool signedIn: connection.state === ConnectionController.Authenticated
    readonly property bool serverChosen: connection.state !== ConnectionController.Idle
    // The same thresholds the sidebar used, now for the bar. The shell derives them from the window
    // width it owns: a binding on the attached Window.window here kept its load-time value and never followed a resize
    readonly property bool wide: shell.wideHeader
    readonly property bool compact: shell.compactHeader

    // The signed-in user, found among the saved users by id: null when signed out or when the list has no such entry
    readonly property var currentUser: {
        const userId = connection.currentUserId;
        return signedIn && userId.length > 0 ? connection.users.find(user => user.id === userId) ?? null : null;
    }

    // A settings.value() call is not a reactive QML property: keep the menu
    // visibility and the automatic check in sync with edits in Settings.
    property bool updateChecksEnabled: shell.settings.value("main", "checkForUpdates")
    property bool manualUpdateCheck: false

    UpdateChecker {
        id: updateChecker
        objectName: "updateChecker"

        onUpdateAvailable: (version, releaseUrl) => {
            if (!root.updateChecksEnabled)
                return;
            updateDialog.version = version;
            updateDialog.releaseUrl = releaseUrl;
            if (!root.manualUpdateCheck) {
                updateNotificationTimer.restart();
                return;
            }
            // The user asked for this check: the result shows where they are, with no timer that could open a dialog under the pointer
            root.manualUpdateCheck = false;
            updateNotificationTimer.stop();
            if (aboutDialog.opened) {
                aboutDialog.updateVersion = version;
                aboutDialog.updateStatus = "available";
            } else {
                updateDialog.open();
            }
        }
        // The result of a manual check shows in the About dialog while it is open, else as a notification
        onUpToDate: {
            if (root.manualUpdateCheck) {
                aboutDialog.updateStatus = "upToDate";
                if (!aboutDialog.opened)
                    shell.showPassiveNotification(i18n("Kinefin is up to date"));
            }
            root.manualUpdateCheck = false;
        }
        onCheckFailed: {
            // An automatic check without network access must be silent
            if (root.manualUpdateCheck) {
                aboutDialog.updateStatus = "failed";
                if (!aboutDialog.opened)
                    shell.showPassiveNotification(i18n("Could not check for updates. Check the internet connection"));
            }
            root.manualUpdateCheck = false;
        }
    }

    Timer {
        id: automaticUpdateTimer
        interval: 3000
        repeat: false
        running: root.updateChecksEnabled && Qt.application.version.length > 0
        onTriggered: updateChecker.check()
    }

    // The delay of an update found by the automatic check before its dialog opens
    Timer {
        id: updateNotificationTimer
        interval: 3000
        repeat: false
        onTriggered: {
            if (!root.updateChecksEnabled)
                return;
            // Never two dialogs at once
            aboutDialog.close();
            updateDialog.open();
        }
    }

    Connections {
        target: root.shell.settings
        function onValueChanged(section, key) {
            if (section !== "main" || key !== "checkForUpdates")
                return;
            root.updateChecksEnabled = root.shell.settings.value("main", "checkForUpdates");
            if (!root.updateChecksEnabled) {
                root.manualUpdateCheck = false;
                aboutDialog.updateStatus = "";
                updateChecker.cancel();
                updateNotificationTimer.stop();
                updateDialog.close();
            }
        }
    }

    // The Home page on a wide window shows Refresh in the bar, anywhere else it is a menu entry
    readonly property bool refreshFitsBar: signedIn && shell.width >= Kirigami.Units.gridUnit * 48
    readonly property bool refreshInBar: refreshFitsBar && shell.pageStack.currentItem?.objectName === "homePage"

    implicitHeight: bar.implicitHeight

    // Ctrl+F focuses the box and selects what is in it
    function focusSearchField() {
        searchField.forceActiveFocus(Qt.TabFocusReason);
        searchField.selectAll();
    }

    // The keyboard comes back to the box after the page its typing opened was created: unlike Ctrl+F it must not select, or the keystroke that opened the page would be replaced
    function refocusSearchField() {
        searchField.forceActiveFocus(Qt.TabFocusReason);
    }

    // Move between the root tabs in their visible order (a library type may
    // be absent). Detail pages belong to their root tab; using the shortcut
    // returns to the selected tab, just like clicking its header button.
    function switchRelativeTab(step) {
        if (!signedIn)
            return;
        const tabs = [homeAction];
        if (moviesAction.visible)
            tabs.push(moviesAction);
        if (tvShowsAction.visible)
            tabs.push(tvShowsAction);
        if (tabs.length < 2)
            return;
        const current = shell.rootTab === "movies" && moviesAction.visible
            ? tabs.indexOf(moviesAction)
            : (shell.rootTab === "tvshows" && tvShowsAction.visible ? tabs.indexOf(tvShowsAction) : 0);
        tabs[(current + step + tabs.length) % tabs.length].trigger();
    }

    // The list the box searches: the model of the type tab at the root of the stack; on any other page the box feeds the global search
    readonly property var localModel: {
        // The Settings and Favorites pages opened over a tab are no part of it: the box searches the whole server there
        if (shell.globalSearchPage)
            return null;
        switch (shell.rootTab) {
        case "movies":
            return connection.movies;
        case "tvshows":
            return connection.shows;
        }
        return null;
    }

    // The term the box shows now: the one of the list it searches
    function currentTerm() {
        return localModel ? localModel.searchTerm : connection.search.term;
    }

    // Runs `action` at once, or once the user settled the unsaved preferences draft that the page above would lose
    function afterUnsavedSettled(action) {
        if (!shell.askBeforeLeaving(action))
            action();
    }

    // The box holds the keyboard while the term is being typed: the shell does not move the focus while a keystroke of its own opens the search page
    readonly property bool searchFieldHasFocus: searchField.activeFocus

    // First in the file: siblings take the Tab order by declaration, so the menu button precedes the tab and the right cluster
    QQC2.ToolButton {
        id: headerBackButton
        objectName: "headerBackButton"
        visible: shell.pageStack.currentItem?.objectName === "settingsPage"
        z: 2
        anchors.left: parent.left
        anchors.leftMargin: Kirigami.Units.smallSpacing
        anchors.verticalCenter: parent.verticalCenter
        text: i18nc("@action:button", "Back")
        icon.name: "go-previous"
        display: QQC2.AbstractButton.IconOnly
        QQC2.ToolTip.text: text
        QQC2.ToolTip.visible: hovered
        QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
        onClicked: shell.pageStack.goBack()
    }

    QQC2.ToolButton {
        id: menuButton
        objectName: "mainMenuButton"

        // The tab bar fills the header and is declared after this button: without a z above it the bar covers the button, which neither shows nor takes the clicks (the Tab order follows the declaration, not z)
        z: 1
        anchors {
            left: headerBackButton.visible ? headerBackButton.right : parent.left
            leftMargin: Kirigami.Units.smallSpacing
            verticalCenter: parent.verticalCenter
        }
        text: i18nc("@action:button", "Main Menu")
        icon.name: "open-menu-symbolic"
        // Only the icon; the text is for screen readers
        display: QQC2.AbstractButton.IconOnly
        QQC2.ToolTip.text: text
        QQC2.ToolTip.visible: hovered
        QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
        // The menu marks where its entries lead while one of them shows
        down: shell.currentSection === "settings" || shell.currentSection === "about"
        onClicked: mainMenu.popup(menuButton, 0, menuButton.height)
    }

    // The bar spans the whole header, so the room for the buttons at its sides comes from its padding
    Kirigami.NavigationTabBar {
        id: bar

        anchors.fill: parent
        leftPadding: menuButton.width + (headerBackButton.visible ? headerBackButton.width : 0)
                     + Kirigami.Units.smallSpacing * 3
        // The room of the Refresh button stays reserved on the other pages of a wide window: the tabs keep their width while it comes and goes
        rightPadding: rightCluster.width + (root.refreshFitsBar && !root.refreshInBar ? refreshButton.implicitWidth + rightCluster.spacing : 0)
                      + Kirigami.Units.smallSpacing * 2

        actions: [homeAction, moviesAction, tvShowsAction]

        // The bar's own tab shortcuts neither wrap nor follow a page above the tabs, and two shortcuts with one sequence are ambiguous so neither fires: the shell's take over all of their sequences
        Component.onCompleted: {
            for (const child of bar.contentData) {
                if (child instanceof Shortcut)
                    child.enabled = false;
            }
        }

        delegate: Kirigami.NavigationTabButton {
            id: tab

            required property Kirigami.Action modelData

            parent: bar.contentItem
            action: modelData
            // The same workaround as the stock delegate: without it the tabs are not checkable
            onActionChanged: action.checkable = true
            // The label rides along only on a wide window: the tab is icon-only in the other modes
            display: root.wide ? QQC2.AbstractButton.TextBesideIcon : QQC2.AbstractButton.IconOnly

            Layout.minimumWidth: bar.buttonWidth
            Layout.maximumWidth: bar.buttonWidth
            Layout.fillHeight: true

            contentItem: RowLayout {
                spacing: Kirigami.Units.largeSpacing

                Item {
                    Layout.fillWidth: true
                }
                Kirigami.Icon {
                    source: tab.icon.name
                    color: tab.icon.color
                    Layout.preferredWidth: Kirigami.Units.iconSizes.smallMedium
                    Layout.preferredHeight: Kirigami.Units.iconSizes.smallMedium
                }
                QQC2.Label {
                    visible: root.wide
                    text: tab.text
                    color: Kirigami.Theme.textColor
                }
                Item {
                    Layout.fillWidth: true
                }
            }
        }
    }

    // Kirigami's tab bar switches the tab with the mouse wheel: this item sits above it and takes the wheel first, so a stray scroll over the header opens no other tab
    Item {
        anchors.fill: bar

        WheelHandler {
            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        }
    }

    RowLayout {
        id: rightCluster

        anchors {
            right: parent.right
            rightMargin: Kirigami.Units.smallSpacing
            verticalCenter: parent.verticalCenter
        }
        spacing: Kirigami.Units.smallSpacing

        // The search box: on a type tab it searches that tab's list and the first keystroke pops back to the tab; elsewhere it owns the global term and the first keystroke opens the search page above the current one
        Kirigami.SearchField {
            id: searchField
            objectName: "headerSearchField"

            Layout.preferredWidth: Kirigami.Units.gridUnit * (root.wide ? 16 : 11)
            Layout.alignment: Qt.AlignVCenter
            visible: !root.compact
            enabled: root.signedIn
            // The shell owns Ctrl+F: the field's own sequence would make it ambiguous
            focusSequences: []
            maximumLength: 128
            autoAccept: false
            placeholderText: {
                switch (root.localModel ? root.shell.rootTab : "") {
                case "movies":
                    return i18nc("@info:placeholder", "Search in Movies");
                case "tvshows":
                    return i18nc("@info:placeholder", "Search in Shows");
                }
                return i18nc("@info:placeholder", "Search");
            }

            // The box is the source of truth of the term: its own text never echoes back
            onTextChanged: {
                if (text === root.currentTerm())
                    return;
                if (root.localModel) {
                    root.localModel.searchTerm = text;
                    root.shell.popToBaseWhileTyping();
                } else {
                    root.connection.search.term = text;
                    root.shell.openSearchIfMissing();
                }
            }
            onAccepted: {
                if (!root.localModel)
                    root.connection.search.searchNow();
            }
            Keys.onDownPressed: root.shell.searchResultsDown()
            Component.onCompleted: text = root.currentTerm()
        }

        QQC2.ToolButton {
            id: refreshButton
            objectName: "headerRefreshButton"
            Layout.alignment: Qt.AlignVCenter
            visible: root.refreshInBar
            text: refreshAction.text
            icon.name: refreshAction.icon.name
            display: QQC2.AbstractButton.IconOnly
            QQC2.ToolTip.text: text
            QQC2.ToolTip.visible: hovered
            QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
            onClicked: refreshAction.trigger()
        }

        QQC2.ToolButton {
            objectName: "favoritesButton"

            Layout.alignment: Qt.AlignVCenter
            visible: !root.compact
            text: i18nc("@action:button", "Favorites")
            icon.name: "favorite"
            display: QQC2.AbstractButton.IconOnly
            QQC2.ToolTip.text: text
            QQC2.ToolTip.visible: hovered
            QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
            enabled: root.signedIn
            down: shell.currentSection === "favorites"
            onClicked: shell.openFavorites()
        }

        QQC2.ToolButton {
            objectName: "queueButton"

            Layout.alignment: Qt.AlignVCenter
            visible: !root.compact
            text: i18nc("@action:button", "Queue")
            icon.name: "view-media-playlist"
            display: QQC2.AbstractButton.IconOnly
            QQC2.ToolTip.text: text
            QQC2.ToolTip.visible: hovered
            QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
            enabled: root.signedIn
            down: shell.currentSection === "queue"
            onClicked: shell.openSection("queuePage", "QueuePage.qml", { connection: shell.connection })
        }

        QQC2.ToolButton {
            id: accountButton
            objectName: "accountButton"

            Layout.alignment: Qt.AlignVCenter
            // The desktop style draws the icon and sizes the button in its own background: with this one the button would be empty and 0 x 0, so the icon and the size are given here
            contentItem: Item {
                implicitWidth: Kirigami.Units.iconSizes.medium
                implicitHeight: Kirigami.Units.iconSizes.medium
                Kirigami.Icon {
                    anchors.centerIn: parent
                    width: Kirigami.Units.iconSizes.smallMedium
                    height: width
                    source: accountButton.icon.name
                    visible: !accountButton.hasAvatar
                }
            }
            // The same hit area as the other header buttons, with the ring and the avatar centred in it
            implicitWidth: menuButton.implicitHeight
            implicitHeight: menuButton.implicitHeight
            padding: 0
            // The ring thickens while the keyboard focus is on the button
            background: Rectangle {
                radius: width / 2
                color: Qt.alpha(Kirigami.Theme.highlightColor, accountButton.down ? 0.30 : 0.14)
                border.width: accountButton.visualFocus ? 3 : 2
                border.color: Kirigami.Theme.highlightColor
                Behavior on color { ColorAnimation { duration: Kirigami.Units.shortDuration } }
            }
            // The avatar of the user replaces the icon; the name is for screen readers only
            readonly property bool hasAvatar: root.currentUser !== null && root.currentUser.imageUrl.toString().length > 0
            // The text stays static: it feeds the tooltip, which must never carry the remote user name
            readonly property string accessibleName: root.currentUser ? i18nc("@label %1 is a user name", "Account: %1", root.currentUser.name) : text
            text: i18nc("@action:button", "Account")
            Accessible.name: accessibleName
            icon.name: hasAvatar ? "" : "im-user"
            display: QQC2.AbstractButton.IconOnly
            visible: root.signedIn || root.serverChosen
            QQC2.ToolTip.text: text
            QQC2.ToolTip.visible: hovered
            QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
            onClicked: accountMenu.popup(accountButton, 0, accountButton.height)

            Components.Avatar {
                objectName: "accountAvatar"
                anchors.centerIn: parent
                width: Kirigami.Units.iconSizes.medium
                height: width
                visible: accountButton.hasAvatar
                Accessible.ignored: true
                name: root.currentUser ? root.currentUser.name : ""
                // Only URLs built by JellyfinClient::userImageUrl belong here: a raw server string must never become a source
                source: root.currentUser ? root.currentUser.imageUrl : ""
                // The image is decoded at the size it shows
                sourceSize.width: width * Screen.devicePixelRatio
                sourceSize.height: height * Screen.devicePixelRatio
            }
        }
    }

    // The term reached the box from outside (a field of the search page, a term the page took with it when it left): the box follows it, and never while it echoes its own edit
    Connections {
        target: root.connection.search
        function onTermChanged() {
            if (!root.localModel && searchField.text !== root.connection.search.term)
                searchField.text = root.connection.search.term;
        }
    }

    // The same for the list a type tab searches, and the box takes the term of the other list when the tab changes
    Connections {
        target: root.localModel
        function onSearchTermChanged() {
            if (searchField.text !== root.localModel.searchTerm)
                searchField.text = root.localModel.searchTerm;
        }
    }
    onLocalModelChanged: searchField.text = currentTerm()

    // The tab group of Kirigami unchecks an action when its buttons are rebuilt (a tab appears or goes), which would break a binding on `checked`: the marks are written here instead, on every change of the section and whenever one is cleared
    function syncTabs() {
        homeAction.checked = shell.currentSection === "home";
        moviesAction.checked = shell.currentSection === "movies";
        tvShowsAction.checked = shell.currentSection === "tvshows";
    }

    Connections {
        target: shell
        function onCurrentSectionChanged() {
            root.syncTabs();
        }
    }
    Component.onCompleted: syncTabs()

    Kirigami.Action {
        id: homeAction
        objectName: "homeTabAction"

        text: i18nc("@title:tab", "Home")
        tooltip: root.wide ? "" : text
        icon.name: "go-home"
        enabled: root.signedIn
        onCheckedChanged: Qt.callLater(root.syncTabs)
        shortcut: "Ctrl+1"
        onTriggered: shell.goHome()
    }

    // Ctrl+2 and Ctrl+3 follow the position of the visible tabs: a missing Movies tab moves Shows to Ctrl+2
    Kirigami.Action {
        id: moviesAction
        objectName: "moviesTabAction"

        text: i18nc("@title:tab", "Movies")
        tooltip: root.wide ? "" : text
        icon.name: "video-x-generic"
        visible: root.connection.movieLibraries.length > 0
        enabled: root.signedIn
        onCheckedChanged: Qt.callLater(root.syncTabs)
        shortcut: visible ? "Ctrl+2" : ""
        onTriggered: shell.openTab("movies")
    }
    Kirigami.Action {
        id: tvShowsAction
        objectName: "tvShowsTabAction"

        text: i18nc("@title:tab", "Shows")
        tooltip: root.wide ? "" : text
        icon.name: "video-television"
        visible: root.connection.showLibraries.length > 0
        enabled: root.signedIn
        onCheckedChanged: Qt.callLater(root.syncTabs)
        shortcut: visible ? (moviesAction.visible ? "Ctrl+3" : "Ctrl+2") : ""
        onTriggered: shell.openTab("tvshows")
    }

    // PageUp/PageDown are the KDE tab navigation conventions, the other sequences are the ones of Kirigami's tab bar. They work even when a detail page is on top; absent Movies/TV types are skipped
    Shortcut {
        objectName: "previousTabShortcut"
        sequences: ["Ctrl+PgUp", "Ctrl+[", "Ctrl+Shift+Tab"]
        context: Qt.WindowShortcut
        enabled: root.signedIn
        onActivated: root.switchRelativeTab(-1)
    }
    Shortcut {
        objectName: "nextTabShortcut"
        sequences: ["Ctrl+PgDown", "Ctrl+]", "Ctrl+Tab"]
        context: Qt.WindowShortcut
        enabled: root.signedIn
        onActivated: root.switchRelativeTab(1)
    }

    // The shortcuts live on the actions, not on the menu items, so they work with the menu closed
    Kirigami.Action {
        id: settingsAction
        objectName: "settingsAction"

        text: i18nc("@action:inmenu", "Settings")
        icon.name: "configure"
        shortcut: "Ctrl+,"
        onTriggered: shell.openSection("settingsPage", "SettingsPage.qml", { settings: shell.settings, connection: shell.connection })
    }
    Kirigami.Action {
        id: shortcutsAction
        objectName: "shortcutsAction"

        text: i18nc("@action:inmenu", "Keyboard Shortcuts")
        icon.name: "input-keyboard-symbolic"
        shortcut: "Ctrl+Shift+/"
        onTriggered: shortcutsDialog.open()
    }
    Kirigami.Action {
        id: aboutAction
        objectName: "aboutAction"

        text: i18nc("@action:inmenu", "About Kinefin")
        icon.name: "help-about"
        onTriggered: aboutDialog.open()
    }
    // No shortcut: F5 belongs to the page actions, and a second one makes both ambiguous
    Kirigami.Action {
        id: refreshAction
        objectName: "refreshAction"

        text: i18nc("@action:inmenu", "Refresh")
        icon.name: "view-refresh"
        visible: root.signedIn
        // Only a page that has something to refresh takes it: Home reloads the libraries, the others do it themselves
        enabled: typeof shell.pageStack.currentItem?.refreshPage === "function" || shell.pageStack.currentItem?.objectName === "homePage"
        onTriggered: {
            const page = shell.pageStack.currentItem;
            if (typeof page?.refreshPage === "function")
                page.refreshPage();
            else if (page?.objectName === "homePage")
                root.connection.reloadLibraries();
        }
    }
    Kirigami.Action {
        id: adminDashboardAction
        objectName: "adminDashboardAction"

        text: i18nc("@action:inmenu", "Admin Dashboard")
        icon.name: "preferences-system-network"
        visible: root.signedIn && root.connection.currentUserIsAdministrator
        onTriggered: {
            if (!root.connection.openAdminDashboard())
                shell.showPassiveNotification(i18n("The admin dashboard could not be opened: check the default browser"));
        }
    }

    // The right cluster of a Compact window, as menu entries: the bar itself has no room for them.
    // A MenuItem does not mirror the visibility of its action: each entry hides on its own
    Kirigami.Action {
        id: compactSearchAction
        objectName: "compactSearchAction"

        text: i18nc("@action:inmenu", "Search")
        icon.name: "system-search"
        visible: root.compact
        enabled: root.signedIn
        onTriggered: shell.openSearch()
    }
    Kirigami.Action {
        id: compactFavoritesAction
        objectName: "compactFavoritesAction"

        text: i18nc("@action:inmenu", "Favorites")
        icon.name: "favorite"
        visible: root.compact
        enabled: root.signedIn
        onTriggered: shell.openFavorites()
    }
    Kirigami.Action {
        id: compactQueueAction
        objectName: "compactQueueAction"

        text: i18nc("@action:inmenu", "Queue")
        icon.name: "view-media-playlist"
        visible: root.compact
        enabled: root.signedIn
        onTriggered: shell.openSection("queuePage", "QueuePage.qml", { connection: shell.connection })
    }

    Kirigami.Action {
        id: remoteSessionsAction
        objectName: "remoteSessionsAction"

        text: i18nc("@action:inmenu", "Remote Sessions")
        icon.name: "input-tvremote"
        visible: root.signedIn
        onTriggered: remoteSessionsDialog.open()
    }

    // The account menu: what a session can lose, and what a chosen server can still be left for
    Kirigami.Action {
        id: switchUserAction
        objectName: "switchUserAction"

        text: i18nc("@action:inmenu Opens the list of the saved users of this server", "Switch User")
        icon.name: "system-switch-user"
        visible: root.signedIn
        onTriggered: root.afterUnsavedSettled(() => root.connection.switchUser())
    }
    Kirigami.Action {
        id: signOutAction
        objectName: "signOutAction"

        text: i18nc("@action:inmenu", "Sign Out")
        tooltip: i18nc("@info:tooltip", "Remove this user's saved session from this system")
        icon.name: "system-log-out"
        visible: root.signedIn
        // Signing out removes the saved session and queue of the user, so it asks first (about unsaved preferences, then about the sign-out)
        onTriggered: root.afterUnsavedSettled(() => signOutDialog.openFor(root.currentUser ? root.currentUser.name : ""))
    }
    Kirigami.Action {
        id: switchServerAction
        objectName: "switchServerAction"

        text: i18nc("@action:inmenu Goes back to the list of servers", "Switch Server")
        icon.name: "network-server"
        visible: root.serverChosen
        onTriggered: root.afterUnsavedSettled(() => root.connection.cancel())
    }

    QQC2.Menu {
        id: mainMenu
        objectName: "mainMenu"
        // The widest shown entry (its label plus the shortcut column, which the style's MenuItem counts in its implicit width) and the menu's paddings, never wider than the window
        readonly property real widestEntry: {
            let widest = 0;
            for (let i = 0; i < count; ++i) {
                const entry = itemAt(i);
                if (entry?.visible)
                    widest = Math.max(widest, entry.implicitWidth);
            }
            return widest;
        }
        implicitWidth: Math.min(Math.max(implicitBackgroundWidth + leftInset + rightInset, widestEntry + leftPadding + rightPadding),
                                shell.width - Kirigami.Units.largeSpacing * 2)

        QQC2.MenuItem {
            objectName: "menuRefreshItem"
            visible: root.signedIn && !root.refreshInBar
            action: refreshAction
        }

        // In Compact mode the right cluster hides and its three actions fold in here, in the order of the bar
        QQC2.MenuItem {
            objectName: "menuSearchItem"
            visible: root.compact
            action: compactSearchAction
        }
        QQC2.MenuItem {
            objectName: "menuFavoritesItem"
            visible: root.compact
            action: compactFavoritesAction
        }
        QQC2.MenuItem {
            objectName: "menuQueueItem"
            visible: root.compact
            action: compactQueueAction
        }
        QQC2.MenuSeparator {
            visible: root.compact || (root.signedIn && !root.refreshInBar)
        }

        QQC2.MenuItem {
            objectName: "menuRemoteItem"
            visible: root.signedIn
            action: remoteSessionsAction
        }
        QQC2.MenuItem {
            objectName: "menuAdminItem"
            visible: adminDashboardAction.visible
            action: adminDashboardAction
        }
        QQC2.MenuSeparator {
            visible: root.signedIn
        }

        QQC2.MenuItem {
            objectName: "menuSettingsItem"
            action: settingsAction
        }
        QQC2.MenuItem {
            objectName: "menuShortcutsItem"
            action: shortcutsAction
        }
        QQC2.MenuItem {
            objectName: "menuAboutItem"
            action: aboutAction
        }
    }

    QQC2.Menu {
        id: accountMenu
        objectName: "accountMenu"

        QQC2.MenuItem {
            objectName: "switchUserItem"
            visible: root.signedIn
            action: switchUserAction
        }
        QQC2.MenuItem {
            objectName: "signOutItem"
            visible: root.signedIn
            action: signOutAction
            Accessible.description: signOutAction.tooltip
            QQC2.ToolTip.text: signOutAction.tooltip
            QQC2.ToolTip.visible: hovered
            QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
        }
        QQC2.MenuItem {
            objectName: "switchServerItem"
            visible: root.serverChosen
            action: switchServerAction
        }
    }

    Kirigami.PromptDialog {
        id: updateDialog
        objectName: "updateAvailableDialog"

        property string version: ""
        property url releaseUrl: ""

        title: i18nc("@title:window", "Update Available")
        standardButtons: Kirigami.Dialog.Cancel
        customFooterActions: [
            Kirigami.Action {
                text: i18nc("@action:button", "Open Release Page")
                icon.name: "internet-web-browser"
                onTriggered: {
                    updateDialog.close();
                    if (!Qt.openUrlExternally(updateDialog.releaseUrl))
                        shell.showPassiveNotification(i18n("Could not open the release page in the browser"));
                }
            }
        ]

        QQC2.Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
            text: i18n("Kinefin %1 is available. Open its release page in your browser?", updateDialog.version)
        }
    }

    Kirigami.PromptDialog {
        id: signOutDialog
        objectName: "signOutDialog"

        property string userName

        function openFor(name) {
            userName = name;
            open();
        }

        title: i18nc("@title:window", "Sign Out")
        preferredWidth: Kirigami.Units.gridUnit * 26
        maximumWidth: Math.min(Kirigami.Units.gridUnit * 28, shell.width - Kirigami.Units.largeSpacing * 2)
        standardButtons: Kirigami.Dialog.Cancel
        customFooterActions: [
            Kirigami.Action {
                text: i18nc("@action:button", "Sign Out")
                icon.name: "system-log-out"
                onTriggered: {
                    signOutDialog.close();
                    root.connection.signOut();
                }
            }
        ]

        // The subtitle would render the user name as rich text: the name goes into a plain-text label
        QQC2.Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
            text: signOutDialog.userName.length > 0
                ? i18nc("@info %1 is a user name", "Sign out %1? Saved credentials and the local queue will be removed. The server account stays unchanged.", signOutDialog.userName)
                : i18nc("@info", "Sign out? Saved credentials and the local queue will be removed. The server account stays unchanged.")
        }
    }

    AboutDialog {
        id: aboutDialog
        objectName: "aboutDialog"

        updateChecksEnabled: root.updateChecksEnabled
        checking: updateChecker.checking
        onCheckForUpdatesRequested: {
            root.manualUpdateCheck = true;
            updateStatus = "checking";
            updateChecker.check();
        }
        onUpdateDetailsRequested: {
            close();
            updateDialog.open();
        }
    }

    RemoteSessionsDialog {
        id: remoteSessionsDialog
        objectName: "remoteSessionsDialog"

        connection: root.connection
        maximumWidth: Math.min(Kirigami.Units.gridUnit * 34, root.shell.width - Kirigami.Units.largeSpacing * 2)
    }

    // Opening while the startup layout runs leaves a y binding loop in Kirigami.Dialog
    function showAboutDialog() { Qt.callLater(aboutDialog.open); }

    ShortcutsDialog {
        id: shortcutsDialog
        objectName: "shortcutsDialog"
    }
}
