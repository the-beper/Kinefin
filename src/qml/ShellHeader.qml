// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kirigamiaddons.components as Components

// The bar of a window without the sidebar: a Compact window (narrower than `compactLimit`), and a window with nothing to choose from (signed out, or offline with only Downloads). It has the search button, which opens the search popup, the one menu button (the account avatar) and, in Compact, the button that opens the sections in a drawer; the queue and the SyncPlay group fold into the menu. With the sidebar showing, the bar is hidden: its menu button, its menus, its dialogs and its actions stay here, and the button stands at the foot of the sidebar
Item {
    id: root
    objectName: "shellHeader"

    // The shell window: its functions open the pages, its connection holds the session and the search
    required property var shell
    // The sections the sidebar draws: their actions and shortcuts live there
    required property ShellNavigation navigation

    readonly property ConnectionController connection: shell.connection
    readonly property bool signedIn: connection.state === ConnectionController.Authenticated
    readonly property bool offline: connection.state === ConnectionController.Offline
    readonly property bool serverChosen: connection.state !== ConnectionController.Idle
    // Watching together; null when the application gives none
    readonly property var syncPlay: shell.syncPlay ?? null
    // The shell derives the mode from the window width it owns: a binding on the attached Window.window here kept its load-time value and never followed a resize
    readonly property bool compact: shell.compactHeader
    // The sidebar shows: the bar is hidden, and the menu button stands at the foot of the sidebar
    readonly property bool inSidebar: shell.sidebarWidth > 0
    // A SyncPlay group is joined: the sidebar's entry opens its menu, and in Compact the menu of the bar holds it
    readonly property bool groupJoined: signedIn && shell.groupJoined
    readonly property bool groupMenuOpened: groupMenu.opened

    // The signed-in user, found among the saved users by id: null when signed out or when the list has no such entry
    readonly property var currentUser: {
        const userId = connection.currentUserId;
        return signedIn && userId.length > 0 ? connection.users.find(user => user.id === userId) ?? null : null;
    }

    // The height and the margins of the bars of the pages
    implicitHeight: Math.max(rightCluster.implicitHeight + Kirigami.Units.smallSpacing * 2, Kirigami.Units.gridUnit * 2.5)

    // The button of a Compact window asks for the drawer with the sections
    signal navigationRequested

    // A gamepad's Start opens the main menu where its button stands
    function openMainMenu() {
        mainMenu.open();
    }

    // The menu of the SyncPlay group opens beside the button of the sidebar that asks
    function openGroupMenu(button) {
        groupPlacement.openFor(button);
    }

    // The drawer of a Compact window gives the keyboard back to the button that opened it
    function focusNavigationButton() {
        navigationButton.forceActiveFocus(Qt.TabFocusReason);
    }

    // Runs `action` at once, or once the user settled the unsaved preferences draft that the page above would lose
    function afterUnsavedSettled(action) {
        if (!shell.askBeforeLeaving(action))
            action();
    }

    // First in the file: siblings take the Tab order by declaration, so this button precedes the back button and the right cluster. A Compact window has no sidebar: it opens the drawer with the sections, which gives the keyboard back here when it closes without a choice
    QQC2.ToolButton {
        id: navigationButton
        objectName: "navigationButton"
        flat: false
        visible: root.compact && root.navigation.navigable
        anchors.left: parent.left
        anchors.leftMargin: Kirigami.Units.largeSpacing
        anchors.verticalCenter: parent.verticalCenter
        text: i18nc("@action:button", "Open Navigation")
        icon.name: "application-menu"
        display: QQC2.AbstractButton.IconOnly
        QQC2.ToolTip.text: text
        QQC2.ToolTip.visible: hovered || visualFocus
        QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
        onClicked: root.navigationRequested()
    }

    // Only a narrow Settings page with an entry open has it: there the entry hides the list, and this is the pointer's way back to it (with the sidebar showing the page has its own)
    QQC2.ToolButton {
        id: headerBackButton
        objectName: "headerBackButton"
        flat: false
        visible: shell.pageStack.currentItem?.objectName === "settingsPage" && shell.pageStack.currentItem.narrow && shell.pageStack.currentItem.entryOpen
        z: 2
        anchors.left: parent.left
        anchors.leftMargin: Kirigami.Units.largeSpacing + (navigationButton.visible ? navigationButton.implicitWidth + Kirigami.Units.smallSpacing : 0)
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

        // The search popup, as Ctrl+F opens it
        QQC2.ToolButton {
            id: searchButton
            objectName: "headerSearchButton"

            Layout.alignment: Qt.AlignVCenter
            visible: root.compact && root.signedIn
            flat: false
            text: i18nc("@action:button", "Search")
            icon.name: "search"
            display: QQC2.AbstractButton.IconOnly
            QQC2.ToolTip.text: text
            QQC2.ToolTip.visible: hovered || visualFocus
            QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
            down: shell.currentSection === "search"
            onClicked: shell.openSearch()
        }

        // The one menu: the avatar of the user marks the account, the arrow the menu; signed out it is the plain menu icon. With the sidebar showing, this same button stands at the foot of the sidebar, with the avatar alone
        QQC2.ToolButton {
            id: mainMenuButton
            objectName: "mainMenuButton"

            // Framed in the bar like the buttons of the pages, bare in the rail like its entries
            flat: root.inSidebar
            parent: root.inSidebar ? (root.shell.sidebarUserSlot ?? rightCluster) : rightCluster
            Layout.alignment: Qt.AlignHCenter | Qt.AlignVCenter
            // In the rail the hit area is as wide as the other entries, and the avatar stays in the middle of it
            Layout.fillWidth: root.inSidebar
            Layout.leftMargin: root.inSidebar ? Kirigami.Units.smallSpacing : 0
            Layout.rightMargin: root.inSidebar ? Kirigami.Units.smallSpacing : 0
            Layout.minimumHeight: implicitHeight
            implicitHeight: root.inSidebar ? Kirigami.Units.iconSizes.smallMedium + Kirigami.Units.largeSpacing * 2 : searchButton.implicitHeight
            // The hover and focus frame keeps some room around the avatar and the arrow
            leftPadding: root.inSidebar ? Kirigami.Units.smallSpacing : Kirigami.Units.largeSpacing
            rightPadding: root.inSidebar ? Kirigami.Units.smallSpacing : Kirigami.Units.largeSpacing
            // Not `text`: with no icon of its own the style would draw it beside the avatar; the label is the tooltip, and the accessible name adds who is signed in
            readonly property string label: i18nc("@action:button", "Main Menu")
            Accessible.name: root.signedIn && root.currentUser ? i18nc("@label %1 is a user name", "Main Menu, signed in as %1", root.currentUser.name) : label
            Accessible.role: Accessible.ButtonMenu
            display: QQC2.AbstractButton.IconOnly
            QQC2.ToolTip.text: label
            QQC2.ToolTip.visible: !root.inSidebar && (hovered || visualFocus) && !mainMenu.opened
            QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
            // The menu marks where its entries lead while one of them shows
            down: shell.currentSection === "settings"
            onClicked: mainMenu.open()

            // The content is centred in the button whatever its width, and the avatar is as wide as it is tall
            contentItem: Item {
                implicitWidth: userRow.implicitWidth
                implicitHeight: userRow.implicitHeight

                RowLayout {
                    id: userRow
                    anchors.centerIn: parent
                    spacing: Kirigami.Units.smallSpacing

                    Components.Avatar {
                        objectName: "accountAvatar"
                        visible: root.signedIn
                        // Square, so that the avatar is round whatever the width of the button
                        Layout.preferredWidth: Kirigami.Units.iconSizes.smallMedium
                        Layout.preferredHeight: Kirigami.Units.iconSizes.smallMedium
                        Layout.minimumWidth: Kirigami.Units.iconSizes.smallMedium
                        Layout.minimumHeight: Kirigami.Units.iconSizes.smallMedium
                        Layout.maximumWidth: Kirigami.Units.iconSizes.smallMedium
                        Layout.maximumHeight: Kirigami.Units.iconSizes.smallMedium
                        Accessible.ignored: true
                        name: root.currentUser ? root.currentUser.name : ""
                        // Only URLs built by JellyfinClient::userImageUrl belong here: a raw server string must never become a source
                        source: root.currentUser ? root.currentUser.imageUrl : ""
                        // The image is decoded at the size it shows
                        sourceSize.width: width * Screen.devicePixelRatio
                        sourceSize.height: height * Screen.devicePixelRatio
                    }
                    Kirigami.Icon {
                        visible: root.signedIn && !root.inSidebar
                        source: "arrow-down"
                        color: Kirigami.Theme.textColor
                        Layout.preferredWidth: Kirigami.Units.iconSizes.small
                        Layout.preferredHeight: Kirigami.Units.iconSizes.small
                    }
                    Kirigami.Icon {
                        visible: !root.signedIn
                        source: "open-menu-symbolic"
                        color: Kirigami.Theme.textColor
                        Layout.preferredWidth: Kirigami.Units.iconSizes.smallMedium
                        Layout.preferredHeight: Kirigami.Units.iconSizes.smallMedium
                    }
                }
            }

            // The rail's tooltip stands beside the button, where it never covers the entry above
            PlainToolTip {
                text: mainMenuButton.label
                visible: root.inSidebar && (mainMenuButton.hovered || mainMenuButton.visualFocus) && !mainMenu.opened
                x: mainMenuButton.mirrored ? -width - Kirigami.Units.smallSpacing : mainMenuButton.width + Kirigami.Units.smallSpacing
                y: Math.round((mainMenuButton.height - height) / 2)
            }
        }
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

    // The queue of a Compact window, as a menu entry, since the bar has no room for it; a MenuItem does not mirror the visibility of its action, so the entry hides on its own
    Kirigami.Action {
        id: compactQueueAction
        objectName: "compactQueueAction"

        text: i18nc("@action:inmenu", "Queue")
        icon.name: "media-track-queue-amarok"
        visible: root.compact && root.signedIn
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

    Kirigami.Action {
        id: syncPlayAction
        objectName: "syncPlayAction"

        text: i18nc("@action:inmenu Watching together with other users, the server's feature name", "SyncPlay")
        icon.name: "system-users"
        visible: root.signedIn && !!root.syncPlay && (root.syncPlay.available || root.syncPlay.status !== SyncPlayController.Idle)
        onTriggered: syncPlayDialog.open()
    }

    // The account entries of the menu: what a session can lose, and what a chosen server can still be left for
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

        text: i18nc("@action:inmenu", "Sign Out…")
        tooltip: i18nc("@info:tooltip", "Remove this user’s saved session from this system")
        icon.name: "system-log-out"
        visible: root.signedIn
        // Signing out removes the saved session and queue of the user, so it asks first (about unsaved preferences, then about the sign-out)
        onTriggered: root.afterUnsavedSettled(() => signOutDialog.openFor(root.currentUser ? root.currentUser.name : ""))
    }
    Kirigami.Action {
        id: leaveOfflineAction
        objectName: "leaveOfflineAction"

        text: i18nc("@action:inmenu Leaves the offline downloads and shows the servers", "Leave Offline")
        icon.name: "network-disconnect"
        visible: root.offline
        onTriggered: root.connection.cancel()
    }
    Kirigami.Action {
        id: switchServerAction
        objectName: "switchServerAction"

        text: i18nc("@action:inmenu Goes back to the list of servers", "Switch Server")
        icon.name: "network-server"
        visible: root.serverChosen && !root.offline
        onTriggered: root.afterUnsavedSettled(() => root.connection.cancel())
    }

    // Under the button, right-aligned with it (left-aligned when mirrored) as the bar ends there, and above it in a window too short to hold the menu below; beside it, as high as the window allows, while the button stands in the sidebar
    PopupPlacement { popup: mainMenu; anchor: mainMenuButton; beside: root.inSidebar }

    // The quick actions of the group the viewer is in: the menu of the sidebar's entry and, in a compact window, an entry of the main menu
    component GroupMenu: QQC2.Menu {
        id: groupMenuRoot

        // The submenu of a compact window leaves it out: the main menu has the SyncPlay entry right above it
        property bool withDialogEntry: true
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
        QQC2.MenuSeparator {
            visible: groupMenuRoot.withDialogEntry
        }
        QQC2.MenuItem {
            objectName: "syncPlayMenuOpen"
            visible: groupMenuRoot.withDialogEntry
            text: i18nc("@action:inmenu", "SyncPlay…")
            icon.name: "system-users"
            onTriggered: syncPlayDialog.open()
        }
    }

    // A compact window has no sidebar entry: the group's menu is an entry of the main menu, under SyncPlay
    Instantiator {
        model: root.compact && root.groupJoined ? 1 : 0
        delegate: GroupMenu {
            objectName: "syncPlayGroupSubmenu"
            withDialogEntry: false
            title: i18nc("@title:menu", "SyncPlay Group")
        }
        onObjectAdded: (index, object) => {
            let at = mainMenu.count;
            for (let i = 0; i < mainMenu.count; ++i) {
                if (mainMenu.itemAt(i)?.objectName === "menuSyncPlayItem")
                    at = i + 1;
            }
            mainMenu.insertMenu(at, object);
        }
        onObjectRemoved: (index, object) => mainMenu.removeMenu(object)
    }

    // The menu of the sidebar's SyncPlay entry: it hangs from the entry that opens it
    GroupMenu {
        id: groupMenu
        objectName: "syncPlayGroupMenu"
    }
    PopupPlacement { id: groupPlacement; popup: groupMenu; beside: true }

    QQC2.Menu {
        id: mainMenu
        objectName: "mainMenu"
        parent: mainMenuButton
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

        // Who is signed in and where: a plain item, no entry, so neither the pointer nor the keyboard stops on it (and not disabled, which would grey the name); the server has no name in the data, its address stands for it
        Item {
            id: menuHeader
            objectName: "mainMenuHeader"

            readonly property string serverAddress: root.connection.serverUrl.replace(/^https?:\/\//i, "")

            visible: root.signedIn
            implicitWidth: Math.min(headerRow.implicitWidth, Kirigami.Units.gridUnit * 20) + Kirigami.Units.largeSpacing * 2
            // The menu's list keeps the room of a hidden plain item: signed out it takes none
            implicitHeight: root.signedIn ? headerRow.implicitHeight + Kirigami.Units.largeSpacing * 2 : 0

            RowLayout {
                id: headerRow

                anchors {
                    fill: parent
                    margins: Kirigami.Units.largeSpacing
                }
                spacing: Kirigami.Units.largeSpacing

                Components.Avatar {
                    Layout.preferredWidth: Kirigami.Units.iconSizes.medium
                    Layout.preferredHeight: Kirigami.Units.iconSizes.medium
                    name: root.currentUser ? root.currentUser.name : ""
                    source: root.currentUser ? root.currentUser.imageUrl : ""
                    sourceSize.width: width * Screen.devicePixelRatio
                    sourceSize.height: height * Screen.devicePixelRatio
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 0

                    QQC2.Label {
                        Layout.fillWidth: true
                        text: root.currentUser ? root.currentUser.name : ""
                        color: Kirigami.Theme.textColor
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                    }
                    QQC2.Label {
                        Layout.fillWidth: true
                        text: menuHeader.serverAddress
                        color: Kirigami.Theme.disabledTextColor
                        font: Kirigami.Theme.smallFont
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                    }
                }
            }
        }
        QQC2.MenuSeparator {
            visible: root.signedIn
        }

        QQC2.MenuItem {
            objectName: "switchUserItem"
            visible: root.signedIn
            action: switchUserAction
        }
        QQC2.MenuItem {
            objectName: "signOutItem"
            visible: root.signedIn
            action: signOutAction
            // No tooltip: the confirmation the entry opens says what signing out removes, the description stays for screen readers
            Accessible.description: signOutAction.tooltip
        }
        QQC2.MenuItem {
            objectName: "switchServerItem"
            visible: switchServerAction.visible
            action: switchServerAction
        }
        QQC2.MenuItem {
            objectName: "leaveOfflineItem"
            visible: leaveOfflineAction.visible
            action: leaveOfflineAction
        }
        QQC2.MenuSeparator {
            visible: root.serverChosen
        }

        // In Compact mode the queue of the right cluster folds in here
        QQC2.MenuItem {
            objectName: "menuQueueItem"
            visible: root.compact
            action: compactQueueAction
        }
        QQC2.MenuSeparator {
            visible: root.compact
        }

        QQC2.MenuItem {
            objectName: "menuRemoteItem"
            visible: root.signedIn
            action: remoteSessionsAction
        }
        QQC2.MenuItem {
            objectName: "menuSyncPlayItem"
            visible: syncPlayAction.visible
            action: syncPlayAction
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

        displayVersion: root.shell.displayVersion ?? ""
    }

    RemoteSessionsDialog {
        id: remoteSessionsDialog
        objectName: "remoteSessionsDialog"

        connection: root.connection
        maximumWidth: Math.min(Kirigami.Units.gridUnit * 34, root.shell.width - Kirigami.Units.largeSpacing * 2)
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
