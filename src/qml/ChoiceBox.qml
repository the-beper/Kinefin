// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import org.kde.kirigami as Kirigami

// The drop-down list of the app (the settings rows use the delegates of kirigami-addons): a combo box over a list of objects, `textRole` naming the field with the text of a row. The list is a menu, its rows menu items with the chosen one highlighted, and it opens below the box and above it when there is no room below, never over it. Rows are plain text: the names come from the server, and the style's own rows read them as mnemonic rich text
Controls.ComboBox {
    id: choiceBox

    // The start of the object names of the rows (the index follows), none when empty
    property string choicePrefix: ""

    PopupPlacement { popup: choiceBox.popup; anchor: choiceBox }

    delegate: Controls.MenuItem {
        id: choice

        required property int index
        required property var modelData

        // The row's text as the data holds it
        readonly property string plain: String(choiceBox.textRole.length > 0 ? modelData[choiceBox.textRole] : modelData)

        objectName: choiceBox.choicePrefix.length > 0 ? choiceBox.choicePrefix + index : ""
        // The style builds its own label for a row before this one replaces it, and that one reads `text` as rich text: the markup of a name from the server is made inert there
        text: plain.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;")
        Accessible.name: plain
        highlighted: choiceBox.highlightedIndex === index
        Kirigami.MnemonicData.enabled: false
        contentItem: Controls.Label {
            text: choice.plain
            textFormat: Text.PlainText
            elide: Text.ElideRight
            font: choice.font
            verticalAlignment: Text.AlignVCenter
            color: choice.highlighted || choice.pressed ? Kirigami.Theme.highlightedTextColor : Kirigami.Theme.textColor
        }
    }
}
