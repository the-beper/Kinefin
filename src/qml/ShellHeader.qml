// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kirigamiaddons.components as Components

// The top bar: the Home, Movies, Shows, Downloads, Favorites and Playlists tabs at the left, content-sized with an underline under the current one (Movies and Shows only while the server has a library of the type), and search, queue and the one menu button (the account avatar) at the right; the menu holds the account and the application entries. The tabs carry their labels while they fit with the search box at its minimum width (`labelledTabs`) and are icon-only otherwise; Compact (narrower than the bar with icon-only tabs and the box at its minimum, `minimumBarWidth`) turns the box into a search button and folds the queue into the menu
Item {
    id: root
    objectName: "shellHeader"

    // The shell window: its functions open the pages, its connection holds the session and the search
    required property var shell

    readonly property ConnectionController connection: shell.connection
    readonly property bool signedIn: connection.state === ConnectionController.Authenticated
    readonly property bool offline: connection.state === ConnectionController.Offline
    readonly property bool serverChosen: connection.state !== ConnectionController.Idle
    // Watching together; null when the application gives none
    readonly property var syncPlay: shell.syncPlay ?? null
    // The shell derives the mode from the window width it owns: a binding on the attached Window.window here kept its load-time value and never followed a resize
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

    function openReleasePage(url) {
        if (!Qt.openUrlExternally(url))
            shell.showPassiveNotification(i18n("Could not open the release page in the browser"));
    }

    UpdateChecker {
        id: updateChecker
        objectName: "updateChecker"

        onUpdateAvailable: (version, releaseUrl) => {
            if (!root.updateChecksEnabled)
                return;
            updateDialog.version = version;
            updateDialog.releaseUrl = releaseUrl;
            if (!root.manualUpdateCheck) {
                // The automatic check never opens a dialog by itself
                shell.showPassiveNotification(i18n("Kinefin %1 is available", version), "long", i18nc("@action:button", "Open Release Page"), () => root.openReleasePage(releaseUrl));
                return;
            }
            // The user asked for this check: the result shows where they are
            root.manualUpdateCheck = false;
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
                updateDialog.close();
            }
        }
    }

    implicitHeight: Math.max(tabsRow.implicitHeight, rightCluster.implicitHeight + Kirigami.Units.smallSpacing * 2)

    // Ctrl+F focuses the box and selects what is in it
    function focusSearchField() {
        searchField.forceActiveFocus(Qt.TabFocusReason);
        searchField.selectAll();
    }

    // The keyboard comes back to the box after the page its typing opened was created: unlike Ctrl+F it must not select, or the keystroke that opened the page would be replaced
    function refocusSearchField() {
        searchField.forceActiveFocus(Qt.TabFocusReason);
    }

    // A gamepad's Start opens the main menu where its button stands
    function openMainMenu() {
        mainMenu.open();
    }

    // Move between the root tabs in their visible order, wrapping (a library type may be absent). Detail pages belong to their root tab; using the shortcut returns to the selected tab, just like clicking its header button
    function switchRelativeTab(step) {
        if (!signedIn)
            return;
        const tabs = tabActions.filter(action => action.visible);
        if (tabs.length < 2)
            return;
        const current = Math.max(0, tabs.findIndex(action => action.section === shell.rootTab));
        tabs[(current + step + tabs.length) % tabs.length].trigger();
    }

    // The tabs in their order: the fixed sections, then the library types after a divider; the Ctrl+N shortcuts follow the position among the visible ones
    readonly property var fixedTabActions: [homeAction, downloadsAction, favoritesTabAction, playlistsTabAction]
    readonly property var libraryTabActions: [moviesAction, tvShowsAction]
    readonly property var tabActions: fixedTabActions.concat(libraryTabActions)
    // The divider stands only between two groups that both show a tab
    readonly property bool tabGroupsDivided: fixedTabActions.some(action => action.visible) && libraryTabActions.some(action => action.visible)
    function tabShortcut(action) {
        let position = 0;
        for (const candidate of tabActions) {
            if (candidate.visible)
                ++position;
            if (candidate === action)
                return candidate.visible ? "Ctrl+" + position : "";
        }
        return "";
    }

    // Whether the tabs carry their labels: computed from the translated labels and the fixed parts of the bar, never from the layout that results from it
    readonly property real tabHorizontalPadding: Kirigami.Units.largeSpacing
    readonly property real tabSpacing: Kirigami.Units.smallSpacing
    readonly property real tabsStart: Kirigami.Units.smallSpacing + (headerBackButton.visible ? headerBackButton.implicitWidth + Kirigami.Units.smallSpacing : 0)
    readonly property real searchMinimumWidth: Kirigami.Units.gridUnit * 11
    readonly property real searchMaximumWidth: Kirigami.Units.gridUnit * 20
    // What the right cluster takes besides the search box outside Compact mode, with the spacing before it and the margin after it
    readonly property real rightFixedWidth: queueButton.implicitWidth + clusterSeparator.implicitWidth + mainMenuButton.implicitWidth + rightCluster.spacing * 3 + Kirigami.Units.smallSpacing
    FontMetrics {
        id: tabFontMetrics
        font: Kirigami.Theme.defaultFont
    }
    function tabsWidth(labelled) {
        let width = 0;
        let count = 0;
        for (const action of tabActions) {
            if (!action.visible)
                continue;
            width += tabHorizontalPadding * 2 + Kirigami.Units.iconSizes.smallMedium;
            if (labelled)
                width += tabSpacing + Math.ceil(tabFontMetrics.advanceWidth(action.text));
            ++count;
        }
        // The divider is one more item of the row, with its own margins
        if (tabGroupsDivided)
            width += tabSpacing + tabsDivider.implicitWidth + tabsDivider.Layout.leftMargin + tabsDivider.Layout.rightMargin;
        return width + Math.max(0, count - 1) * tabSpacing;
    }
    function barWidth(labelled) {
        return tabsStart + tabsWidth(labelled) + Kirigami.Units.smallSpacing * 2 + searchMinimumWidth + rightCluster.spacing + rightFixedWidth;
    }
    readonly property bool labelledTabs: !compact && barWidth(true) <= width
    // The narrowest bar that keeps the box and the queue: the shell's Compact limit, so nothing overlaps whatever the font and the icon sizes
    readonly property real minimumBarWidth: barWidth(false)

    // The list the box searches: the model of the type tab at the root of the stack; on any other page the box feeds the global search
    readonly property var localModel: {
        // The Settings page opened over a tab is no part of it: the box searches the whole server there, and on the Favorites and Playlists tabs
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

    // First in the file: siblings take the Tab order by declaration, so the back button precedes the tabs and the right cluster. Only a narrow Settings page with an entry open has it: there the entry hides the list, and this is the pointer's way back to it (a wide one leaves through the tabs)
    QQC2.ToolButton {
        id: headerBackButton
        objectName: "headerBackButton"
        visible: shell.pageStack.currentItem?.objectName === "settingsPage" && shell.pageStack.currentItem.narrow && shell.pageStack.currentItem.entryOpen
        z: 2
        anchors.left: parent.left
        anchors.leftMargin: Kirigami.Units.smallSpacing
        anchors.verticalCenter: parent.verticalCenter
        text: i18nc("@action:button", "Back")
        icon.name: Qt.application.layoutDirection === Qt.RightToLeft ? "go-next" : "go-previous"
        display: QQC2.AbstractButton.IconOnly
        QQC2.ToolTip.text: text
        QQC2.ToolTip.visible: hovered || visualFocus
        QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
        onClicked: shell.pageStack.goBack()
    }

    // The tabs are as wide as their content; the underline of the current one sits at the bottom edge of the header
    RowLayout {
        id: tabsRow
        objectName: "headerTabs"

        // The tabs report the page tab role: their row is the list they belong to
        Accessible.role: Accessible.PageTabList

        anchors {
            left: headerBackButton.visible ? headerBackButton.right : parent.left
            leftMargin: Kirigami.Units.smallSpacing
            top: parent.top
            bottom: parent.bottom
        }
        spacing: root.tabSpacing
        visible: root.tabActions.filter(action => action.visible).length > 1

        Repeater {
            model: root.fixedTabActions
            delegate: tabButton
        }

        Kirigami.Separator {
            id: tabsDivider
            objectName: "headerTabsDivider"
            visible: root.tabGroupsDivided
            Layout.alignment: Qt.AlignVCenter
            Layout.preferredHeight: Kirigami.Units.gridUnit
            Layout.leftMargin: Kirigami.Units.smallSpacing
            Layout.rightMargin: Kirigami.Units.smallSpacing
        }

        Repeater {
            model: root.libraryTabActions
            delegate: tabButton
        }

        Component {
            id: tabButton

            Kirigami.NavigationTabButton {
                id: tab

                required property Kirigami.Action modelData

                text: modelData.text
                icon.name: modelData.icon.name
                visible: modelData.visible
                enabled: modelData.enabled
                // A plain binding: the button is no member of a tab group and a click does not toggle it
                checkable: false
                checked: root.shell.currentSection === modelData.section
                // Not checkable, so the screen reader learns the current tab from the selection
                Accessible.role: Accessible.PageTab
                Accessible.selected: checked
                display: root.labelledTabs ? QQC2.AbstractButton.TextBesideIcon : QQC2.AbstractButton.IconOnly
                onClicked: modelData.trigger()
                Keys.onReturnPressed: animateClick()
                Keys.onEnterPressed: animateClick()

                QQC2.ToolTip.text: display === QQC2.AbstractButton.IconOnly ? text : ""
                QQC2.ToolTip.visible: (hovered || visualFocus) && QQC2.ToolTip.text.length > 0

                Layout.fillHeight: true

                leftPadding: root.tabHorizontalPadding
                rightPadding: root.tabHorizontalPadding
                topPadding: Kirigami.Units.largeSpacing
                bottomPadding: Kirigami.Units.largeSpacing
                leftInset: 0
                rightInset: 0
                topInset: 0
                bottomInset: 0
                spacing: root.tabSpacing
                implicitWidth: implicitContentWidth + leftPadding + rightPadding
                implicitHeight: implicitContentHeight + topPadding + bottomPadding

                contentItem: RowLayout {
                    spacing: tab.spacing

                    Kirigami.Icon {
                        source: tab.icon.name
                        color: Kirigami.Theme.textColor
                        Layout.preferredWidth: Kirigami.Units.iconSizes.smallMedium
                        Layout.preferredHeight: Kirigami.Units.iconSizes.smallMedium
                    }
                    QQC2.Label {
                        visible: tab.display !== QQC2.AbstractButton.IconOnly
                        text: tab.text
                        color: Kirigami.Theme.textColor
                        textFormat: Text.PlainText
                    }
                }

                // The same geometry in every state: hover, focus and the current mark only add paint
                background: Item {
                    Rectangle {
                        anchors {
                            fill: parent
                            topMargin: Kirigami.Units.smallSpacing
                            bottomMargin: Kirigami.Units.smallSpacing
                        }
                        radius: Kirigami.Units.cornerRadius
                        color: tab.down ? Qt.alpha(Kirigami.Theme.highlightColor, 0.3) : (tab.hovered ? Qt.alpha(Kirigami.Theme.highlightColor, 0.15) : "transparent")
                        border.width: tab.visualFocus ? 1 : 0
                        border.color: Kirigami.Theme.highlightColor
                    }
                    Rectangle {
                        objectName: "tabIndicator"
                        visible: tab.checked
                        anchors.bottom: parent.bottom
                        anchors.horizontalCenter: parent.horizontalCenter
                        width: Math.max(0, tab.availableWidth - Kirigami.Units.smallSpacing)
                        height: Math.max(2, Math.round(Kirigami.Units.smallSpacing * 0.75))
                        radius: height / 2
                        color: Kirigami.Theme.highlightColor
                    }
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

        // The search box: on a type tab it searches that tab's list and the first keystroke pops back to the tab; elsewhere it owns the global term and the first keystroke opens the search page above the current one
        Kirigami.SearchField {
            id: searchField
            objectName: "headerSearchField"

            // The room the tabs leave, between the minimum and the maximum width
            Layout.preferredWidth: Math.min(root.searchMaximumWidth, Math.max(root.searchMinimumWidth,
                root.width - root.tabsStart - root.tabsWidth(root.labelledTabs) - Kirigami.Units.smallSpacing * 2 - rightCluster.spacing - root.rightFixedWidth))
            Layout.alignment: Qt.AlignVCenter
            // Before signing in (servers, sign-in, user picker) there is nothing to search and no queue: the menu alone stays
            visible: !root.compact && root.signedIn
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

        // The box of a Compact window: as Ctrl+F, it opens the search page with its own field, or the field of the type tab in front
        QQC2.ToolButton {
            id: searchButton
            objectName: "headerSearchButton"

            Layout.alignment: Qt.AlignVCenter
            visible: root.compact && root.signedIn
            text: i18nc("@action:button", "Search")
            icon.name: "search"
            display: QQC2.AbstractButton.IconOnly
            QQC2.ToolTip.text: text
            QQC2.ToolTip.visible: hovered || visualFocus
            QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
            down: shell.currentSection === "search"
            onClicked: shell.openSearch()
        }

        QQC2.ToolButton {
            id: queueButton
            objectName: "queueButton"

            readonly property int queued: root.signedIn ? root.connection.queue.count : 0

            Layout.alignment: Qt.AlignVCenter
            visible: !root.compact && root.signedIn
            text: i18nc("@action:button", "Queue")
            // Not the icon of the Playlists tab
            icon.name: "media-track-queue-amarok"
            display: QQC2.AbstractButton.IconOnly
            QQC2.ToolTip.text: queued > 0 ? i18ncp("@info:tooltip the queue button and how many items the queue holds", "Queue, %1 item", "Queue, %1 items", queued) : text
            QQC2.ToolTip.visible: hovered || visualFocus
            QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
            Accessible.name: QQC2.ToolTip.text
            down: shell.currentSection === "queue"
            onClicked: shell.openSection("queuePage", "QueuePage.qml", { connection: shell.connection })

            // How many items wait in the queue, on the icon's corner; the accessible name says it in words
            Rectangle {
                objectName: "queueCountBadge"
                visible: queueButton.queued > 0
                anchors {
                    top: parent.top
                    right: parent.right
                    topMargin: Kirigami.Units.smallSpacing / 2
                    rightMargin: Kirigami.Units.smallSpacing / 2
                }
                height: Math.round(Kirigami.Units.gridUnit * 0.8)
                width: Math.max(height, badgeLabel.implicitWidth + Kirigami.Units.smallSpacing)
                radius: height / 2
                color: Kirigami.Theme.highlightColor
                Accessible.ignored: true

                QQC2.Label {
                    id: badgeLabel
                    anchors.centerIn: parent
                    font.pixelSize: Math.round(parent.height * 0.7)
                    color: Kirigami.Theme.highlightedTextColor
                    textFormat: Text.PlainText
                    text: queueButton.queued > 99 ? i18nc("@label more than 99 items in the queue badge", "99+") : Number(queueButton.queued).toLocaleString(Qt.locale(), "f", 0)
                }
            }
        }

        Kirigami.Separator {
            id: clusterSeparator

            Layout.alignment: Qt.AlignVCenter
            Layout.preferredHeight: Kirigami.Units.gridUnit
            visible: !root.compact && root.signedIn
        }

        // The one menu: the avatar of the user marks the account, the arrow the menu; signed out it is the plain menu icon
        QQC2.ToolButton {
            id: mainMenuButton
            objectName: "mainMenuButton"

            Layout.alignment: Qt.AlignVCenter
            implicitHeight: queueButton.implicitHeight
            // The hover and focus frame keeps some room around the avatar and the arrow
            leftPadding: Kirigami.Units.largeSpacing
            rightPadding: Kirigami.Units.largeSpacing
            // Not `text`: with no icon of its own the style would draw it beside the avatar. No tooltip either (the avatar and the arrow say what it is); the name is for screen readers
            readonly property string label: i18nc("@action:button", "Main Menu")
            Accessible.name: root.signedIn && root.currentUser ? i18nc("@label %1 is a user name", "Main Menu, signed in as %1", root.currentUser.name) : label
            Accessible.role: Accessible.ButtonMenu
            display: QQC2.AbstractButton.IconOnly
            // The menu marks where its entries lead while one of them shows
            down: shell.currentSection === "settings"
            onClicked: mainMenu.open()

            contentItem: RowLayout {
                spacing: Kirigami.Units.smallSpacing

                Components.Avatar {
                    objectName: "accountAvatar"
                    visible: root.signedIn
                    Layout.preferredWidth: Kirigami.Units.iconSizes.smallMedium
                    Layout.preferredHeight: Kirigami.Units.iconSizes.smallMedium
                    Accessible.ignored: true
                    name: root.currentUser ? root.currentUser.name : ""
                    // Only URLs built by JellyfinClient::userImageUrl belong here: a raw server string must never become a source
                    source: root.currentUser ? root.currentUser.imageUrl : ""
                    // The image is decoded at the size it shows
                    sourceSize.width: width * Screen.devicePixelRatio
                    sourceSize.height: height * Screen.devicePixelRatio
                }
                Kirigami.Icon {
                    visible: root.signedIn
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

    Kirigami.Action {
        id: homeAction
        objectName: "homeTabAction"

        property string section: "home"
        text: i18nc("@title:tab", "Home")
        icon.name: "go-home"
        visible: root.signedIn
        enabled: root.signedIn
        shortcut: root.tabShortcut(homeAction)
        onTriggered: shell.goHome()
    }

    // Ctrl+2 and onwards follow the position of the visible tabs: a missing Movies tab moves Shows to Ctrl+2
    Kirigami.Action {
        id: moviesAction
        objectName: "moviesTabAction"

        property string section: "movies"
        text: i18nc("@title:tab", "Movies")
        icon.name: "video-symbolic"
        visible: root.connection.movieLibraries.length > 0
        enabled: root.signedIn
        shortcut: root.tabShortcut(moviesAction)
        onTriggered: shell.openTab("movies")
    }
    Kirigami.Action {
        id: tvShowsAction
        objectName: "tvShowsTabAction"

        property string section: "tvshows"
        text: i18nc("@title:tab", "Shows")
        icon.name: "video-television"
        visible: root.connection.showLibraries.length > 0
        enabled: root.signedIn
        shortcut: root.tabShortcut(tvShowsAction)
        onTriggered: shell.openTab("tvshows")
    }
    Kirigami.Action {
        id: downloadsAction
        objectName: "downloadsTabAction"

        property string section: "downloads"
        text: i18nc("@title:tab", "Downloads")
        icon.name: "folder-download"
        visible: root.signedIn || root.offline
        enabled: root.signedIn || root.offline
        shortcut: root.tabShortcut(downloadsAction)
        onTriggered: shell.openDownloads()
    }
    Kirigami.Action {
        id: favoritesTabAction
        objectName: "favoritesTabAction"

        property string section: "favorites"
        text: i18nc("@title:tab", "Favorites")
        icon.name: "favorite"
        visible: root.signedIn
        enabled: root.signedIn
        shortcut: root.tabShortcut(favoritesTabAction)
        onTriggered: shell.openFavorites()
    }
    Kirigami.Action {
        id: playlistsTabAction
        objectName: "playlistsTabAction"

        property string section: "playlists"
        text: i18nc("@title:tab", "Playlists")
        icon.name: "view-media-playlist"
        visible: root.signedIn
        enabled: root.signedIn
        shortcut: root.tabShortcut(playlistsTabAction)
        onTriggered: shell.openPlaylists()
    }

    // PageUp/PageDown are the KDE tab navigation conventions. They work even when a detail page is on top; absent Movies/TV types are skipped
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
    // The one Refresh of the app: no page binds F5 itself, or the two shortcuts would be ambiguous. One key, not StandardKey.Refresh: the style's MenuItem binds a single sequence and warns on a standard key that has several
    Kirigami.Action {
        id: refreshAction
        objectName: "refreshAction"

        text: i18nc("@action:inmenu", "Refresh")
        icon.name: "view-refresh"
        shortcut: "F5"
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

        text: i18nc("@action:inmenu", "Sign Out")
        tooltip: i18nc("@info:tooltip", "Remove this user's saved session from this system")
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

    QQC2.Menu {
        id: mainMenu
        objectName: "mainMenu"
        // Under the button, right-aligned with it (left-aligned when mirrored), where the bar ends: a binding, since the menu only knows its width once its entries show
        parent: mainMenuButton
        x: Qt.application.layoutDirection === Qt.RightToLeft ? 0 : mainMenuButton.width - width
        y: mainMenuButton.height
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
        id: updateDialog
        objectName: "updateAvailableDialog"

        property string version: ""
        property url releaseUrl: ""

        title: i18nc("@title:window", "Update Available")
        standardButtons: Kirigami.Dialog.Close
        customFooterActions: [
            Kirigami.Action {
                text: i18nc("@action:button", "Open Release Page")
                icon.name: "internet-web-browser"
                onTriggered: {
                    updateDialog.close();
                    root.openReleasePage(updateDialog.releaseUrl);
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
        displayVersion: root.shell.displayVersion ?? ""
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
        tabActions: root.tabActions
        gamepad: root.shell.gamepad ?? null
    }
}
