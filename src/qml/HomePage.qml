// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Kirigami.ScrollablePage {
    id: page
    objectName: "homePage"

    required property ConnectionController connection
    property string requestedItemId: ""

    title: i18n("Home")

    ColumnLayout {
        spacing: Kirigami.Units.largeSpacing

        MessageBox {
            visible: page.connection.errorText.length > 0
            type: Kirigami.MessageType.Error
            text: page.connection.errorText
            actions: [
                Kirigami.Action {
                    text: i18n("Retry")
                    icon.name: "view-refresh"
                    onTriggered: page.connection.reloadLibraries()
                }
            ]
        }

        MessageBox {
            visible: page.requestedItemId.length > 0
            type: Kirigami.MessageType.Information
            text: i18n("Requested item: %1", page.requestedItemId)
        }

        Kirigami.PlaceholderMessage {
            Layout.fillWidth: true
            visible: page.connection.libraries.length === 0
            text: i18n("No libraries to show")
            explanation: i18n("Library browsing is not implemented yet")
            icon.name: "folder-video"
        }

        Kirigami.Heading {
            visible: page.connection.libraries.length > 0
            level: 2
            text: i18n("Libraries")
        }

        Kirigami.CardsLayout {
            Layout.fillWidth: true
            visible: page.connection.libraries.length > 0
            Repeater {
                model: page.connection.libraries
                delegate: Kirigami.AbstractCard {
                    id: card
                    required property var modelData
                    contentItem: Controls.Label {
                        // Library names come from the server: plain text only.
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                        text: card.modelData.name
                    }
                }
            }
        }
    }
}
