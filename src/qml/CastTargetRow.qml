// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// One row of the Cast list: icon, name, an optional detail line, and a check mark on the target that plays now
Controls.ItemDelegate {
    id: row
    property string iconName
    property string subtitle
    property bool current: false
    // The list is the one tab stop, the arrow keys move inside it
    focusPolicy: Qt.NoFocus
    Accessible.name: text
    Accessible.description: subtitle
    Accessible.checkable: true
    Accessible.checked: current
    // The rows take no focus of their own: the list's current one is the focused one for a screen reader
    Accessible.focused: highlighted
    contentItem: RowLayout {
        spacing: Kirigami.Units.largeSpacing
        Kirigami.Icon {
            Layout.alignment: Qt.AlignVCenter
            implicitWidth: Kirigami.Units.iconSizes.medium
            implicitHeight: Kirigami.Units.iconSizes.medium
            source: row.iconName
        }
        ColumnLayout {
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignVCenter
            spacing: 0
            // Device names and models come from the network: plain text only
            Controls.Label {
                Layout.fillWidth: true
                text: row.text
                textFormat: Text.PlainText
                elide: Text.ElideRight
                maximumLineCount: 1
                font.bold: row.current
            }
            Controls.Label {
                Layout.fillWidth: true
                visible: row.subtitle.length > 0
                text: row.subtitle
                textFormat: Text.PlainText
                elide: Text.ElideRight
                maximumLineCount: 1
                font: Kirigami.Theme.smallFont
                opacity: 0.7
            }
        }
        Kirigami.Icon {
            Layout.alignment: Qt.AlignVCenter
            implicitWidth: Kirigami.Units.iconSizes.small
            implicitHeight: Kirigami.Units.iconSizes.small
            source: "checkmark-symbolic"
            // The room stays when hidden, so names line up whichever row is checked
            opacity: row.current ? 1 : 0
        }
    }
}
