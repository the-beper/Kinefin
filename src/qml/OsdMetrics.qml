// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
pragma Singleton
import QtQuick
import org.kde.kirigami as Kirigami

// The proportions of the boxes over the picture, shared by the mini player and the picture-in-picture window so that the two look alike
QtObject {
    // The distance of a box from the edge of its window: nothing touches an edge, and the picture-in-picture window keeps its resize strips this wide
    readonly property int edge: Kirigami.Units.largeSpacing
    // The side of a button, and the room around it on its panel; a box that is a button itself is the side of a panel
    readonly property int buttonSize: Math.round(Kirigami.Units.gridUnit * 1.75)
    readonly property int panelPadding: Math.round(Kirigami.Units.smallSpacing / 2)
    readonly property int panelSize: buttonSize + panelPadding * 2
}
