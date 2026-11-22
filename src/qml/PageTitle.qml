// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// The page's name in its row, beside the way back: server text for a playlist or a library, so plain text only; it takes up to half the row before it is cut, and the whole name shows under the pointer
Controls.Label {
    id: label

    Layout.maximumWidth: Math.max(Kirigami.Units.gridUnit * 16, (parent?.width ?? 0) / 2)
    visible: text.length > 0
    elide: Text.ElideRight
    maximumLineCount: 1
    font.weight: Font.DemiBold
    textFormat: Text.PlainText

    HoverHandler {
        id: hover
    }

    PlainToolTip {
        visible: label.truncated && hover.hovered
        text: label.text
    }
}
