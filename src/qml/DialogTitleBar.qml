// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// The header of a dialog: an optional icon, the title, a close button and a separator under them. Every side has its own padding because the desktop style ignores `padding` for the top and bottom; the bar spans the dialog border (1 px at the top and the sides) and the content sits inside it. The icon and the close button are the same size, so the bar has the same room above, below and at the sides of what you see: the icon (or the title without one), and the box of the button (it shows when focused)
QQC2.Control {
    id: bar

    property string title
    // Empty: the title starts at the edge
    property string iconName

    signal closeRequested

    // The room around the first and the last thing of the bar, the same on every side
    readonly property real inset: Kirigami.Units.largeSpacing
    readonly property real itemSize: Kirigami.Units.iconSizes.medium

    topPadding: inset + 1
    bottomPadding: inset
    leftPadding: inset + 1
    rightPadding: inset + 1

    contentItem: RowLayout {
        spacing: Kirigami.Units.largeSpacing

        Kirigami.Icon {
            objectName: "dialogTitleIcon"
            visible: bar.iconName !== ""
            source: bar.iconName
            Layout.preferredWidth: bar.itemSize
            Layout.preferredHeight: bar.itemSize
        }
        Kirigami.Heading {
            objectName: "dialogTitleText"
            level: 3
            textFormat: Text.PlainText
            elide: Text.ElideRight
            text: bar.title
            Layout.fillWidth: true
            Layout.minimumWidth: 0
        }
        QQC2.ToolButton {
            objectName: "dialogCloseButton"
            icon.name: "window-close"
            icon.width: Kirigami.Units.iconSizes.smallMedium
            icon.height: Kirigami.Units.iconSizes.smallMedium
            display: QQC2.AbstractButton.IconOnly
            text: i18nc("@action:button", "Close")
            Layout.preferredWidth: bar.itemSize
            Layout.preferredHeight: bar.itemSize
            QQC2.ToolTip.text: text
            QQC2.ToolTip.visible: hovered || visualFocus
            QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
            onClicked: bar.closeRequested()
        }
    }

    Kirigami.Separator {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
    }
}
