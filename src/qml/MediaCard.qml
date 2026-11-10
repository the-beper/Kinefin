// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// A poster tile for one library or home row item: the size comes from the view that places it, the poster takes what the two label lines leave
Controls.ItemDelegate {
    id: card
    // A tile does nothing when activated until the item details exist, so it does not answer the pointer either
    hoverEnabled: false

    required property string name
    required property int year
    required property url imageUrl
    required property bool played
    required property int unplayedCount
    required property real playedPercentage
    required property bool favorite
    required property bool loaded

    // A 16:9 image area instead of the 2:3 poster
    property bool landscape: false
    // The second text line, the year when empty
    property string subtitle: ""

    readonly property real posterAspect: landscape ? 9 / 16 : 3 / 2
    readonly property string secondLine: subtitle.length > 0 ? subtitle : (year > 0 ? String(year) : "")

    readonly property bool inProgress: loaded && playedPercentage > 0 && playedPercentage < 100

    // The description lists only what applies to the item, and stays empty for a placeholder row
    readonly property string description: {
        if (!loaded)
            return "";
        const parts = [];
        if (played)
            parts.push(i18nc("state of an item", "Played"));
        if (favorite)
            parts.push(i18n("Favorite"));
        if (!played && unplayedCount > 0)
            parts.push(i18np("%1 unplayed item", "%1 unplayed items", unplayedCount));
        if (inProgress)
            parts.push(i18n("%1 percent watched", Math.round(playedPercentage)));
        return parts.join(", ");
    }

    padding: Kirigami.Units.smallSpacing
    Accessible.ignored: !loaded
    Accessible.name: subtitle.length > 0 ? name + ", " + subtitle : (year > 0 ? name + " (" + year + ")" : name)
    Accessible.description: description

    contentItem: ColumnLayout {
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.preferredHeight: width * card.posterAspect
            Layout.bottomMargin: Kirigami.Units.smallSpacing
            color: Kirigami.Theme.alternateBackgroundColor
            clip: true

            Kirigami.Icon {
                anchors.centerIn: parent
                width: Kirigami.Units.iconSizes.huge
                height: width
                source: "video-x-generic"
                visible: poster.status !== Image.Ready
            }

            Image {
                id: poster
                anchors.fill: parent
                asynchronous: true
                fillMode: Image.PreserveAspectCrop
                sourceSize.width: width * Screen.devicePixelRatio
                // A placeholder row has no image to fetch
                // Only URLs built by JellyfinClient::imageUrl belong here: the image fetcher allows nothing else, but a raw server string must never become a source
                source: card.loaded ? card.imageUrl : ""
            }

            Rectangle {
                anchors {
                    left: parent.left
                    right: parent.right
                    bottom: parent.bottom
                }
                height: Math.max(2, Kirigami.Units.smallSpacing / 2)
                visible: card.inProgress
                color: Qt.rgba(0, 0, 0, 0.6)

                Rectangle {
                    height: parent.height
                    width: parent.width * card.playedPercentage / 100
                    color: Kirigami.Theme.highlightColor
                }
            }

            Rectangle {
                anchors {
                    top: parent.top
                    left: parent.left
                    margins: Kirigami.Units.smallSpacing
                }
                width: Kirigami.Units.iconSizes.smallMedium
                height: width
                radius: width / 2
                visible: card.loaded && card.favorite
                color: Qt.rgba(0, 0, 0, 0.6)

                Kirigami.Icon {
                    anchors.fill: parent
                    anchors.margins: Kirigami.Units.smallSpacing / 2
                    source: "emblem-favorite"
                }
            }

            Rectangle {
                anchors {
                    top: parent.top
                    right: parent.right
                    margins: Kirigami.Units.smallSpacing
                }
                width: Kirigami.Units.iconSizes.smallMedium
                height: width
                radius: width / 2
                visible: card.loaded && card.played
                color: Kirigami.Theme.highlightColor

                Kirigami.Icon {
                    anchors.fill: parent
                    anchors.margins: Kirigami.Units.smallSpacing / 2
                    source: "checkmark-symbolic"
                    color: Kirigami.Theme.highlightedTextColor
                }
            }

            Rectangle {
                anchors {
                    top: parent.top
                    right: parent.right
                    margins: Kirigami.Units.smallSpacing
                }
                width: Math.max(height, unplayedLabel.implicitWidth + Kirigami.Units.largeSpacing)
                height: unplayedLabel.implicitHeight + Kirigami.Units.smallSpacing
                radius: height / 2
                visible: card.loaded && !card.played && card.unplayedCount > 0
                color: Kirigami.Theme.highlightColor

                Controls.Label {
                    id: unplayedLabel
                    anchors.centerIn: parent
                    color: Kirigami.Theme.highlightedTextColor
                    font.bold: true
                    textFormat: Text.PlainText
                    text: String(card.unplayedCount)
                }
            }
        }

        // Item names come from the server: plain text only
        Controls.Label {
            Layout.fillWidth: true
            elide: Text.ElideRight
            maximumLineCount: 1
            textFormat: Text.PlainText
            text: card.loaded ? card.name : ""
        }

        Controls.Label {
            Layout.fillWidth: true
            elide: Text.ElideRight
            maximumLineCount: 1
            opacity: 0.7
            textFormat: Text.PlainText
            text: card.loaded ? card.secondLine : ""
        }
    }
}
