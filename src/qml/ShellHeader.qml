// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// The bar of an offline window with only Downloads to choose from: it has the one menu button. A signed-out window has no bar (the menu button stands alone in the top corner), and neither has a window with sections to choose from: the sidebar stands at its edge, or in a Compact window (narrower than `compactLimit`) the drawer opens from the button that floats in the top corner. The bar is hidden there: its menus, its dialogs and its actions stay here, the main menu hangs from the Menu entry of the sidebar, or in a Compact window from the floating button, and the account lives on the users page the sidebar's avatar opens
Item {
    id: root
    objectName: "shellHeader"

    // The shell window: its functions open the pages, its connection holds the session and the search
    required property var shell
    // The sections the sidebar draws: their actions and shortcuts live there
    required property ShellNavigation navigation

    readonly property ConnectionController connection: shell.connection
    // The window's rule: false on the user picker of a session, whose menu keeps the entries that need no session (Settings, Keyboard Shortcuts, About)
    readonly property bool signedIn: shell.signedIn
    readonly property bool offline: connection.state === ConnectionController.Offline
    // Watching together; null when the application gives none
    readonly property var syncPlay: shell.syncPlay ?? null
    // The sidebar shows: the bar is hidden, and the main menu hangs from the sidebar's Menu entry
    readonly property bool inSidebar: shell.sidebarWidth > 0
    readonly property bool groupMenuOpened: groupMenu.opened
    readonly property bool mainMenuOpened: mainMenu.opened
    readonly property bool queueOpened: queueDialog.opened
    // Whether SyncPlay is offered: the sidebar's entry follows it
    readonly property bool syncPlayOffered: syncPlayAction.visible
    // The drawer's Main Menu row while the menu hangs from it, with the drawer open under it, what runs a choice then (the drawer's own `closeThen`) and the choice made: it runs once the menu and then the drawer have gone, since either one, closing over a dialog the choice opened, would take the keyboard back from it
    property Item drawerMenuRow: null
    property var drawerMenuRunner: null
    property var drawerMenuChoice: null
    // What the main menu hangs from: the drawer's row, the sidebar's Menu entry, the floating navigation button of a Compact window, else the bar's button
    readonly property Item menuOrigin: drawerMenuRow ?? (shell.cornerNavigation ? shell.cornerNavigationButton : inSidebar ? (shell.sidebarMenuButton ?? mainMenuButton) : mainMenuButton)

    // The signed-in user, found among the saved users by id: null when signed out or when the list has no such entry
    readonly property var currentUser: {
        const userId = connection.currentUserId;
        return signedIn && userId.length > 0 ? connection.users.find(user => user.id === userId) ?? null : null;
    }

    // The height and the margins of the bars of the pages
    implicitHeight: Math.max(rightCluster.implicitHeight + Kirigami.Units.smallSpacing * 2, Kirigami.Units.gridUnit * 2.5)

    // A gamepad's Start opens the main menu where its button stands
    function openMainMenu() {
        mainMenu.open();
    }

    // The main menu beside the drawer's row; `runner` takes each choice
    function openMainMenuFromDrawer(row, runner) {
        drawerMenuRow = row;
        drawerMenuRunner = runner;
        mainMenu.open();
    }

    // The drawer goes for another reason (the window grew past the Compact limit, the session ended): the menu hanging from its row goes too
    function dismissDrawerMenu() {
        if (drawerMenuRow)
            mainMenu.close();
    }

    // A choice of the main menu runs at once, or once the menu opened from the drawer has closed; a shortcut runs at once, as no menu is open then
    function runChoice(choice) {
        if (drawerMenuRunner)
            drawerMenuChoice = choice;
        else
            choice();
    }

    // The menu of the SyncPlay group opens beside the button of the sidebar that asks
    function openGroupMenu(button) {
        groupPlacement.openFor(button);
    }

    function openRemoteSessions() {
        remoteSessionsAction.trigger();
    }

    // The rail's Queue entry and the drawer's row open it
    function openQueue() {
        queueDialog.open();
    }

    // Only a narrow Settings page with an entry open has it: there the entry hides the list, and this is the pointer's way back to it (with the sidebar or the drawer the page has its own)
    QQC2.ToolButton {
        id: headerBackButton
        objectName: "headerBackButton"
        flat: false
        visible: shell.pageStack.currentItem?.objectName === "settingsPage" && shell.pageStack.currentItem.narrow && shell.pageStack.currentItem.entryOpen
        z: 2
        anchors.left: parent.left
        anchors.leftMargin: Kirigami.Units.largeSpacing
        anchors.verticalCenter: parent.verticalCenter
        text: i18nc("@action:button", "Back")
        icon.name: Qt.application.layoutDirection === Qt.RightToLeft ? "go-next" : "go-previous"
        display: QQC2.AbstractButton.IconOnly
        QQC2.ToolTip.text: text
        QQC2.ToolTip.visible: hovered || visualFocus
        QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
        onClicked: shell.pageStack.goBack()
    }

    RowLayout {
        id: rightCluster

        anchors {
            right: parent.right
            rightMargin: Kirigami.Units.largeSpacing
            verticalCenter: parent.verticalCenter
        }
        spacing: Kirigami.Units.smallSpacing

        // The one menu of the bar and of the signed-out corner; the sidebar has its own Menu entry, and the drawer a row
        QQC2.ToolButton {
            id: mainMenuButton
            objectName: "mainMenuButton"

            flat: false
            parent: root.shell.cornerMenu ? (root.shell.cornerMenuSlot ?? rightCluster) : rightCluster
            Layout.alignment: Qt.AlignHCenter | Qt.AlignVCenter
            Layout.minimumHeight: implicitHeight
            implicitHeight: PageMetrics.cornerButton
            implicitWidth: implicitHeight
            text: i18nc("@action:button", "Main Menu")
            // Not the hamburger: in a Compact window that is the floating button that opens the sections
            icon.name: "view-more-symbolic"
            Accessible.name: text
            Accessible.role: Accessible.ButtonMenu
            display: QQC2.AbstractButton.IconOnly
            QQC2.ToolTip.text: text
            QQC2.ToolTip.visible: (hovered || visualFocus) && !mainMenu.opened
            QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
            // The menu marks where its entries lead while one of them shows
            down: shell.currentSection === "settings"
            onClicked: mainMenu.open()
        }
    }

    // The shortcuts live on the actions, not on the menu items, so they work with the menu closed
    Kirigami.Action {
        id: settingsAction
        objectName: "settingsAction"

        text: i18nc("@action:inmenu", "Settings")
        icon.name: "configure"
        shortcut: "Ctrl+,"
        onTriggered: root.runChoice(() => shell.openSection("settingsPage", "SettingsPage.qml", { settings: shell.settings, connection: shell.connection }))
    }
    Kirigami.Action {
        id: shortcutsAction
        objectName: "shortcutsAction"

        text: i18nc("@action:inmenu", "Keyboard Shortcuts")
        icon.name: "input-keyboard-symbolic"
        shortcut: "Ctrl+Shift+/"
        onTriggered: root.runChoice(() => shortcutsDialog.open())
    }
    Kirigami.Action {
        id: aboutAction
        objectName: "aboutAction"

        text: i18nc("@action:inmenu", "About Kinefin")
        icon.name: "help-about"
        onTriggered: root.runChoice(() => aboutDialog.open())
    }
    // The one Refresh of the app, on Ctrl+R (never a function key alone, per the HIG): no page binds it itself, or the two shortcuts would be ambiguous. One sequence, not StandardKey.Refresh: the style's MenuItem binds a single sequence and warns on a standard key that has several
    Kirigami.Action {
        id: refreshAction
        objectName: "refreshAction"

        text: i18nc("@action:inmenu", "Refresh")
        icon.name: "view-refresh"
        shortcut: "Ctrl+R"
        visible: root.signedIn
        // Only a page that has something to refresh takes it: Home reloads the libraries, the others do it themselves
        enabled: typeof shell.pageStack.currentItem?.refreshPage === "function" || shell.pageStack.currentItem?.objectName === "homePage"
        onTriggered: root.runChoice(() => {
            const page = shell.pageStack.currentItem;
            if (typeof page?.refreshPage === "function")
                page.refreshPage();
            else if (page?.objectName === "homePage")
                root.connection.reloadLibraries();
        })
    }
    Kirigami.Action {
        id: adminDashboardAction
        objectName: "adminDashboardAction"

        text: i18nc("@action:inmenu", "Admin Dashboard")
        icon.name: "preferences-system-network"
        visible: root.signedIn && root.connection.currentUserIsAdministrator
        onTriggered: root.runChoice(() => {
            if (!root.connection.openAdminDashboard())
                shell.showPassiveNotification(i18n("The admin dashboard could not be opened: check the default browser"));
        })
    }

    // The sidebar's Remote entry and the drawer's row trigger this one; the SyncPlay entries follow the visibility of the next and open its dialog through openSyncPlay()
    Kirigami.Action {
        id: remoteSessionsAction
        objectName: "remoteSessionsAction"

        text: i18nc("@action:inmenu", "Remote Sessions")
        icon.name: "input-tvremote"
        visible: root.signedIn
        onTriggered: remoteSessionsDialog.open()
    }

    Kirigami.Action {
        id: syncPlayAction
        objectName: "syncPlayAction"

        text: i18nc("@action:inmenu Watching together with other users, the server's feature name", "SyncPlay")
        icon.name: "system-users"
        visible: root.signedIn && !!root.syncPlay && (root.syncPlay.available || root.syncPlay.status !== SyncPlayController.Idle)
        onTriggered: syncPlayDialog.open()
    }

    // An offline window has no users page: leaving the downloads is an entry of the menu
    Kirigami.Action {
        id: leaveOfflineAction
        objectName: "leaveOfflineAction"

        text: i18nc("@action:inmenu Leaves the offline downloads and shows the servers", "Leave Offline")
        icon.name: "network-disconnect"
        visible: root.offline
        onTriggered: root.connection.cancel()
    }

    // Under the button, right-aligned with it (left-aligned when mirrored) as the bar ends there, and above it in a window too short to hold the menu below; beside the sidebar's Menu entry or the drawer's row, past the margin and the line of the rail or the drawer and a margin more, so that its frame does not double the line, as high as the window allows. A Compact window has no bar: Start opens the menu under the floating navigation button
    PopupPlacement { popup: mainMenu; anchor: root.menuOrigin; beside: root.inSidebar || root.drawerMenuRow !== null; besideGap: Kirigami.Units.smallSpacing * 2 + 1 }

    // The quick actions of the group the viewer is in: the menu of the sidebar's SyncPlay entry and of the drawer's row while a group is joined
    component GroupMenu: QQC2.Menu {
        id: groupMenuRoot

        readonly property var syncPlay: root.syncPlay
        readonly property var members: syncPlay?.participants ?? []
        readonly property int shownMembers: 10

        PlainMenuItem {
            objectName: "syncPlayMenuGroup"
            plainText: (groupMenuRoot.syncPlay?.groupName ?? "").length > 0 ? groupMenuRoot.syncPlay.groupName : i18n("Unnamed group")
            enabled: false
        }
        Instantiator {
            model: groupMenuRoot.members.slice(0, groupMenuRoot.shownMembers)
            delegate: PlainMenuItem {
                required property string modelData
                objectName: "syncPlayMenuMember"
                plainText: modelData
                enabled: false
            }
            onObjectAdded: (index, object) => groupMenuRoot.insertItem(index + 1, object)
            onObjectRemoved: (index, object) => groupMenuRoot.removeItem(object)
        }
        QQC2.MenuItem {
            objectName: "syncPlayMenuMore"
            visible: groupMenuRoot.members.length > groupMenuRoot.shownMembers
            enabled: false
            text: i18ncp("@item:inmenu more people in the group than the menu lists", "and %1 more", "and %1 more", groupMenuRoot.members.length - groupMenuRoot.shownMembers)
        }
        QQC2.MenuSeparator {}
        QQC2.MenuItem {
            objectName: "syncPlayMenuFollow"
            text: groupMenuRoot.syncPlay?.following ? i18nc("@action:button", "Stop Following") : i18nc("@action:button", "Follow Again")
            icon.name: groupMenuRoot.syncPlay?.following ? "media-playback-pause" : "media-playback-start"
            enabled: !(groupMenuRoot.syncPlay?.offline ?? false)
            onTriggered: groupMenuRoot.syncPlay.following ? groupMenuRoot.syncPlay.stopFollowing() : groupMenuRoot.syncPlay.resumeFollowing()
        }
        QQC2.MenuItem {
            objectName: "syncPlayMenuLeave"
            text: i18nc("@action:button", "Leave Group")
            icon.name: "list-remove-user"
            onTriggered: groupMenuRoot.syncPlay.leaveGroup()
        }
        QQC2.MenuSeparator {}
        QQC2.MenuItem {
            objectName: "syncPlayMenuOpen"
            text: i18nc("@action:inmenu", "SyncPlay…")
            icon.name: "system-users"
            onTriggered: syncPlayDialog.open()
        }
    }

    // The menu of the sidebar's SyncPlay entry: it hangs from the entry that opens it
    GroupMenu {
        id: groupMenu
        objectName: "syncPlayGroupMenu"
    }
    // Past the margin and the line of the rail or the drawer, and a margin more, as the main menu
    PopupPlacement { id: groupPlacement; popup: groupMenu; beside: true; besideGap: Kirigami.Units.smallSpacing * 2 + 1 }

    QQC2.Menu {
        id: mainMenu
        objectName: "mainMenu"
        parent: root.menuOrigin
        // Over the drawer a click outside closes the menu only, and no row or page under it takes the click
        modal: root.drawerMenuRow !== null
        dim: false
        onClosed: {
            const runner = root.drawerMenuRunner;
            const choice = root.drawerMenuChoice;
            root.drawerMenuRow = null;
            root.drawerMenuRunner = null;
            root.drawerMenuChoice = null;
            if (runner && choice)
                runner(choice);
        }
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

        // The account and the server are on the users page; an offline window keeps its way back here
        QQC2.MenuItem {
            objectName: "leaveOfflineItem"
            visible: leaveOfflineAction.visible
            action: leaveOfflineAction
        }
        QQC2.MenuSeparator {
            visible: leaveOfflineAction.visible
        }

        QQC2.MenuItem {
            objectName: "menuAdminItem"
            visible: adminDashboardAction.visible
            action: adminDashboardAction
        }
        QQC2.MenuSeparator {
            visible: adminDashboardAction.visible
        }

        QQC2.MenuItem {
            objectName: "menuRefreshItem"
            visible: root.signedIn
            action: refreshAction
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

    AboutDialog {
        id: aboutDialog
        objectName: "aboutDialog"

        displayVersion: root.shell.displayVersion ?? ""
    }

    RemoteSessionsDialog {
        id: remoteSessionsDialog
        objectName: "remoteSessionsDialog"

        connection: root.connection
        maximumWidth: Math.min(Kirigami.Units.gridUnit * 34, root.shell.width - Kirigami.Units.largeSpacing * 2)
    }

    QueueDialog {
        id: queueDialog
        objectName: "queueDialog"

        connection: root.connection
        maximumWidth: Math.min(Kirigami.Units.gridUnit * 40, root.shell.width - Kirigami.Units.largeSpacing * 2)
    }

    SyncPlayDialog {
        id: syncPlayDialog
        objectName: "syncPlayDialog"

        connection: root.connection
        syncPlay: root.syncPlay
        userName: root.currentUser ? root.currentUser.name : ""
        maximumWidth: Math.min(Kirigami.Units.gridUnit * 34, root.shell.width - Kirigami.Units.largeSpacing * 2)
    }

    function openSyncPlay() {
        syncPlayDialog.open();
    }

    // Outside the dialog a failure is a notification that stays a while; outside the player a member who comes or goes is a short one, without the name (the player's own notice names them)
    Connections {
        target: root.syncPlay
        function onFailed(failure) {
            if (!syncPlayDialog.opened)
                root.shell.showPassiveNotification(syncPlayDialog.failureText(failure), "long");
        }
        function onMemberJoined() {
            if (!root.shell.playbackPageShowing)
                root.shell.showPassiveNotification(i18n("Someone joined the SyncPlay group"));
        }
        function onMemberLeft() {
            if (!root.shell.playbackPageShowing)
                root.shell.showPassiveNotification(i18n("Someone left the SyncPlay group"));
        }
        function onFellOutOfStep() {
            root.shell.showPassiveNotification(i18n("Out of step with the SyncPlay group. Playback catches up at the next pause or seek"), "long");
        }
    }

    // Opening while the startup layout runs leaves a y binding loop in Kirigami.Dialog
    function showAboutDialog() { Qt.callLater(aboutDialog.open); }

    ShortcutsDialog {
        id: shortcutsDialog
        objectName: "shortcutsDialog"
        tabActions: root.navigation.actions
        gamepad: root.shell.gamepad ?? null
    }

    // A hairline under the bar, as Dolphin and the other Breeze applications draw; it starts where the sidebar's own line ends
    Kirigami.Separator {
        objectName: "shellHeaderSeparator"
        anchors.left: parent.left
        anchors.leftMargin: root.shell.sidebarWidth
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        Accessible.ignored: true
    }
}
