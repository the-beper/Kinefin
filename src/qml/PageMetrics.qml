// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
pragma Singleton
import QtQuick
import org.kde.kirigami as Kirigami

// The one margin of the content pages: the room between the edge of a page (the bar above it, the sides) and its first item, and the gap between two cards of a grid
QtObject {
    readonly property int margin: Kirigami.Units.largeSpacing

    // Set by the shell: a Compact window has no bar, and the button that opens the navigation floats in the top corner (the leading one), which the pages keep free
    property bool cornerNavigation: false
    // The side of the buttons that float in the top corner: the navigation button and the main menu button of a window with no bar
    readonly property real cornerButton: Kirigami.Units.gridUnit * 2
    // The height of the bar of a page, which the floating button is centred in; a page with no bar leaves a strip as tall above its content
    readonly property real barHeight: Kirigami.Units.gridUnit * 2.5
}
