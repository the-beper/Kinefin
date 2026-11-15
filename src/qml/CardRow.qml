// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// A horizontal row of cards that scrolls sideways and leaves the vertical wheel to the page: the delegate sets its own width and takes the highlight from `ListView.isCurrentItem && ListView.view.activeFocus`
Item {
    id: root

    required property var model
    required property Component delegate
    // The height of the cards: the row adds the band the horizontal scroll bar takes
    required property real cardHeight
    required property string accessibleName
    // The flickable of the page the row sits in, which receives the vertical wheel
    required property Flickable pageFlickable
    // The object name of the inner list, for the tests
    property string listObjectName: ""
    // A page that builds its rows lazily moves between them itself: Up and Down then emit the signals below instead of moving the focus
    property bool externalVerticalNavigation: false
    // The room left before the first and after the last card: zero in a page that pads its content, Kirigami.Units.largeSpacing where the row sits against the window edge, so the focus frame is never cut
    property real edgeMargin: 0

    signal upRequested
    signal downRequested

    // The cards the model holds: most item models count them, the people of a details page are a plain list; -1 for a model that does not say, whose list is built at once
    readonly property int itemCount: model ? (model.count ?? model.length ?? -1) : 0
    // The list comes once the model has cards, and stays: built asynchronously, its visible cards arrive a few per frame (a view created in an asynchronous incubation creates its delegates the same way), where cards reaching a list that already exists would build them all in one frame
    property bool listKept: false
    onItemCountChanged: {
        if (itemCount !== 0)
            listKept = true;
    }
    Component.onCompleted: listKept = listKept || itemCount !== 0
    readonly property ListView list: loader.item

    Layout.fillWidth: true
    // The scroll bar is built with the row, the list later: the row has its height from the start
    implicitHeight: cardHeight + scrollBar.implicitHeight

    function focusList(reason) {
        if (list) {
            list.forceActiveFocus(reason);
            return;
        }
        // A row without cards has no list coming: nothing waits for it
        if (itemCount === 0)
            return;
        focusPending = true;
        pendingReason = reason ?? Qt.OtherFocusReason;
        focusBefore = Window.activeFocusItem;
    }

    // Asked before the list exists: the list takes the keys when it comes, unless the user took the focus out of the row's page meanwhile (Kirigami moves it inside the page by itself)
    property bool focusPending: false
    property int pendingReason: Qt.OtherFocusReason
    property var focusBefore: null

    function focusLeftPage() {
        const focused = Window.activeFocusItem;
        if (!focused || focused === focusBefore)
            return false;
        let page = root.parent;
        while (page && page.isCurrentPage === undefined)
            page = page.parent;
        for (let item = focused; item; item = item.parent) {
            if (item === page)
                return false;
        }
        return true;
    }

    onListChanged: {
        if (!list)
            return;
        if (focusPending && !focusLeftPage())
            list.forceActiveFocus(pendingReason);
        focusPending = false;
        focusBefore = null;
    }

    // The next row of cards above or below along the focus chain, which also lists containers and other controls; no row there, no move
    function focusNeighbour(forward) {
        if (!list)
            return;
        let item = list;
        // The chain returns to this list; the bound only guards a chain that never does
        for (let step = 0; step < 1000; ++step) {
            item = item.nextItemInFocusChain(forward);
            if (!item || item === list)
                return;
            // The chain wraps around: a row above is not the answer to Down
            if (item.visible && item.enabled && item.isCardRowList === true && (item.mapToItem(null, 0, 0).y - list.mapToItem(null, 0, 0).y) * (forward ? 1 : -1) > 0) {
                item.forceActiveFocus(forward ? Qt.TabFocusReason : Qt.BacktabFocusReason);
                item.cardRow.scrollIntoView();
                return;
            }
        }
    }

    // The page scrolls so the whole row shows
    function scrollIntoView() {
        const flick = root.pageFlickable;
        const top = root.mapToItem(flick.contentItem, 0, 0).y;
        const margin = Kirigami.Units.largeSpacing;
        if (top < flick.contentY)
            flick.contentY = Math.max(flick.originY, top - margin);
        else if (top + root.height > flick.contentY + flick.height)
            flick.contentY = Math.min(flick.originY + flick.contentHeight - flick.height, top + root.height - flick.height + margin);
    }

    Loader {
        id: loader

        anchors.fill: parent
        asynchronous: true
        active: root.listKept

        sourceComponent: ListView {
            id: cards
            objectName: root.listObjectName

            anchors.fill: parent
            orientation: ListView.Horizontal
            // The gap between two cards, the same as the grids
            spacing: Kirigami.Units.largeSpacing
            leftMargin: root.edgeMargin
            rightMargin: root.edgeMargin
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            readonly property bool isCardRowList: true
            // The row this list belongs to, for the moves between rows
            readonly property Item cardRow: root
            activeFocusOnTab: true
            keyNavigationEnabled: true
            currentIndex: 0
            model: root.model
            delegate: root.delegate
            Accessible.role: Accessible.List
            Accessible.name: root.accessibleName

            // Up and Down move between the rows, Left and Right inside one
            Keys.onUpPressed: {
                if (root.externalVerticalNavigation)
                    root.upRequested();
                else
                    root.focusNeighbour(false);
            }
            Keys.onDownPressed: {
                if (root.externalVerticalNavigation)
                    root.downRequested();
                else
                    root.focusNeighbour(true);
            }

            // The cards are not tab stops: the list is, and it activates the current card
            function activateCurrent() {
                if (cards.currentItem?.enabled)
                    cards.currentItem.clicked?.();
            }
            Keys.onReturnPressed: activateCurrent()
            Keys.onEnterPressed: activateCurrent()
            Keys.onSpacePressed: activateCurrent()
            // Only the media cards have a menu: the people cards of a details page have no openMenu
            Keys.onMenuPressed: cards.currentItem?.openMenu?.(0, cards.currentItem.height)
            Keys.onPressed: event => {
                if (event.modifiers === Qt.ShiftModifier && event.key === Qt.Key_F10 && cards.currentItem?.openMenu) {
                    cards.currentItem.openMenu(0, cards.currentItem.height);
                    event.accepted = true;
                }
            }

            Controls.ScrollBar.horizontal: scrollBar
        }
    }

    Controls.ScrollBar {
        id: scrollBar

        anchors {
            left: parent.left
            right: parent.right
            bottom: parent.bottom
        }
        orientation: Qt.Horizontal
    }

    // A horizontal list swallows the vertical wheel without using it: this layer above it takes the vertical deltas for the page and leaves the horizontal ones to the list below
    Item {
        anchors.fill: parent
        z: 2

        WheelHandler {
            orientation: Qt.Vertical
            onWheel: event => {
                const flick = root.pageFlickable;
                const step = event.pixelDelta.y !== 0 ? event.pixelDelta.y : event.angleDelta.y / 120 * Kirigami.Units.gridUnit * 3;
                flick.contentY = Math.max(flick.originY, Math.min(flick.originY + flick.contentHeight - flick.height, flick.contentY - step));
            }
        }
    }
}
