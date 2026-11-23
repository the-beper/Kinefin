// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// One episode as a list row: a 16:9 image beside the label, the runtime and the first lines of the overview. The values are plain properties so a list delegate and a single entry can both bind them
Controls.ItemDelegate {
    id: row
    objectName: "episodeRow_" + itemId
    // A placeholder row has nothing to open and does not answer the pointer or the keys, nor does an episode the server only knows of: there is no page of it to play from
    enabled: loaded && !virtualItem

    property string itemId: ""
    property string name: ""
    property int seasonNumber: -1
    property int episodeNumber: -1
    // The last number of a double episode, -1 for a single one
    property int episodeNumberEnd: -1
    property int runtimeMinutes: -1
    property string overview: ""
    property url imageUrl
    property bool played: false
    property real playedPercentage: 0
    property bool favorite: false
    property bool loaded: true
    // The server lists the episode but has no file for it: missing, or unaired when it premieres later
    property bool virtualItem: false
    property bool unaired: false
    // The label reads "S1:E5" instead of "E5", for a row shown outside its season
    property bool showSeason: false

    readonly property bool inProgress: loaded && playedPercentage > 0 && playedPercentage < 100
    readonly property real thumbnailWidth: Kirigami.Units.gridUnit * 10
    readonly property string availabilityText: !loaded || !virtualItem ? "" : (unaired ? i18nc("badge on an episode that has not premiered yet", "Unaired") : i18nc("badge on an episode that is not in the library", "Missing"))

    readonly property string numberText: {
        if (episodeNumber < 0)
            return "";
        return EpisodeLabel.label(showSeason ? seasonNumber : -1, episodeNumber, episodeNumberEnd);
    }
    readonly property string label: {
        if (!loaded)
            return "";
        if (numberText.length === 0)
            return name;
        return name.length > 0 ? i18nc("episode label and episode title", "%1 — %2", numberText, name) : numberText;
    }
    readonly property string runtimeText: runtimeMinutes > 0 ? (runtimeMinutes >= 60 ? i18n("%1 h %2 min", Math.floor(runtimeMinutes / 60), runtimeMinutes % 60) : i18n("%1 min", runtimeMinutes)) : ""
    // The description lists only what applies to the episode, and stays empty for a placeholder row
    readonly property string description: {
        if (!loaded)
            return "";
        const parts = [];
        if (availabilityText.length > 0)
            parts.push(availabilityText);
        if (runtimeText.length > 0)
            parts.push(runtimeText);
        // A rewatch stopped halfway is in progress, not watched, as on its card
        if (played && !inProgress)
            parts.push(i18nc("state of an item", "Played"));
        if (favorite)
            parts.push(i18n("Favorite"));
        if (inProgress)
            parts.push(i18np("%1 percent watched", "%1 percent watched", Math.round(playedPercentage)));
        return parts.join(i18nc("separator between items of a list", ", "));
    }

    padding: Kirigami.Units.smallSpacing
    Accessible.ignored: !loaded
    Accessible.name: label
    Accessible.description: description

    contentItem: RowLayout {
        spacing: Kirigami.Units.largeSpacing

        Rectangle {
            Layout.preferredWidth: row.thumbnailWidth
            Layout.preferredHeight: row.thumbnailWidth * 9 / 16
            Layout.alignment: Qt.AlignTop
            color: Kirigami.Theme.alternateBackgroundColor
            clip: true

            Kirigami.Icon {
                anchors.centerIn: parent
                width: Kirigami.Units.iconSizes.large
                height: width
                source: "video-x-generic"
                visible: thumbnail.opacity < 1
            }

            Image {
                id: thumbnail
                anchors.fill: parent
                // The image fades in once loaded, over the fallback icon
                opacity: status === Image.Ready ? 1 : 0
                Behavior on opacity {
                    NumberAnimation { duration: Kirigami.Units.longDuration; easing.type: Easing.OutCubic }
                }
                asynchronous: true
                fillMode: Image.PreserveAspectCrop
                sourceSize.width: width * Screen.devicePixelRatio
                // A placeholder row has no image to fetch. Only URLs built by JellyfinClient::imageUrl belong here: the image fetcher allows nothing else, but a raw server string must never become a source
                source: row.loaded ? row.imageUrl : ""
                Accessible.ignored: true
            }

            Rectangle {
                anchors {
                    left: parent.left
                    right: parent.right
                    bottom: parent.bottom
                }
                height: Math.max(2, Kirigami.Units.smallSpacing / 2)
                visible: row.inProgress
                color: Qt.rgba(0, 0, 0, 0.6)

                Rectangle {
                    height: parent.height
                    width: parent.width * row.playedPercentage / 100
                    color: Kirigami.Theme.highlightColor
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
                visible: row.loaded && row.played && !row.inProgress
                color: Kirigami.Theme.highlightColor

                Kirigami.Icon {
                    anchors.fill: parent
                    anchors.margins: Kirigami.Units.smallSpacing / 2
                    source: "checkmark-symbolic"
                    color: Kirigami.Theme.highlightedTextColor
                }
            }

            // Not enabled: the row is dimmed, so the badge sits on a solid background of its own
            Rectangle {
                objectName: "episodeAvailability"
                anchors {
                    bottom: parent.bottom
                    left: parent.left
                    margins: Kirigami.Units.smallSpacing
                }
                width: availabilityLabel.implicitWidth + Kirigami.Units.largeSpacing
                height: availabilityLabel.implicitHeight + Kirigami.Units.smallSpacing
                radius: Kirigami.Units.cornerRadius
                visible: row.availabilityText.length > 0
                color: Qt.rgba(0, 0, 0, 0.7)

                Controls.Label {
                    id: availabilityLabel
                    anchors.centerIn: parent
                    color: "white"
                    textFormat: Text.PlainText
                    text: row.availabilityText
                    Accessible.ignored: true
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
                visible: row.loaded && row.favorite
                color: Qt.rgba(0, 0, 0, 0.6)

                Kirigami.Icon {
                    anchors.fill: parent
                    anchors.margins: Kirigami.Units.smallSpacing / 2
                    source: "emblem-favorite"
                }
            }
        }

        // Episode titles and overviews come from the server: plain text only
        ColumnLayout {
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignTop
            spacing: Kirigami.Units.smallSpacing

            Controls.Label {
                Layout.fillWidth: true
                elide: Text.ElideRight
                maximumLineCount: 1
                font.bold: true
                textFormat: Text.PlainText
                text: row.label
            }

            Controls.Label {
                Layout.fillWidth: true
                visible: text.length > 0
                elide: Text.ElideRight
                maximumLineCount: 1
                opacity: 0.7
                textFormat: Text.PlainText
                text: row.loaded ? row.runtimeText : ""
            }

            Controls.Label {
                Layout.fillWidth: true
                visible: text.length > 0
                wrapMode: Text.Wrap
                elide: Text.ElideRight
                maximumLineCount: 2
                opacity: 0.7
                textFormat: Text.PlainText
                text: row.loaded ? row.overview : ""
            }
        }
    }
}
