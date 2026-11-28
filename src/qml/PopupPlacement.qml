// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Templates as T

// Where a menu or a combo box popup that a button opens sits: below the button with the start edges aligned (the other edges when it would leave the window), above it when it does not fit below or `preferAbove` is set, and on the side with more room and cut to it (it scrolls) when it fits on neither; never over the button. Qt shifts a popup that overflows the window, which can land it over its button, so the position and the height are bindings that follow the popup's real size. A menu at the pointer (right click, Menu key) and a submenu are placed by their owner, not by this
QtObject {
    id: placement

    required property T.Popup popup
    // The button or control only this popup opens (a combo box, a button's own menu): the placement is always on
    property Item anchor: null
    // The button of the moment, for a popup that several buttons open: `openFor` sets it, and it is cleared once the popup has closed
    property Item openedFrom: null
    // Opens above the button when it fits there, as the player's bottom bar does
    property bool preferAbove: false
    // Opens at the side of the button instead of below or above it, bottom-aligned with it and moved up as far as the window needs: the menu of a button in a rail at the window's edge
    property bool beside: false
    // The room left between the button and a popup at its side: the margin and the line of the rail or the drawer that holds the button, which the popup must not cover
    property real besideGap: 0

    // `popup(parent, x, y)` of another opener changes the parent: the button counts only while the popup hangs from it
    readonly property Item control: openedFrom && openedFrom === popup.parent ? openedFrom : anchor
    // The coordinates of the popup's x and y are those of its parent: whatever the owner made it
    readonly property Item frame: popup.parent
    readonly property Window window: control ? control.Window.window : null
    readonly property bool ready: control !== null && frame !== null && window !== null

    // Counts the openings: where the button sits in the window is read again each time
    property int openings: 0
    readonly property point controlAt: {
        placement.openings;
        placement.window?.width;
        placement.window?.height;
        return placement.control && placement.window ? placement.control.mapToItem(null, 0, 0) : Qt.point(0, 0);
    }
    readonly property point frameAt: {
        placement.openings;
        placement.window?.width;
        placement.window?.height;
        return placement.frame && placement.window ? placement.frame.mapToItem(null, 0, 0) : Qt.point(0, 0);
    }

    // The height the popup has when nothing cuts it, the style's own sum (reading the popup's implicit height would loop with the cut below)
    readonly property real fullHeight: Math.max(popup.implicitBackgroundHeight + popup.topInset + popup.bottomInset, popup.contentHeight + popup.topPadding + popup.bottomPadding)
    readonly property real roomAbove: controlAt.y
    readonly property real roomBelow: control && window ? window.height - controlAt.y - control.height : 0
    readonly property bool opensUp: {
        const fitsAbove = roomAbove >= fullHeight;
        const fitsBelow = roomBelow >= fullHeight;
        if (fitsAbove && fitsBelow)
            return preferAbove;
        if (fitsAbove || fitsBelow)
            return fitsAbove;
        return roomAbove > roomBelow || (roomAbove === roomBelow && preferAbove);
    }
    // At the side of the button only when the popup fits there, on one side or the other; else below or above it like any other popup, never over the button
    readonly property bool besideFits: {
        if (!beside || !control || !window)
            return false;
        const width = popup.width;
        return controlAt.x + control.width + besideGap + width <= window.width || controlAt.x - besideGap - width >= 0;
    }
    readonly property real shownHeight: Math.min(fullHeight, Math.max(0, besideFits ? (window?.height ?? 0) : opensUp ? roomAbove : roomBelow))
    readonly property bool cut: ready && shownHeight < fullHeight

    // In the window, as the scene counts: the popup's left edge, with the start edges aligned (the left ones, the right ones when mirrored) unless that leaves the window
    readonly property real sceneX: {
        if (!placement.control || !placement.window)
            return 0;
        const width = placement.popup.width;
        const rtl = Qt.application.layoutDirection === Qt.RightToLeft;
        if (placement.besideFits) {
            const gap = placement.besideGap;
            const trailing = rtl ? placement.controlAt.x - gap - width : placement.controlAt.x + placement.control.width + gap;
            const leading = rtl ? placement.controlAt.x + placement.control.width + gap : placement.controlAt.x - gap - width;
            const left = trailing >= 0 && trailing + width <= placement.window.width ? trailing : leading;
            return Math.max(0, Math.min(left, placement.window.width - width));
        }
        const leftAligned = placement.controlAt.x;
        const rightAligned = placement.controlAt.x + placement.control.width - width;
        let left = rtl ? rightAligned : leftAligned;
        if (left < 0 || left + width > placement.window.width)
            left = rtl ? leftAligned : rightAligned;
        return Math.max(0, Math.min(left, placement.window.width - width));
    }
    readonly property real sceneY: {
        if (besideFits)
            return Math.max(0, Math.min(controlAt.y + (control?.height ?? 0) - popup.height, (window?.height ?? 0) - popup.height));
        return opensUp ? controlAt.y - popup.height : controlAt.y + (control?.height ?? 0);
    }

    // Opens the popup of a button that shares it with others or with the pointer: it is placed as above until it closes, then it is the owner's again
    function openFor(button: Item): void {
        openedFrom = button;
        popup.parent = button;
        popup.open();
    }

    // QtObject has no default property for children, so the Connections and the Bindings are the items of a list
    property list<QtObject> parts: [
        Connections {
            target: placement.popup
            function onAboutToShow() {
                placement.openings++;
            }
            // A layout that was still settling when the popup was asked for (the window was just resized) has moved the button since: it is read once more
            function onOpened() {
                placement.openings++;
            }
            function onClosed() {
                placement.openedFrom = null;
            }
        },
        Binding {
            target: placement.popup
            property: "x"
            value: placement.sceneX - placement.frameAt.x
            when: placement.ready
            restoreMode: Binding.RestoreNone
        },
        Binding {
            target: placement.popup
            property: "y"
            value: placement.sceneY - placement.frameAt.y
            when: placement.ready
            restoreMode: Binding.RestoreNone
        },
        // The cut: a menu taller than the room it has scrolls in the room, which the style's own `interactive` (only for a menu taller than the window) does not say
        Binding {
            target: placement.popup
            property: "implicitHeight"
            value: placement.shownHeight
            when: placement.cut
            restoreMode: Binding.RestoreBindingOrValue
        },
        Binding {
            target: placement.popup.contentItem
            property: "interactive"
            value: true
            when: placement.cut
            restoreMode: Binding.RestoreBindingOrValue
        }
    ]
}
