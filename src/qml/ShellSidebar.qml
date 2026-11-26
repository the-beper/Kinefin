// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kirigamiaddons.components as Components

// The rail down the leading edge of the window, from its top to its bottom, with each label under its icon: Search, the sections of the shell (they scroll in a short window), then the queue, the SyncPlay group while one is joined and the user menu, which stay in reach. A Compact window has no rail: the header is shown, and its button opens a modal drawer with the same content as the rail (the sections, Search, the queue, the SyncPlay group and the user), each with its label beside the icon. The item lives in the root item of the window, beside the content item, not in it: the header is painted above the content item, and only a sibling can paint over the header's leading cell
Item {
    id: root
    objectName: "shellSidebar"

    // Up and Down on the page never hop into the rail beside it (the spatial navigation reads this): Left and Right reach it
    readonly property bool spatialSideRegion: true

    // The shell window: the current section, the modes and the geometry of the window
    required property var shell
    // The sections: their actions and the order they are shown in
    required property ShellNavigation navigation

    // Wide enough for the longest label in the rail's own font, never less than 4.5 grid units: the label of an entry is elided only if a translation still does not fit, and never wraps
    readonly property real railWidth: Math.max(Kirigami.Units.gridUnit * 4.5, Math.ceil(Math.max(sidebarEntries.widestLabel, searchEntry.labelWidth, queueEntry.labelWidth, groupEntry.labelWidth)) + Kirigami.Units.smallSpacing * 3 + 1)
    readonly property real drawerWidth: Kirigami.Units.gridUnit * 13
    // The room the page row leaves on the leading side: none in a Compact window and while there is nothing to choose from. It never animates: a resize of the page row ends the slide of a tab switch
    readonly property real inlineWidth: navigation.navigable && !shell.compactHeader ? railWidth : 0
    // A Compact window with sections to choose from has the modal drawer
    readonly property bool drawerAllowed: shell.compactHeader && navigation.navigable
    readonly property bool drawerOpen: navigationDrawer.visible
    readonly property bool mirrored: Qt.application.layoutDirection === Qt.RightToLeft
    // Downloads that are not finished (queued, running, paused, waiting or failed), as the Active Downloads list of the page counts them
    readonly property int activeDownloads: navigation.connection.downloads.model.activeCount
    // Where the header puts its user menu button while the rail shows: the last entry of the rail
    readonly property Item userSlot: userSlotLayout
    // Watching together, null when the application gives none; the group entry shows while one is joined
    readonly property var syncPlay: shell.syncPlay ?? null
    readonly property bool groupJoined: navigation.signedIn && shell.groupJoined
    // The signed-in user, as the header finds it among the saved users: the main menu and who is signed in belong to the header
    readonly property var currentUser: shell.header?.currentUser ?? null

    // The drawer closed without a choice: the keyboard goes back to the button that opened it
    signal drawerDismissed

    function openDrawer() {
        if (drawerAllowed)
            navigationDrawer.open();
    }

    // The count a section shows in its badge
    function badgeOf(section) {
        return section === "downloads" ? activeDownloads : 0;
    }

    // The name a screen reader and the tooltip give a section: the count is part of it, since the badge is not read on its own
    function entryName(section, text, badge) {
        if (section === "downloads" && badge > 0)
            return i18ncp("@info accessible name of the Downloads section, %1 is how many downloads are queued or running", "Downloads, %1 active", "Downloads, %1 active", badge);
        return text;
    }

    // Up and Down move between the entries of a list that show, as in a list
    function stepFocus(buttons, from, step) {
        for (let i = buttons.indexOf(from) + step; i >= 0 && i < buttons.length; i += step) {
            if (buttons[i].visible && buttons[i].enabled) {
                buttons[i].forceActiveFocus(Qt.TabFocusReason);
                return;
            }
        }
    }

    // Every entry of the rail, top to bottom
    function railButtons() {
        return [searchEntry].concat(sidebarEntries.buttons(), [queueEntry, groupEntry]);
    }

    // Every row of the drawer, top to bottom
    function drawerButtons() {
        return drawerEntries.buttons().concat([drawerSearch, drawerQueue, drawerGroup, drawerUser]);
    }

    // A row of the open drawer that went away (the group was left) cannot keep the keyboard: the row above takes it
    function releaseHiddenRow(row) {
        if (!row.visible && row.activeFocus && navigationDrawer.visible)
            stepFocus(drawerButtons(), row, -1);
    }

    function openQueue() {
        shell.openSection("queuePage", "QueuePage.qml", { connection: shell.connection });
    }

    // The main menu is the header's: it opens where the header's user button stands
    function openMainMenu() {
        shell.header?.openMainMenu();
    }

    onDrawerAllowedChanged: {
        if (!drawerAllowed)
            navigationDrawer.close();
    }

    // Beside the content item, above the header: both sit at z 1 with the header later, so a child of the content item can never paint over it, whatever its own z. Stacked before the content item it also comes before the pages in the Tab chain, which follows the order of the siblings
    parent: shell.contentItem.parent
    // Above the page row (z 1): a page pushed over another one leaves the first one drawn past the row's left edge, over this area
    z: 2
    x: mirrored ? (parent?.width ?? 0) - width : 0
    y: 0
    width: inlineWidth
    height: parent?.height ?? 0
    // The header's own rules: hidden under the start-up cover, faded and inert while the player page is in front; once faded out it is hidden too, since a disabled item still blocks the hover of the player's bar under it
    visible: inlineWidth > 0 && !shell.startupVisible && opacity > 0
    opacity: shell.headerOpacity
    enabled: !shell.playbackPageShowing
    Accessible.ignored: shell.playbackPageShowing

    // A count, on the corner of an icon in the rail or at the end of a row in the drawer
    component CountBadge: Rectangle {
        id: badge

        property int count
        property string overflowText: i18nc("@label more than 99 downloads in the badge of the sidebar", "99+")

        implicitHeight: Math.round(Kirigami.Units.gridUnit * 0.9)
        implicitWidth: Math.max(implicitHeight, badgeLabel.implicitWidth + Kirigami.Units.smallSpacing)
        radius: implicitHeight / 2
        color: Kirigami.Theme.highlightColor
        Accessible.ignored: true

        QQC2.Label {
            id: badgeLabel
            anchors.centerIn: parent
            font.pixelSize: Math.round(parent.height * 0.7)
            color: Kirigami.Theme.highlightedTextColor
            textFormat: Text.PlainText
            text: badge.count > 99 ? badge.overflowText : Number(badge.count).toLocaleString(Qt.locale(), "f", 0)
        }
    }

    // One entry: the icon over a small label in the rail, a row in the drawer; the current section is marked, and a count sits on the icon's corner
    component RailEntry: Kirigami.NavigationTabButton {
        id: tab

        // The name for the screen reader and the tooltip, which may say more than the label
        property string entryLabel: text
        property string iconName
        // The label beside the icon, as a row of the drawer; else under it
        property bool labelled: false
        // The section it opens is the one showing: only a mark, while `checked` is the selection a screen reader reads
        property bool current: false
        // A menu of the entry is open
        property bool opened: false
        property int badgeCount: 0
        property string overflowText: i18nc("@label more than 99 downloads in the badge of the sidebar", "99+")
        // A dot on the icon's corner, for a state that the accessible name says in words
        property bool dot: false
        property string dotObjectName
        // The icon shows less while the state is told in words
        property real iconOpacity: 1
        // A line of small text under the label of a row of the drawer: the state that the rail leaves to its tooltip
        property string stateText
        property string labelObjectName
        property string stateObjectName
        // The row opens a menu: the drawer ends it with an arrow
        property bool opensMenu: false
        property string chevronObjectName
        // The avatar of a user in the place of the icon
        property bool withAvatar: false
        property string avatarName
        property var avatarSource: ""
        property string badgeObjectName
        property string rowBadgeObjectName
        property string indicatorObjectName
        property int accessibleRole: Accessible.PageTab
        readonly property bool marked: checked || current
        readonly property color foreground: marked ? Kirigami.Theme.highlightColor : Kirigami.Theme.textColor
        readonly property real iconSize: Kirigami.Units.iconSizes.smallMedium
        // The rail uses the theme's small font, the rows of the drawer the default one
        readonly property font labelFont: labelled ? Kirigami.Theme.defaultFont : Kirigami.Theme.smallFont
        readonly property real labelWidth: labelMetrics.advanceWidth

        icon.name: iconName
        // A plain binding: the button is no member of a tab group and a click does not toggle it
        checkable: false
        // Not checkable, so the screen reader learns the current section from the selection
        Accessible.role: accessibleRole
        Accessible.selected: checked
        Accessible.name: entryLabel
        Keys.onReturnPressed: animateClick()
        Keys.onEnterPressed: animateClick()

        TextMetrics {
            id: labelMetrics
            font: Kirigami.Theme.smallFont
            text: tab.text
        }

        // The rail may elide a long label; the drawer shows it whole. The text can hold server text (a group name): plain. It follows the pointer only, and stands beside the entry so that it never covers the next one (a focused entry shows its label already)
        PlainToolTip {
            text: tab.entryLabel
            visible: !tab.labelled && tab.hovered && !tab.opened
            x: tab.mirrored ? -width - Kirigami.Units.smallSpacing : tab.width + Kirigami.Units.smallSpacing
            y: Math.round((tab.height - height) / 2)
        }

        // The entries never give up their height to a short window: it is the list of the sections that shrinks and scrolls
        Layout.minimumHeight: implicitHeight
        Layout.fillWidth: true
        Layout.leftMargin: Kirigami.Units.smallSpacing
        Layout.rightMargin: Kirigami.Units.smallSpacing

        leftPadding: labelled ? Kirigami.Units.largeSpacing : Kirigami.Units.smallSpacing / 2
        rightPadding: labelled ? Kirigami.Units.largeSpacing : Kirigami.Units.smallSpacing / 2
        // A row of the drawer keeps its height with a second line under the label, so it pads less
        topPadding: labelled ? Kirigami.Units.smallSpacing / 2 : Kirigami.Units.smallSpacing
        bottomPadding: topPadding
        leftInset: 0
        rightInset: 0
        topInset: 0
        bottomInset: 0
        spacing: Kirigami.Units.smallSpacing
        implicitWidth: implicitContentWidth + leftPadding + rightPadding
        implicitHeight: labelled ? Math.round(Kirigami.Units.gridUnit * 2.2) : implicitContentHeight + topPadding + bottomPadding

        contentItem: GridLayout {
            // Icon, label, badge and arrow side by side in the drawer; the icon over the label in the rail, where the badge sits on the icon
            columns: tab.labelled ? 4 : 1
            rowSpacing: Kirigami.Units.smallSpacing / 2
            columnSpacing: Kirigami.Units.largeSpacing

            Item {
                Layout.alignment: tab.labelled ? Qt.AlignVCenter | Qt.AlignLeft : Qt.AlignHCenter
                Layout.preferredWidth: tab.iconSize
                Layout.preferredHeight: tab.iconSize

                Kirigami.Icon {
                    anchors.fill: parent
                    visible: !tab.withAvatar
                    opacity: tab.iconOpacity
                    source: tab.icon.name
                    color: tab.foreground
                }
                Loader {
                    anchors.fill: parent
                    active: tab.withAvatar

                    sourceComponent: Components.Avatar {
                        objectName: tab.objectName + "Avatar"
                        anchors.fill: parent
                        name: tab.avatarName
                        // Only URLs built by JellyfinClient::userImageUrl belong here: a raw server string must never become a source
                        source: tab.avatarSource
                        sourceSize.width: width * Screen.devicePixelRatio
                        sourceSize.height: height * Screen.devicePixelRatio
                        Accessible.ignored: true
                    }
                }
                CountBadge {
                    objectName: tab.badgeObjectName
                    visible: !tab.labelled && tab.badgeCount > 0
                    count: tab.badgeCount
                    overflowText: tab.overflowText
                    x: parent.width - Math.round(width * 0.6)
                    y: -Math.round(height * 0.4)
                }
                Rectangle {
                    objectName: tab.dotObjectName
                    visible: tab.dot
                    x: parent.width - width / 2
                    width: Kirigami.Units.smallSpacing * 2
                    height: width
                    radius: width / 2
                    color: Kirigami.Theme.neutralTextColor
                    Accessible.ignored: true
                }
            }
            ColumnLayout {
                spacing: 0
                Layout.fillWidth: true

                QQC2.Label {
                    objectName: tab.labelObjectName
                    text: tab.text
                    color: tab.foreground
                    font: tab.labelFont
                    textFormat: Text.PlainText
                    wrapMode: Text.NoWrap
                    elide: Text.ElideRight
                    horizontalAlignment: tab.labelled ? Text.AlignLeft : Text.AlignHCenter
                    Layout.fillWidth: true
                }
                QQC2.Label {
                    objectName: tab.stateObjectName
                    visible: tab.labelled && tab.stateText.length > 0
                    text: tab.stateText
                    color: tab.foreground
                    font: Kirigami.Theme.smallFont
                    textFormat: Text.PlainText
                    wrapMode: Text.NoWrap
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }
            }
            CountBadge {
                objectName: tab.rowBadgeObjectName
                visible: tab.labelled && tab.badgeCount > 0
                count: tab.badgeCount
                overflowText: tab.overflowText
                Layout.alignment: Qt.AlignVCenter
            }
            Kirigami.Icon {
                objectName: tab.chevronObjectName
                visible: tab.labelled && tab.opensMenu
                source: root.mirrored ? "go-previous-symbolic" : "go-next-symbolic"
                color: tab.foreground
                Layout.alignment: Qt.AlignVCenter
                Layout.preferredWidth: Kirigami.Units.iconSizes.small
                Layout.preferredHeight: Kirigami.Units.iconSizes.small
                Accessible.ignored: true
            }
        }

        // The same geometry in every state: hover, focus and the current mark only add paint
        background: Item {
            Rectangle {
                anchors.fill: parent
                radius: Kirigami.Units.cornerRadius
                color: tab.down || tab.opened ? Qt.alpha(Kirigami.Theme.highlightColor, 0.3)
                    : tab.marked ? Qt.alpha(Kirigami.Theme.highlightColor, 0.18)
                    : tab.hovered ? Qt.alpha(Kirigami.Theme.highlightColor, 0.12) : "transparent"
                border.width: tab.visualFocus ? 1 : 0
                border.color: Kirigami.Theme.highlightColor
            }
            Rectangle {
                objectName: tab.indicatorObjectName
                visible: tab.marked
                anchors.left: parent.left
                anchors.verticalCenter: parent.verticalCenter
                width: Math.max(2, Math.round(Kirigami.Units.smallSpacing * 0.75))
                height: Math.round(parent.height * 0.6)
                radius: width / 2
                color: Kirigami.Theme.highlightColor
            }
        }
    }

    // The line between two groups of the rail: under Search and before the personal group; in the drawer, before Search
    component GroupDivider: Kirigami.Separator {
        Layout.fillWidth: true
        Layout.leftMargin: Kirigami.Units.largeSpacing
        Layout.rightMargin: Kirigami.Units.largeSpacing
        Layout.topMargin: Kirigami.Units.smallSpacing
        Layout.bottomMargin: Kirigami.Units.smallSpacing
    }

    // A list that scrolls in a window too short for all of it: the bar says that more lies below and is the pointer's way to it besides the wheel, a notch of the wheel moves about an entry (a bare flickable moves a few pixels), and the keyboard brings the entry it moved to into view
    component ScrollingList: Flickable {
        id: view

        // Tells the objectNames of the rail's list from the drawer's
        property string namePrefix: "sidebar"

        function reveal(item) {
            const top = item.mapToItem(contentItem, 0, 0).y;
            if (top < contentY)
                contentY = top;
            else if (top + item.height > contentY + height)
                contentY = top + item.height - height;
        }

        objectName: namePrefix + "EntriesView"
        Layout.fillWidth: true
        Layout.fillHeight: true
        Layout.minimumHeight: 0
        contentWidth: width
        clip: true
        boundsBehavior: Flickable.StopAtBounds

        // The room the bar takes along the end side while it shows: the entries are as wide as what is left, so the bar never lies over the highlight of one
        readonly property real barInset: contentHeight > height ? bar.implicitWidth : 0
        readonly property bool mirrored: Qt.application.layoutDirection === Qt.RightToLeft

        QQC2.ScrollBar.vertical: QQC2.ScrollBar {
            id: bar
            objectName: view.namePrefix + "ScrollBar"
            policy: QQC2.ScrollBar.AsNeeded
        }

        Kirigami.WheelHandler {
            target: view
            filterMouseEvents: true
            keyNavigationEnabled: false
            verticalStepSize: Kirigami.Units.gridUnit * 3
        }
    }

    // The search popup, as Ctrl+F opens it
    component SearchEntry: RailEntry {
        text: i18nc("@action:button", "Search")
        iconName: "search"
        current: root.shell.currentSection === "search"
        accessibleRole: Accessible.Button
    }

    component QueueEntry: RailEntry {
        readonly property int queued: root.navigation.signedIn ? root.shell.connection.queue.count : 0

        visible: root.navigation.signedIn
        text: i18nc("@action:button", "Queue")
        // Not the icon of the Playlists tab
        iconName: "media-track-queue-amarok"
        entryLabel: queued > 0 ? i18ncp("@info:tooltip the queue button and how many items the queue holds", "Queue, %1 item", "Queue, %1 items", queued) : text
        badgeCount: queued
        overflowText: i18nc("@label more than 99 items in the queue badge", "99+")
        current: root.shell.currentSection === "queue"
        accessibleRole: Accessible.Button
    }

    // While a SyncPlay group is joined: whether the playback keeps in step with it and whether it waits for someone; the menu of the group belongs to the header
    component GroupEntry: RailEntry {
        readonly property var syncPlay: root.syncPlay
        readonly property bool joined: root.groupJoined
        // The name of the group is server text: a plain text tooltip, and the accessible name says the same
        readonly property string summary: {
            if (!joined)
                return "";
            const name = syncPlay.groupName.length > 0 ? syncPlay.groupName : i18n("Unnamed group");
            const parts = [i18ncp("@info:tooltip %2 is the name of the group", "SyncPlay group %2: %1 person", "SyncPlay group %2: %1 people", syncPlay.participants.length, name)];
            if (stateText.length > 0)
                parts.push(stateText);
            return parts.join(" · ");
        }

        visible: joined
        // The drawer names the group as the main menu does; the rail has room for a short label only
        text: labelled ? i18nc("@action:button the SyncPlay group row of the navigation drawer, which opens the menu of the group", "SyncPlay Group") : i18nc("@action:button short label under the icon of the SyncPlay group button", "Group")
        iconName: "system-users-symbolic"
        entryLabel: summary
        opened: root.shell.groupMenuOpened
        // Not following or waiting: the drawer row says it under its label, the rail in its tooltip
        stateText: {
            if (!joined)
                return "";
            if (!syncPlay.following)
                return i18nc("@info:tooltip the playback does not keep in step with the group", "Not following");
            if (syncPlay.waiting)
                return i18nc("@info:tooltip the group waits for someone", "Waiting for others");
            return "";
        }
        // Not following is the state to see: the icon dims, the label does not
        iconOpacity: syncPlay?.following ? 1 : 0.55
        // The group waits for someone: the words say it too
        dot: (syncPlay?.following ?? false) && (syncPlay?.waiting ?? false)
        opensMenu: true
        accessibleRole: Accessible.ButtonMenu
    }

    // The sections, as the rail or as the rows of the drawer; the divider and, in the drawer, the caption stand before the personal group
    component Entries: ColumnLayout {
        id: list

        // The label beside the icon, as the drawer shows it; else under it, as the rail does
        property bool labelled: false
        // Tells the objectNames of the sidebar's entries from the modal drawer's
        property string namePrefix: "sidebar"
        readonly property var leading: root.navigation.entries.filter(entry => entry.group !== "personal")
        readonly property var personal: root.navigation.entries.filter(entry => entry.group === "personal")
        // The width of the longest label of the rail, whether its entry shows or not
        readonly property real widestLabel: {
            let widest = 0;
            for (const repeater of [leadingRepeater, personalRepeater]) {
                for (let i = 0; i < repeater.count; ++i)
                    widest = Math.max(widest, repeater.itemAt(i)?.labelWidth ?? 0);
            }
            return widest;
        }

        // A section was chosen with the pointer or the keyboard
        signal chosen
        // Up or Down on an entry: the owner of the list decides which entry is next
        signal moveRequested(var from, int step)
        // An entry took the keyboard, so a scrolling list can bring it into view
        signal entryFocused(var entry)

        function buttons() {
            const all = [];
            for (const repeater of [leadingRepeater, personalRepeater]) {
                for (let i = 0; i < repeater.count; ++i)
                    all.push(repeater.itemAt(i));
            }
            return all;
        }

        // The keyboard goes to the current section, or to the first one that shows
        function focusCurrent() {
            const all = buttons().filter(button => button.visible && button.enabled);
            (all.find(button => button.checked) ?? all[0])?.forceActiveFocus(Qt.TabFocusReason);
        }

        spacing: Kirigami.Units.smallSpacing / 2
        // The entries report the page tab role: this column is the list they belong to
        Accessible.role: Accessible.PageTabList

        Repeater {
            id: leadingRepeater
            model: list.leading
            delegate: entryDelegate
        }

        GroupDivider {
            objectName: list.namePrefix + "Divider"
            visible: root.navigation.groupsDivided
        }

        // The rail has no room for it: its divider stands for the group
        QQC2.Label {
            objectName: list.namePrefix + "PersonalCaption"
            visible: list.labelled && root.navigation.groupsDivided
            text: i18nc("@title:group the sidebar group with Favorites, Playlists and Downloads", "Personal")
            font: Kirigami.Theme.smallFont
            color: Kirigami.Theme.disabledTextColor
            textFormat: Text.PlainText
            Layout.fillWidth: true
            Layout.leftMargin: Kirigami.Units.largeSpacing + Kirigami.Units.smallSpacing
            Layout.rightMargin: Kirigami.Units.largeSpacing
            Layout.bottomMargin: Kirigami.Units.smallSpacing
        }

        Repeater {
            id: personalRepeater
            model: list.personal
            delegate: entryDelegate
        }

        Component {
            id: entryDelegate

            RailEntry {
                id: entry

                required property var modelData

                readonly property Kirigami.Action entryAction: modelData.action
                readonly property string section: modelData.section

                objectName: list.namePrefix + "Entry_" + section
                badgeObjectName: list.namePrefix + "Badge_" + section
                rowBadgeObjectName: list.namePrefix + "RowBadge_" + section
                indicatorObjectName: list.namePrefix + "Indicator"
                labelled: list.labelled
                text: entryAction.text
                iconName: entryAction.icon.name
                badgeCount: root.badgeOf(section)
                entryLabel: root.entryName(section, entryAction.text, badgeCount)
                visible: entryAction.visible
                enabled: entryAction.enabled
                checked: root.shell.currentSection === section
                onClicked: {
                    entryAction.trigger();
                    list.chosen();
                }
                Keys.onUpPressed: list.moveRequested(entry, -1)
                Keys.onDownPressed: list.moveRequested(entry, 1)
                onActiveFocusChanged: {
                    if (activeFocus)
                        list.entryFocused(entry);
                }
            }
        }
    }

    Rectangle {
        anchors.fill: parent
        color: Kirigami.Theme.backgroundColor

        Kirigami.Separator {
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.right: parent.right
            Accessible.ignored: true
        }

        ColumnLayout {
            anchors.fill: parent
            // The line on the trailing edge
            anchors.rightMargin: 1
            anchors.topMargin: Kirigami.Units.smallSpacing
            anchors.bottomMargin: Kirigami.Units.smallSpacing
            spacing: Kirigami.Units.smallSpacing / 2

            SearchEntry {
                id: searchEntry
                objectName: "sidebarSearch"
                onClicked: root.shell.openSearch()
                Keys.onUpPressed: root.stepFocus(root.railButtons(), searchEntry, -1)
                Keys.onDownPressed: root.stepFocus(root.railButtons(), searchEntry, 1)
            }

            // Search is no section: the line sets it apart from Home and the others
            GroupDivider {
                objectName: "sidebarSearchDivider"
            }

            // A window as short as the minimum with a large font cannot show every entry: the column scrolls, and the entries below it stay in reach
            ScrollingList {
                id: entriesView
                contentHeight: sidebarEntries.implicitHeight

                Entries {
                    id: sidebarEntries
                    x: entriesView.mirrored ? entriesView.barInset : 0
                    width: entriesView.width - entriesView.barInset
                    onMoveRequested: (from, step) => root.stepFocus(root.railButtons(), from, step)
                    onEntryFocused: entry => entriesView.reveal(entry)
                }
            }

            // The queue, the group and the user menu: what stays in reach whatever the height of the window, below the entries that scroll
            ColumnLayout {
                id: bottomGroup
                Layout.fillWidth: true
                spacing: Kirigami.Units.smallSpacing / 2

                QueueEntry {
                    id: queueEntry
                    objectName: "queueButton"
                    badgeObjectName: "queueCountBadge"
                    onClicked: root.openQueue()
                    Keys.onUpPressed: root.stepFocus(root.railButtons(), queueEntry, -1)
                    Keys.onDownPressed: root.stepFocus(root.railButtons(), queueEntry, 1)
                }

                GroupEntry {
                    id: groupEntry
                    objectName: "syncPlayGroupButton"
                    dotObjectName: "syncPlayWaitingBadge"
                    onClicked: root.shell.openGroupMenu(groupEntry)
                    Keys.onUpPressed: root.stepFocus(root.railButtons(), groupEntry, -1)
                    Keys.onDownPressed: root.stepFocus(root.railButtons(), groupEntry, 1)
                }

                // The header's menu button moves in here while the rail shows: the avatar of the user, with no label
                ColumnLayout {
                    id: userSlotLayout
                    Layout.fillWidth: true
                    Layout.minimumHeight: implicitHeight
                    spacing: 0
                }
            }
        }
    }

    // What the rail holds, as rows with the labels beside the icons, for a Compact window: it opens from the header's button, closes on Escape, on a click outside and on a choice; the modal blocks the window's shortcuts while it is open
    QQC2.Drawer {
        id: navigationDrawer
        objectName: "navigationDrawer"

        // Set by a choice, which hands the keyboard to the page it opens rather than back to the button
        property bool chosen: false
        // What a row that opens a popup of its own runs once the drawer has gone: over the fading drawer the popup would lose the keyboard to it
        property var afterClose: null

        // A section or the queue was chosen: the page it opened takes the keyboard
        function choose() {
            chosen = true;
            close();
        }

        // The keyboard goes back to the button that opened the drawer, then `action` opens its popup
        function closeThen(action) {
            afterClose = action;
            close();
        }

        parent: QQC2.Overlay.overlay
        edge: Qt.LeftEdge
        modal: true
        focus: true
        // No swipe from the window edge: the header's button is the one way in
        dragMargin: 0
        width: Math.min(root.drawerWidth, root.shell.width - Kirigami.Units.gridUnit * 2)
        height: parent ? parent.height : root.shell.height
        // The rail's own margins at the top and the foot
        topPadding: Kirigami.Units.smallSpacing
        bottomPadding: Kirigami.Units.smallSpacing

        onAboutToShow: chosen = false
        onOpened: drawerEntries.focusCurrent()
        onClosed: {
            const next = afterClose;
            afterClose = null;
            if (chosen)
                Qt.callLater(root.shell.focusCurrentPage);
            else
                root.drawerDismissed();
            if (next)
                Qt.callLater(next);
        }

        background: Rectangle {
            color: Kirigami.Theme.backgroundColor

            Kirigami.Separator {
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                anchors.right: parent.right
            }
        }

        contentItem: ColumnLayout {
            spacing: Kirigami.Units.smallSpacing / 2

            // The sections scroll in a window too short for all of them, as the rail's do
            ScrollingList {
                id: drawerView
                namePrefix: "drawer"
                contentHeight: drawerEntries.implicitHeight

                Entries {
                    id: drawerEntries
                    x: drawerView.mirrored ? drawerView.barInset : 0
                    width: drawerView.width - drawerView.barInset
                    labelled: true
                    namePrefix: "drawer"
                    onMoveRequested: (from, step) => root.stepFocus(root.drawerButtons(), from, step)
                    onEntryFocused: entry => drawerView.reveal(entry)
                    onChosen: navigationDrawer.choose()
                }
            }

            GroupDivider {
                objectName: "drawerActionsDivider"
            }

            // Search, the queue, the group and the user, in the order of the rail and in reach whatever the height of the window
            ColumnLayout {
                Layout.fillWidth: true
                spacing: Kirigami.Units.smallSpacing / 2

                SearchEntry {
                    id: drawerSearch
                    objectName: "drawerSearch"
                    labelled: true
                    onClicked: navigationDrawer.closeThen(() => root.shell.openSearch())
                    Keys.onUpPressed: root.stepFocus(root.drawerButtons(), drawerSearch, -1)
                    Keys.onDownPressed: root.stepFocus(root.drawerButtons(), drawerSearch, 1)
                }

                QueueEntry {
                    id: drawerQueue
                    objectName: "drawerQueue"
                    rowBadgeObjectName: "drawerQueueBadge"
                    labelled: true
                    onClicked: {
                        root.openQueue();
                        navigationDrawer.choose();
                    }
                    Keys.onUpPressed: root.stepFocus(root.drawerButtons(), drawerQueue, -1)
                    Keys.onDownPressed: root.stepFocus(root.drawerButtons(), drawerQueue, 1)
                }

                // The menu of the group hangs from the row, and the drawer stays open under it
                GroupEntry {
                    id: drawerGroup
                    objectName: "drawerSyncPlayGroup"
                    dotObjectName: "drawerSyncPlayWaitingBadge"
                    labelObjectName: "drawerSyncPlayGroupLabel"
                    stateObjectName: "drawerSyncPlayGroupState"
                    chevronObjectName: "drawerSyncPlayGroupChevron"
                    labelled: true
                    onClicked: root.shell.openGroupMenu(drawerGroup)
                    // The menu that closes after Leave Group may hand the keyboard back to a row that has already gone
                    onVisibleChanged: root.releaseHiddenRow(drawerGroup)
                    onActiveFocusChanged: root.releaseHiddenRow(drawerGroup)
                    Keys.onUpPressed: root.stepFocus(root.drawerButtons(), drawerGroup, -1)
                    Keys.onDownPressed: root.stepFocus(root.drawerButtons(), drawerGroup, 1)
                }

                // The main menu is the header's: the drawer goes first, and the menu opens under the header's button
                RailEntry {
                    id: drawerUser
                    objectName: "drawerUser"

                    readonly property string menuLabel: i18nc("@action:button", "Main Menu")

                    chevronObjectName: "drawerUserChevron"
                    labelled: true
                    opensMenu: true
                    withAvatar: root.navigation.signedIn
                    avatarName: root.currentUser?.name ?? ""
                    // Only URLs built by JellyfinClient::userImageUrl belong here: a raw server string must never become a source
                    avatarSource: root.currentUser?.imageUrl ?? ""
                    text: root.currentUser?.name ?? menuLabel
                    iconName: "view-more-symbolic"
                    entryLabel: root.currentUser ? i18nc("@label %1 is a user name", "Main Menu, signed in as %1", root.currentUser.name) : menuLabel
                    current: root.shell.currentSection === "settings"
                    accessibleRole: Accessible.ButtonMenu
                    onClicked: navigationDrawer.closeThen(() => root.openMainMenu())
                    Keys.onUpPressed: root.stepFocus(root.drawerButtons(), drawerUser, -1)
                    Keys.onDownPressed: root.stepFocus(root.drawerButtons(), drawerUser, 1)
                }
            }
        }
    }
}
