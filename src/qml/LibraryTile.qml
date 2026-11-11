// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// The tile of a library on Home: its image and its name. A type the app cannot browse yet stays disabled and says so; no tile has a text of its own that could make it wider than the others
Kirigami.AbstractCard {
    id: tile

    // One entry of `ConnectionController::libraries`: `id`, `name`, `collectionType`, `imageUrl`, `browsable` and `tab` carry server metadata, rendered as plain text
    required property var library

    // The cards share the row by what they ask for: asking the same, whatever the name says, keeps them the same size
    Layout.fillWidth: true
    Layout.preferredWidth: Kirigami.Units.gridUnit * 12
    Layout.minimumWidth: 0

    objectName: "libraryCard_" + library.id
    enabled: library.browsable
    showClickFeedback: true
    Accessible.name: library.name
    Accessible.description: library.browsable ? "" : i18n("Not supported yet")

    contentItem: ColumnLayout {
        spacing: Kirigami.Units.smallSpacing

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: width * 9 / 16
            color: Kirigami.Theme.alternateBackgroundColor
            clip: true

            Kirigami.Icon {
                anchors.centerIn: parent
                width: Kirigami.Units.iconSizes.huge
                height: width
                source: "folder-video"
                visible: image.status !== Image.Ready
            }

            Image {
                id: image
                anchors.fill: parent
                asynchronous: true
                fillMode: Image.PreserveAspectCrop
                sourceSize.width: width * Screen.devicePixelRatio
                // Only URLs built by JellyfinClient::imageUrl belong here: the image fetcher allows nothing else, but a raw server string must never become a source
                source: tile.library.imageUrl
            }
        }

        Controls.Label {
            Layout.fillWidth: true
            // Library names come from the server: plain text only
            textFormat: Text.PlainText
            elide: Text.ElideRight
            text: tile.library.name
        }

        Controls.Label {
            objectName: "libraryTileDescription"
            Layout.fillWidth: true
            visible: !tile.library.browsable
            opacity: 0.7
            textFormat: Text.PlainText
            text: i18n("Not supported yet")
        }
    }
}
