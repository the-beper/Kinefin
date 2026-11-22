// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import org.kde.kirigami as Kirigami

// A tooltip that shows its text as plain text: the style's own tooltip lets Qt guess the format, so server text (item, library, user and device names) that starts with a tag would be read as rich text. Declare it inside the control it belongs to and bind `visible` (for example `visible: hovered && label.truncated`); a control's own fixed text keeps the attached Controls.ToolTip
Controls.ToolTip {
    id: tip

    delay: Kirigami.Units.toolTipDelay
    timeout: -1

    contentItem: Item {
        implicitWidth: Math.min(Kirigami.Units.gridUnit * 14, label.implicitWidth)
        implicitHeight: label.implicitHeight

        Controls.Label {
            id: label

            width: parent.width
            text: tip.text
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
            font: tip.font
            color: Kirigami.Theme.textColor

            Kirigami.Theme.inherit: false
            Kirigami.Theme.colorSet: tip.Kirigami.Theme.colorSet
        }
    }
}
