// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Kirigami.ScrollablePage {
    id: page
    objectName: "queuePage"

    required property ConnectionController connection

    readonly property PlayQueue queue: connection.queue

    title: i18n("Queue")
    // No title bar, as in the Movies and Shows tabs: the actions sit in a toolbar on top of the list, the header's queue button says which page this is
    globalToolBarStyle: Kirigami.ApplicationHeaderStyle.None

    header: RowLayout {
        spacing: Kirigami.Units.mediumSpacing

        // A Compact window hides the header's queue button, which otherwise names the page
        Kirigami.Heading {
            id: queueHeading
            objectName: "queueHeading"
            Layout.leftMargin: Kirigami.Units.largeSpacing
            visible: page.Window.window?.compactHeader ?? false
            level: 3
            text: page.title
        }

        Kirigami.ActionToolBar {
            objectName: "queueToolbar"
            Layout.fillWidth: true
            Layout.leftMargin: queueHeading.visible ? 0 : Kirigami.Units.largeSpacing
            Layout.rightMargin: Kirigami.Units.largeSpacing
            actions: page.actions
        }
    }

    // The list takes the keys as soon as the page shows
    function focusPrimary() {
        list.forceActiveFocus(Qt.TabFocusReason);
    }

    // The row follows the item it moved
    // The menu acts on the row it was opened for, as long as that row still holds the same item
    function menuRowIsCurrent() {
        return menu.row >= 0 && list.itemAtIndex(menu.row)?.itemId === menu.itemId;
    }

    function moveRow(from, delta) {
        const to = from + delta;
        if (from < 0 || to < 0 || to >= queue.count)
            return;
        if (queue.move(from, to))
            list.currentIndex = to;
    }

    // The current row stays at the same place, so the next item takes its spot
    function removeRow(row) {
        if (row < 0 || row >= queue.count)
            return;
        queue.remove(row);
        list.currentIndex = queue.count > 0 ? Math.min(row, queue.count - 1) : -1;
    }

    function undoLastEdit() {
        const row = list.currentIndex;
        if (!queue.undo())
            return;
        list.currentIndex = queue.count > 0 ? Math.max(0, Math.min(row, queue.count - 1)) : -1;
        list.forceActiveFocus(Qt.TabFocusReason);
    }

    Shortcut {
        sequences: [StandardKey.Undo]
        enabled: page.Window.window?.pageStack.currentItem === page
                 && page.queue.canUndo && !page.queue.busy && !clearQueueDialog.visible
        onActivated: page.undoLastEdit()
    }

    function openRow(itemId) {
        page.Window.window.openItem(itemId, page);
    }

    function openMenu(row, anchorItem, x, y) {
        menu.row = row;
        menu.itemId = anchorItem.itemId;
        menu.popup(anchorItem, x, y);
    }

    // One queue entry: the image, the title, the year and runtime, a check when played. The values come from the model roles
    component QueueRow: Controls.ItemDelegate {
        id: row
        objectName: "queueRow_" + index

        required property int index
        required property string itemId
        required property string name
        required property string seriesName
        required property int seasonNumber
        required property int episodeNumber
        required property int year
        required property int runtimeMinutes
        required property url imageUrl
        required property bool played

        signal opened
        signal menuRequested(real x, real y)

        readonly property real thumbnailWidth: Kirigami.Units.gridUnit * 8
        readonly property string title: {
            if (episodeNumber < 0)
                return name;
            const label = seasonNumber >= 0 ? i18nc("season and episode number", "S%1:E%2", seasonNumber, episodeNumber) : i18nc("episode number", "E%1", episodeNumber);
            const episode = name.length > 0 ? i18nc("episode label and episode title", "%1 — %2", label, name) : label;
            return seriesName.length > 0 ? i18nc("series name and episode", "%1 — %2", seriesName, episode) : episode;
        }
        readonly property string details: {
            const parts = [];
            if (year > 0)
                parts.push(String(year));
            if (runtimeMinutes > 0)
                parts.push(i18n("%1 min", runtimeMinutes));
            return parts.join(i18nc("separator between items of a list", ", "));
        }

        // The row spans the list and its visible part is capped at the shell's reading width and centred: a ListView resets the x of its delegates, so the side margins are insets
        readonly property real sideMargin: Math.max(0, (ListView.view.width - (ListView.view.Window.window?.readingWidth ?? ListView.view.width)) / 2)
        width: ListView.view.width
        leftInset: sideMargin
        rightInset: sideMargin
        leftPadding: sideMargin + Kirigami.Units.smallSpacing
        rightPadding: sideMargin + Kirigami.Units.smallSpacing
        containmentMask: QtObject {
            function contains(point: point): bool {
                return point.x >= row.sideMargin && point.x <= row.width - row.sideMargin;
            }
        }
        // The list is the one tab stop, the arrow keys move inside it
        focusPolicy: Qt.NoFocus
        highlighted: ListView.isCurrentItem && ListView.view.activeFocus
        padding: Kirigami.Units.smallSpacing

        // A click makes this row the current one and gives the list the focus, so the focus comes back to this row after Back
        onPressed: {
            ListView.view.currentIndex = index;
            ListView.view.forceActiveFocus(Qt.MouseFocusReason);
        }
        onClicked: opened()

        TapHandler {
            acceptedButtons: Qt.RightButton
            onTapped: eventPoint => {
                row.ListView.view.currentIndex = row.index;
                row.ListView.view.forceActiveFocus(Qt.MouseFocusReason);
                row.menuRequested(eventPoint.position.x, eventPoint.position.y);
            }
        }

        Accessible.role: Accessible.ListItem
        Accessible.name: i18n("%1, %2 of %3", title, index + 1, ListView.view.count)
        Accessible.description: played ? i18nc("state of an item", "Played") : ""

        contentItem: RowLayout {
            spacing: Kirigami.Units.largeSpacing

            Rectangle {
                Layout.preferredWidth: row.thumbnailWidth
                Layout.preferredHeight: row.thumbnailWidth * 9 / 16
                Layout.alignment: Qt.AlignVCenter
                color: Kirigami.Theme.alternateBackgroundColor
                clip: true

                Kirigami.Icon {
                    anchors.centerIn: parent
                    width: Kirigami.Units.iconSizes.large
                    height: width
                    source: "video-x-generic"
                    visible: image.status !== Image.Ready
                }

                Image {
                    id: image
                    anchors.fill: parent
                    asynchronous: true
                    fillMode: Image.PreserveAspectCrop
                    sourceSize.width: width * Screen.devicePixelRatio
                    // Only URLs built by JellyfinClient::imageUrl belong here: the image fetcher allows nothing else, but a raw server string must never become a source
                    source: row.imageUrl
                    Accessible.ignored: true
                }
            }

            // Item and series names come from the server: plain text only
            ColumnLayout {
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignVCenter
                spacing: Kirigami.Units.smallSpacing

                Controls.Label {
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                    maximumLineCount: 1
                    font.bold: row.ListView.isCurrentItem
                    textFormat: Text.PlainText
                    text: row.title
                }

                Controls.Label {
                    Layout.fillWidth: true
                    visible: text.length > 0
                    elide: Text.ElideRight
                    maximumLineCount: 1
                    opacity: 0.7
                    textFormat: Text.PlainText
                    text: row.details
                }
            }

            Kirigami.Icon {
                Layout.preferredWidth: Kirigami.Units.iconSizes.smallMedium
                Layout.preferredHeight: Kirigami.Units.iconSizes.smallMedium
                Layout.alignment: Qt.AlignVCenter
                visible: row.played
                source: "checkmark-symbolic"
                Accessible.ignored: true
            }
        }
    }

    actions: [
        Kirigami.Action {
            objectName: "resumeQueueAction"
            text: i18n("Resume Queue")
            icon.name: "media-playlist-play"
            enabled: page.queue.count > 0 && !page.queue.busy && !!page.Window.window?.player
            onTriggered: page.Window.window.resumeQueue()
        },
        Kirigami.Action {
            objectName: "playQueueAction"
            text: i18n("Play From Here")
            icon.name: "media-playback-start"
            enabled: page.queue.count > 0 && !page.queue.busy && !!page.Window.window?.player
            onTriggered: page.Window.window.playQueue(Math.max(0, list.currentIndex))
        },
        Kirigami.Action {
            objectName: "undoQueueAction"
            text: i18n("Undo")
            icon.name: "edit-undo"
            enabled: page.queue.canUndo && !page.queue.busy
            onTriggered: page.undoLastEdit()
        },
        Kirigami.Action {
            text: i18n("Clear Queue")
            icon.name: "edit-clear-all"
            enabled: page.queue.count > 0 && !page.queue.busy
            onTriggered: clearQueueDialog.open()
        }
    ]

    Kirigami.PromptDialog {
        id: clearQueueDialog
        objectName: "clearQueueDialog"
        title: i18n("Clear Queue")
        subtitle: i18n("Remove all items from the queue?")
        standardButtons: Kirigami.Dialog.Cancel
        customFooterActions: [
            Kirigami.Action {
                text: i18n("Clear Queue")
                icon.name: "edit-clear-all"
                onTriggered: {
                    page.queue.clear();
                    clearQueueDialog.close();
                    page.focusPrimary();
                }
            }
        ]
    }

    // Rows that move or go while a menu is open make it stale: it closes
    Connections {
        target: page.queue
        function onEdited() {
            menu.close();
        }
    }

    // The menu acts on the row it was opened for, and never shows server text
    Controls.Menu {
        id: menu
        objectName: "queueMenu"

        property int row: -1
        property string itemId: ""

        Controls.MenuItem {
            text: i18n("Play From Here")
            icon.name: "media-playback-start"
            enabled: !!page.Window.window?.player
            onTriggered: if (page.menuRowIsCurrent()) page.Window.window.playQueue(menu.row)
        }
        Controls.MenuItem {
            text: i18n("Move Up")
            icon.name: "arrow-up"
            enabled: menu.row > 0
            onTriggered: if (page.menuRowIsCurrent()) page.moveRow(menu.row, -1)
        }
        Controls.MenuItem {
            text: i18n("Move Down")
            icon.name: "arrow-down"
            enabled: menu.row >= 0 && menu.row < page.queue.count - 1
            onTriggered: if (page.menuRowIsCurrent()) page.moveRow(menu.row, 1)
        }
        Controls.MenuItem {
            text: i18n("Remove")
            icon.name: "edit-delete-remove"
            onTriggered: if (page.menuRowIsCurrent()) page.removeRow(menu.row)
        }
        Controls.MenuSeparator {}
        Controls.MenuItem {
            text: i18n("Go to Item")
            icon.name: "go-next"
            onTriggered: page.openRow(menu.itemId)
        }
    }

    ListView {
        id: list
        objectName: "queueList"

        // On the view, not in its content: the fade stays under the toolbar while the rows scroll
        TopFade {
            objectName: "queueTopFade"
            parent: list
            z: 1
            atStart: list.atYBeginning
        }

        model: page.queue
        activeFocusOnTab: true
        keyNavigationEnabled: true
        currentIndex: 0
        Accessible.role: Accessible.List
        Accessible.name: page.title

        // The rows are not tab stops: the list is, and it opens the current item
        function openCurrent() {
            if (list.currentItem)
                page.openRow(list.currentItem.itemId);
        }
        function openCurrentMenu() {
            const item = list.currentItem;
            if (item)
                page.openMenu(list.currentIndex, item, item.sideMargin, item.height);
        }
        Keys.onReturnPressed: openCurrent()
        Keys.onEnterPressed: openCurrent()
        Keys.onSpacePressed: openCurrent()
        Keys.onMenuPressed: openCurrentMenu()
        Keys.onDeletePressed: page.removeRow(list.currentIndex)
        Keys.onPressed: event => {
            if (event.modifiers === Qt.ControlModifier && event.key === Qt.Key_Up) {
                page.moveRow(list.currentIndex, -1);
                event.accepted = true;
            } else if (event.modifiers === Qt.ControlModifier && event.key === Qt.Key_Down) {
                page.moveRow(list.currentIndex, 1);
                event.accepted = true;
            } else if (event.modifiers === Qt.ShiftModifier && event.key === Qt.Key_F10) {
                openCurrentMenu();
                event.accepted = true;
            }
        }

        // While an expansion runs on top of rows, a strip above them shows it: the rows stay usable
        header: Item {
            width: list.width
            height: visible ? Kirigami.Units.gridUnit * 3 : 0
            visible: page.queue.busy && list.count > 0

            RowLayout {
                anchors.centerIn: parent
                spacing: Kirigami.Units.largeSpacing

                Controls.BusyIndicator {
                    running: parent.visible
                    Accessible.ignored: true
                }

                Controls.Label {
                    textFormat: Text.PlainText
                    text: i18n("Loading…")
                }
            }
        }

        Kirigami.LoadingPlaceholder {
            anchors.centerIn: parent
            visible: page.queue.busy && list.count === 0
            text: i18n("Loading…")
        }

        MessageBox {
            x: Kirigami.Units.largeSpacing
            y: Kirigami.Units.largeSpacing
            width: list.width - Kirigami.Units.largeSpacing * 2
            visible: page.queue.restoreFailed
            type: Kirigami.MessageType.Error
            text: i18n("Could not restore the queue: check the connection to the server and try again")
            actions: [
                Kirigami.Action {
                    text: i18n("Retry")
                    icon.name: "view-refresh"
                    onTriggered: page.queue.retryRestore()
                }
            ]
        }

        Kirigami.PlaceholderMessage {
            anchors.centerIn: parent
            width: parent.width - Kirigami.Units.gridUnit * 4
            visible: page.queue.count === 0 && !page.queue.busy && !page.queue.restoreFailed
            icon.name: "view-media-playlist"
            text: i18n("The queue is empty")
            explanation: i18n("Add a movie, an episode, a season or a series with Add to Queue")
        }

        delegate: QueueRow {
            id: queueRow
            onOpened: page.openRow(itemId)
            onMenuRequested: (x, y) => page.openMenu(index, queueRow, x, y)
        }
    }
}
