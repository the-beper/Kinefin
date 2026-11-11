// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// The top bar: the main menu at the left, the Home tab in the middle, and search, favorites, queue and the account menu at the right.
// The width decides the mode: Wide from 36 grid units (labelled tab, wide search box), Medium from 24 (icon-only tab, narrow box), Compact below 24 (search, favorites and queue fold into the main menu).
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

    // The box holds the keyboard while the term is being typed: the shell does not move the focus while a keystroke of its own opens the search page
    readonly property bool searchFieldHasFocus: searchField.activeFocus

    // First in the file: siblings take the Tab order by declaration, so the menu button precedes the tab and the right cluster
    QQC2.ToolButton {
        id: menuButton
        objectName: "mainMenuButton"

        anchors {
            left: parent.left
            leftMargin: Kirigami.Units.smallSpacing
            verticalCenter: parent.verticalCenter
        }
        text: i18nc("@action:button", "Main Menu")
        icon.name: "open-menu-symbolic"
        // Only the icon; the text is for screen readers
        display: QQC2.AbstractButton.IconOnly
        // The menu marks where its entries lead while one of them shows
        down: shell.currentSection === "settings" || shell.currentSection === "about"
        onClicked: mainMenu.popup(menuButton, 0, menuButton.height)
    }

    // The bar spans the whole header, so the room for the buttons at its sides comes from its padding
    Kirigami.NavigationTabBar {
        id: bar

        anchors.fill: parent
        leftPadding: menuButton.width + Kirigami.Units.smallSpacing * 2
        rightPadding: rightCluster.width + Kirigami.Units.smallSpacing * 2

        actions: [homeAction]

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

    RowLayout {
        id: rightCluster

        anchors {
            right: parent.right
            rightMargin: Kirigami.Units.smallSpacing
            verticalCenter: parent.verticalCenter
        }
        spacing: Kirigami.Units.smallSpacing

        // The global search box: it owns the term, and the first keystroke opens the search page above the current one
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
            placeholderText: i18nc("@info:placeholder", "Search")

            // The box is the source of truth of the term: its own text never echoes back
            onTextChanged: {
                if (text !== root.connection.search.term) {
                    root.connection.search.term = text;
                    root.shell.openSearchIfMissing();
                }
            }
            onAccepted: root.connection.search.searchNow()
            Keys.onDownPressed: root.shell.searchResultsDown()
            Component.onCompleted: text = root.connection.search.term
        }

        // The star already sits where the Favorites page will go
        QQC2.ToolButton {
            objectName: "favoritesButton"

            Layout.alignment: Qt.AlignVCenter
            visible: !root.compact
            text: i18nc("@action:button", "Favorites")
            icon.name: "favorite"
            display: QQC2.AbstractButton.IconOnly
            enabled: false
        }

        QQC2.ToolButton {
            objectName: "queueButton"

            Layout.alignment: Qt.AlignVCenter
            visible: !root.compact
            text: i18nc("@action:button", "Queue")
            icon.name: "view-media-playlist"
            display: QQC2.AbstractButton.IconOnly
            enabled: root.signedIn
            down: shell.currentSection === "queue"
            onClicked: shell.openSection("queuePage", "QueuePage.qml", { connection: shell.connection })
        }

        QQC2.ToolButton {
            id: accountButton
            objectName: "accountButton"

            Layout.alignment: Qt.AlignVCenter
            text: i18nc("@action:button", "Account")
            icon.name: "im-user"
            display: QQC2.AbstractButton.IconOnly
            visible: root.signedIn || root.serverChosen
            onClicked: accountMenu.popup(accountButton, 0, accountButton.height)
        }
    }

    // The term reached the box from outside (a field of the search page, a term the page took with it when it left): the box follows it, and never while it echoes its own edit
    Connections {
        target: root.connection.search
        function onTermChanged() {
            if (searchField.text !== root.connection.search.term)
                searchField.text = root.connection.search.term;
        }
    }

    Kirigami.Action {
        id: homeAction
        objectName: "homeTabAction"

        text: i18nc("@title:tab", "Home")
        icon.name: "go-home"
        enabled: root.signedIn
        checked: shell.currentSection === "home"
        shortcut: "Ctrl+1"
        onTriggered: shell.goHome()
    }

    // The shortcuts live on the actions, not on the menu items, so they work with the menu closed
    Kirigami.Action {
        id: settingsAction
        objectName: "settingsAction"

        text: i18nc("@action:inmenu", "Settings")
        icon.name: "configure"
        shortcut: "Ctrl+,"
        onTriggered: shell.openSection("settingsPage", "SettingsPage.qml", { settings: shell.settings })
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
        onTriggered: shell.openSection("aboutPage", "AboutPage.qml", {})
    }
    Kirigami.Action {
        id: adminDashboardAction
        objectName: "adminDashboardAction"

        text: i18nc("@action:inmenu", "Admin Dashboard")
        icon.name: "preferences-system-network"
        visible: root.signedIn
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
        enabled: false
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

    // The account menu: what a session can lose, and what a chosen server can still be left for
    Kirigami.Action {
        id: switchUserAction
        objectName: "switchUserAction"

        text: i18nc("@action:inmenu", "Switch User")
        icon.name: "system-switch-user"
        visible: root.signedIn
        onTriggered: root.connection.switchUser()
    }
    Kirigami.Action {
        id: signOutAction
        objectName: "signOutAction"

        text: i18nc("@action:inmenu", "Sign Out")
        icon.name: "system-log-out"
        visible: root.signedIn
        onTriggered: root.connection.signOut()
    }
    Kirigami.Action {
        id: switchServerAction
        objectName: "switchServerAction"

        text: i18nc("@action:inmenu", "Switch Server")
        icon.name: "network-server"
        visible: root.serverChosen
        onTriggered: root.connection.cancel()
    }

    QQC2.Menu {
        id: mainMenu
        objectName: "mainMenu"

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
            visible: root.compact
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
        QQC2.MenuItem {
            objectName: "menuAdminItem"
            visible: root.signedIn
            action: adminDashboardAction
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
        }
        QQC2.MenuItem {
            objectName: "switchServerItem"
            visible: root.serverChosen
            action: switchServerAction
        }
    }

    ShortcutsDialog {
        id: shortcutsDialog
        objectName: "shortcutsDialog"
    }
}
