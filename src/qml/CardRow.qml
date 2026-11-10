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

    ListView {
        id: list
        objectName: root.listObjectName

        anchors.fill: parent
        orientation: ListView.Horizontal
        spacing: Kirigami.Units.largeSpacing
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        activeFocusOnTab: true
        keyNavigationEnabled: true
        currentIndex: 0
        model: root.model
        delegate: root.delegate
        Accessible.role: Accessible.List
        Accessible.name: root.accessibleName

        // Up and Down move between the rows, Left and Right inside one
        Keys.onUpPressed: {
            if (root.externalVerticalNavigation) {
                root.upRequested();
                return;
            }
            const target = list.nextItemInFocusChain(false);
            if (target && target !== list)
                target.forceActiveFocus(Qt.BacktabFocusReason);
        }
        Keys.onDownPressed: {
            if (root.externalVerticalNavigation) {
                root.downRequested();
                return;
            }
            const target = list.nextItemInFocusChain(true);
            if (target && target !== list)
                target.forceActiveFocus(Qt.TabFocusReason);
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
