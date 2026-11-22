// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import org.kde.kirigami as Kirigami

// Middle-click autoscroll for the whole window: the middle button over a scrollable part of the page in front puts a marker where it went down, and moving the pointer away from the marker scrolls that part, faster the farther the pointer is. A click, the wheel, Escape (Main.qml), a popup, a change of page, the window losing the focus or the scrolled part going away ends it; so does letting go of a held middle button. One instance serves every page
Item {
    id: root

    // The window: its page stack, its popups and its startup cover
    required property var shell
    // True from the middle click that starts a scroll to its end; only begin() and stop() change it
    property bool active: false
    // The Flickable being scrolled; the property empties itself when the item is destroyed
    property Flickable area: null
    // Where the middle button went down, and where the pointer is, in this item's coordinates
    property real startX: 0
    property real startY: 0
    property real pointerX: 0
    property real pointerY: 0
    // The speed is zero within the dead zone around the start point on an axis, then acceleration times the square of the distance beyond it, up to the ceiling: 6 grid units per second at 2 grid units beyond the dead zone, 54 at 6 and 150 at 10; all in grid units, so they follow the user's scaling
    readonly property real deadZone: 0.5
    readonly property real acceleration: 1.5
    readonly property real maxSpeed: 150
    // Letting go of the middle button after this long (milliseconds), or after the pointer left the dead zone, ends the scroll; a shorter click in place leaves it running
    readonly property int holdTime: 400
    property double pressedAt: 0
    property bool movedOut: false
    property double lastTick: 0

    readonly property bool scrollsVertically: area !== null && area.contentHeight + area.topMargin + area.bottomMargin - area.height > 1
    readonly property bool scrollsHorizontally: area !== null && area.contentWidth + area.leftMargin + area.rightMargin - area.width > 1

    // The window's root item, which also holds the header: a click anywhere ends the scroll. No anchors: they would still point at the old parent while it changes
    parent: Window.contentItem
    width: parent ? parent.width : 0
    height: parent ? parent.height : 0
    // Over the page and the header, under the popups
    z: 100

    // The signed speed in pixels per second for a pointer this far from the start point on one axis
    function speed(delta: real): real {
        const beyond = Math.abs(delta) / Kirigami.Units.gridUnit - deadZone;
        if (beyond <= 0)
            return 0;
        return Math.sign(delta) * Math.min(maxSpeed, acceleration * beyond * beyond) * Kirigami.Units.gridUnit;
    }

    function outsideDeadZone(dx: real, dy: real): bool {
        const limit = deadZone * Kirigami.Units.gridUnit;
        return Math.abs(dx) > limit || Math.abs(dy) > limit;
    }

    // Collects what lies under a window point in the subtree of `item`: the scrollers (with their depth) and whether an editable text does. Hidden and transparent items and what a clip hides are left out
    function gather(item, sceneX, sceneY, depth, found) {
        if (!item.visible || item.opacity === 0)
            return;
        const inside = item.contains(item.mapFromItem(null, sceneX, sceneY));
        if (inside) {
            if (typeof item.contentX === "number" && typeof item.contentY === "number" && typeof item.contentWidth === "number" && typeof item.contentHeight === "number" && typeof item.flick === "function")
                found.scrollers.push({ item: item, depth: depth });
            else if (typeof item.selectedText === "string" && item.readOnly === false)
                found.editing = true;
        } else if (item.clip) {
            return;
        }
        for (const child of item.children)
            gather(child, sceneX, sceneY, depth + 1, found);
    }

    // The Flickable a scroll started at this window point drives: the innermost under it whose content overflows vertically, else the innermost that overflows horizontally; none on an editable text (the middle button pastes there) or where nothing scrolls
    function scrollTarget(page, sceneX, sceneY) {
        const found = { scrollers: [], editing: false };
        gather(page, sceneX, sceneY, 0, found);
        if (found.editing)
            return null;
        let vertical = null;
        let verticalDepth = -1;
        let horizontal = null;
        let horizontalDepth = -1;
        for (const entry of found.scrollers) {
            const flick = entry.item;
            if (flick.contentHeight + flick.topMargin + flick.bottomMargin - flick.height > 1) {
                if (entry.depth > verticalDepth) {
                    vertical = flick;
                    verticalDepth = entry.depth;
                }
            } else if (flick.contentWidth + flick.leftMargin + flick.rightMargin - flick.width > 1 && entry.depth > horizontalDepth) {
                horizontal = flick;
                horizontalDepth = entry.depth;
            }
        }
        return vertical ?? horizontal;
    }

    // Starts a scroll at a point of this item; false, with nothing started, while a popup or the startup cover is up or nothing there scrolls
    function begin(atX: real, atY: real): bool {
        if (active || shell.startupVisible || shell.popupOpen())
            return false;
        const page = shell.pageStack.currentItem;
        if (!page)
            return false;
        const scene = root.mapToItem(null, atX, atY);
        const target = scrollTarget(page, scene.x, scene.y);
        if (!target)
            return false;
        target.cancelFlick();
        area = target;
        startX = atX;
        startY = atY;
        pointerX = atX;
        pointerY = atY;
        movedOut = false;
        pressedAt = Date.now();
        lastTick = pressedAt;
        active = true;
        return true;
    }

    function stop() {
        if (!active)
            return;
        active = false;
        area?.returnToBounds();
        area = null;
    }

    // The middle button came up: a hold that lasted or that left the dead zone ends the scroll, a quick click in place leaves it running
    function releaseMiddle() {
        if (active && (movedOut || outsideDeadZone(pointerX - startX, pointerY - startY) || Date.now() - pressedAt > holdTime))
            stop();
    }

    // Moves the scrolled area by the real time since the last tick, within its bounds as the wheel keeps them
    function step() {
        const now = Date.now();
        const seconds = Math.min((now - lastTick) / 1000, 0.1);
        lastTick = now;
        const flick = area;
        if (!flick || !flick.visible || shell.popupOpen()) {
            stop();
            return;
        }
        const dx = pointerX - startX;
        const dy = pointerY - startY;
        if (outsideDeadZone(dx, dy))
            movedOut = true;
        if (scrollsVertically) {
            const top = flick.originY - flick.topMargin;
            const bottom = Math.max(top, flick.originY + flick.contentHeight + flick.bottomMargin - flick.height);
            flick.contentY = Math.max(top, Math.min(bottom, flick.contentY + speed(dy) * seconds));
        }
        if (scrollsHorizontally) {
            const left = flick.originX - flick.leftMargin;
            const right = Math.max(left, flick.originX + flick.contentWidth + flick.rightMargin - flick.width);
            flick.contentX = Math.max(left, Math.min(right, flick.contentX + speed(dx) * seconds));
        }
    }

    // The area went away with its page or its view
    onAreaChanged: {
        if (active && !area)
            stop();
    }

    Timer {
        interval: 16
        repeat: true
        running: root.active
        onTriggered: root.step()
    }

    Connections {
        target: root.Window.window
        function onActiveChanged() {
            if (!root.Window.window.active)
                root.stop();
        }
    }

    Connections {
        target: root.shell.pageStack
        function onCurrentItemChanged() {
            root.stop();
        }
    }

    // Takes the middle press that starts a scroll and leaves every other to the items below (a text field pastes); while the button is held it also reports the pointer. Not an item or a handler: an item that takes mouse events over the whole window hides the I-beam of the text fields under it, and a handler cannot keep a press from the items below
    MiddleButtonWatcher {
        target: root.Window.window
        takes: (x, y) => root.begin(x, y)
        onMoved: (x, y) => {
            root.pointerX = x;
            root.pointerY = y;
        }
        onReleased: root.releaseMiddle()
    }

    // Exists only while a scroll runs, and for the click that ends it: it takes every button, the hover and the wheel, so what lies under it sees none of them
    Item {
        id: overlay
        objectName: "autoScrollOverlay"
        width: root.width
        height: root.height
        visible: root.active || overlayArea.swallowing

        MouseArea {
            id: overlayArea
            // A press that ended the scroll keeps the overlay up until its release, so the click never reaches the card under it
            property bool swallowing: false
            width: parent.width
            height: parent.height
            acceptedButtons: Qt.AllButtons
            hoverEnabled: true
            cursorShape: root.scrollsVertically && root.scrollsHorizontally ? Qt.SizeAllCursor : root.scrollsHorizontally ? Qt.SizeHorCursor : Qt.SizeVerCursor
            onPositionChanged: mouse => {
                root.pointerX = mouse.x;
                root.pointerY = mouse.y;
            }
            onPressed: mouse => {
                mouse.accepted = true;
                swallowing = true;
                root.stop();
            }
            onReleased: swallowing = false
            onCanceled: swallowing = false
            // The wheel ends the scroll and goes on to the page
            onWheel: wheel => {
                wheel.accepted = false;
                root.stop();
            }
        }

        Rectangle {
            id: marker
            objectName: "autoScrollMarker"
            visible: root.active
            x: root.startX - width / 2
            y: root.startY - height / 2
            width: Kirigami.Units.gridUnit * 2
            height: width
            radius: width / 2
            color: Qt.alpha(Kirigami.Theme.backgroundColor, 0.9)
            border.width: 1
            border.color: Qt.alpha(Kirigami.Theme.textColor, 0.6)

            Arrow {
                source: "arrow-up"
                visible: root.scrollsVertically
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.top: parent.top
                anchors.topMargin: Kirigami.Units.smallSpacing
            }
            Arrow {
                source: "arrow-down"
                visible: root.scrollsVertically
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: parent.bottom
                anchors.bottomMargin: Kirigami.Units.smallSpacing
            }
            Arrow {
                source: "arrow-left"
                visible: root.scrollsHorizontally
                anchors.verticalCenter: parent.verticalCenter
                anchors.left: parent.left
                anchors.leftMargin: Kirigami.Units.smallSpacing
            }
            Arrow {
                source: "arrow-right"
                visible: root.scrollsHorizontally
                anchors.verticalCenter: parent.verticalCenter
                anchors.right: parent.right
                anchors.rightMargin: Kirigami.Units.smallSpacing
            }
        }
    }

    // One arrow of the marker, drawn in the text colour
    component Arrow: Kirigami.Icon {
        width: Kirigami.Units.gridUnit * 0.6
        height: width
        isMask: true
        color: Kirigami.Theme.textColor
    }
}
