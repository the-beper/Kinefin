// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import org.kde.kirigami as Kirigami

// The way back at the start of a page's row (PageToolbar, the row of a library opened from a tile): the shell's back, as Esc does
Controls.ToolButton {
    objectName: "pageBackButton"
    // Framed, as the other buttons of the page's bar
    flat: false
    text: i18nc("@action:button", "Back")
    icon.name: Qt.application.layoutDirection === Qt.RightToLeft ? "go-next" : "go-previous"
    display: Controls.AbstractButton.IconOnly
    Controls.ToolTip.text: text
    Controls.ToolTip.visible: hovered || visualFocus
    Controls.ToolTip.delay: Kirigami.Units.toolTipDelay
    onClicked: Window.window?.pageStack.goBack()
}
