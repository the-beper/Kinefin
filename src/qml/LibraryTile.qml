// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// The tile of a library on Home: its image, which Jellyfin draws the library's name into, so the name shows as text only while there is no image (and in the tooltip). A type the app cannot browse yet stays disabled and says so. Home gives the width
Kirigami.AbstractCard {
    id: tile

    // One entry of `ConnectionController::libraries`: `id`, `name`, `collectionType`, `imageUrl`, `browsable` and `tab` carry server metadata, rendered as plain text
    required property var library

    objectName: "libraryCard_" + library.id
    enabled: library.browsable
    showClickFeedback: true
    Accessible.name: library.name
    Accessible.description: library.browsable ? "" : i18n("Not supported yet")

    // The library name is server text: the tooltip shows it as plain text
    PlainToolTip {
        visible: (tile.hovered || tile.visualFocus) && image.status === Image.Ready
        text: tile.library.name
    }

    // The focus ring the keyboard and the pad show: the style's own highlight fills the card, which would hide the image
    background: Rectangle {
        readonly property bool ring: tile.visualFocus
        color: tile.pressed ? Qt.alpha(Kirigami.Theme.highlightColor, 0.3) : (tile.hovered ? Qt.alpha(Kirigami.Theme.highlightColor, 0.15) : "transparent")
        radius: Kirigami.Units.cornerRadius
        border.width: ring ? 2 : 0
        border.color: Kirigami.Theme.highlightColor
    }

    contentItem: ColumnLayout {
        spacing: Kirigami.Units.smallSpacing
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: width * 9 / 16
            color: Kirigami.Theme.alternateBackgroundColor
            clip: true

            ColumnLayout {
                anchors.centerIn: parent
                width: Math.max(0, parent.width - Kirigami.Units.largeSpacing * 2)
                visible: image.status !== Image.Ready
                spacing: Kirigami.Units.smallSpacing

                Kirigami.Icon {
                    Layout.alignment: Qt.AlignHCenter
                    implicitWidth: Kirigami.Units.iconSizes.large
                    implicitHeight: implicitWidth
                    source: "folder-video"
                }
                Controls.Label {
                    objectName: "libraryTileName"
                    Layout.fillWidth: true
                    // Not while the image loads: the name would flash on every tile that has one
                    visible: image.status === Image.Null || image.status === Image.Error
                    horizontalAlignment: Text.AlignHCenter
                    // Library names come from the server: plain text only
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                    text: tile.library.name
                }
            }

            Image {
                id: image
                anchors.fill: parent
                opacity: status === Image.Ready ? 1 : 0
                Behavior on opacity {
                    NumberAnimation { duration: Kirigami.Units.longDuration; easing.type: Easing.OutCubic }
                }
                asynchronous: true
                fillMode: Image.PreserveAspectCrop
                sourceSize.width: width * Screen.devicePixelRatio
                // Only URLs built by JellyfinClient::imageUrl belong here: the image fetcher allows nothing else, but a raw server string must never become a source
                source: tile.library.imageUrl
            }

            // The image covers a border drawn on this rectangle: the contour is a transparent overlay on top
            Rectangle {
                anchors.fill: parent
                color: "transparent"
                border.width: 1
                border.color: Qt.alpha(Kirigami.Theme.textColor, 0.3)
                Accessible.ignored: true
            }
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
