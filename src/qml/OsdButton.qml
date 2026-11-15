// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import org.kde.kirigami as Kirigami

// A button of the player bars: icon only unless it says otherwise, its text as tooltip and accessible name, and no keyboard focus from a click, so the page keeps the keys
Controls.ToolButton {
    // Hover drives the tooltips and the volume slider whatever the platform's hover hint says
    hoverEnabled: true
    focusPolicy: Qt.TabFocus
    display: Controls.AbstractButton.IconOnly
    Controls.ToolTip.text: text
    Controls.ToolTip.visible: display === Controls.AbstractButton.IconOnly && (hovered || visualFocus)
    Controls.ToolTip.delay: Kirigami.Units.toolTipDelay
}
