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
    required property Item pageFlickable
    // The object name of the inner list, for the tests
    property string listObjectName: ""
    // A page that builds its rows lazily moves between them itself: Up and Down then emit the signals below instead of moving the focus
    property bool externalVerticalNavigation: false

    signal upRequested
    signal downRequested

    Layout.fillWidth: true
    implicitHeight: cardHeight + scrollBar.implicitHeight

    function focusList(reason) {
        list.forceActiveFocus(reason);
    }

    // The next row of cards above or below along the focus chain, which also lists containers and other controls; no row there, no move
    function focusNeighbour(forward) {
        let item = list;
        // The chain returns to this list; the bound only guards a chain that never does
        for (let step = 0; step < 1000; ++step) {
            item = item.nextItemInFocusChain(forward);
            if (!item || item === list)
                return;
            // The chain wraps around: a row above is not the answer to Down
            if (item.visible && item.enabled && item.isCardRowList === true && (item.mapToItem(null, 0, 0).y - list.mapToItem(null, 0, 0).y) * (forward ? 1 : -1) > 0) {
                item.forceActiveFocus(forward ? Qt.TabFocusReason : Qt.BacktabFocusReason);
                item.parent?.scrollIntoView?.();
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

    ListView {
        id: list
        objectName: root.listObjectName

        anchors.fill: parent
        orientation: ListView.Horizontal
        spacing: Kirigami.Units.largeSpacing
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        readonly property bool isCardRowList: true
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
            if (list.currentItem?.enabled)
                list.currentItem.clicked?.();
        }
        Keys.onReturnPressed: activateCurrent()
        Keys.onEnterPressed: activateCurrent()
        Keys.onSpacePressed: activateCurrent()

        Controls.ScrollBar.horizontal: Controls.ScrollBar {
            id: scrollBar
        }
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
