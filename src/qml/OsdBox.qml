// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import org.kde.kirigami as Kirigami

// A box of the bars of the player, the mini player and the picture-in-picture window: a rounded rectangle in the OSD's own colours, nearly opaque and outlined to stand apart from the picture. As the background of a button (`control`) it is a little lighter under the pointer, takes the colour of selection while pressed, shows a ring for the keyboard focus and draws the icon of the button, which the desktop style draws in its own background; without a control it is a panel
Rectangle {
    id: box

    property var control: null

    readonly property color base: Qt.alpha(Kirigami.Theme.backgroundColor, 0.9)

    radius: Kirigami.Units.cornerRadius
    color: box.control?.down ? Qt.tint(box.base, Qt.alpha(Kirigami.Theme.highlightColor, 0.6)) : box.control?.hovered ? Qt.tint(box.base, Qt.alpha(Kirigami.Theme.highlightColor, 0.3)) : box.base
    border.width: box.control?.visualFocus ? 2 : 1
    border.color: box.control?.visualFocus ? Kirigami.Theme.highlightColor : Qt.alpha(Kirigami.Theme.textColor, 0.2)

    Kirigami.Icon {
        anchors.centerIn: parent
        readonly property bool hasIcon: !!box.control && (box.control.icon.name !== "" || box.control.icon.source.toString() !== "")
        visible: hasIcon
        source: hasIcon ? (box.control.icon.name !== "" ? box.control.icon.name : box.control.icon.source) : ""
        implicitWidth: box.control?.icon.width > 0 ? box.control.icon.width : Kirigami.Units.iconSizes.smallMedium
        implicitHeight: box.control?.icon.height > 0 ? box.control.icon.height : Kirigami.Units.iconSizes.smallMedium
        color: Kirigami.Theme.textColor
    }
}
