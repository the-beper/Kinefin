// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// A menu entry for a name from the server or the file: it never becomes the entry's `text`, which the style's label reads as markup (it is built even when replaced) and searches for a mnemonic. The space before the name is the style's for a check mark, which every menu that uses it has
Controls.MenuItem {
    id: plainItem

    property string plainText

    Accessible.name: plainText
    contentItem: RowLayout {
        Item {
            Layout.preferredWidth: plainItem.indicator ? plainItem.indicator.width : Kirigami.Units.smallSpacing
        }
        Controls.Label {
            Layout.fillWidth: true
            Layout.maximumWidth: Kirigami.Units.gridUnit * 24
            Layout.alignment: Qt.AlignVCenter
            text: plainItem.plainText
            textFormat: Text.PlainText
            maximumLineCount: 1
            font: plainItem.font
            color: !plainItem.enabled ? Kirigami.Theme.disabledTextColor : plainItem.pressed ? Kirigami.Theme.highlightedTextColor : Kirigami.Theme.textColor
            elide: Text.ElideRight
        }
    }
}
