// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import org.kde.kirigami as Kirigami

// Fades the item it is declared in (a placeholder, a busy indicator) in whenever that item starts to show, so nothing appears at once; hiding stays immediate, as the content taking its place shows. It takes no room: a layout skips it
Item {
    id: helper

    visible: false

    // On the GUI thread, not an OpacityAnimator: a render-thread animator left the faded item drawn after the item was hidden mid-fade (a "Loading…" stayed over the loaded page)
    NumberAnimation {
        id: fade
        target: helper.parent
        property: "opacity"
        from: 0
        to: 1
        duration: Kirigami.Units.longDuration
        easing.type: Easing.OutCubic
    }
    Connections {
        target: helper.parent
        function onVisibleChanged() {
            if (helper.parent?.visible) {
                fade.restart();
            } else {
                fade.stop();
                helper.parent.opacity = 1;
            }
        }
    }
    Component.onCompleted: {
        if (parent?.visible)
            fade.restart();
    }
}
