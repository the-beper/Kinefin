// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// One row of a list of videos (the queue, a playlist): the image, the title (an episode with its series and number), the year and runtime, a check when played. The values come from the model roles; the list owns the keys and the menu
Controls.ItemDelegate {
    id: row

    required property int index
    required property string itemId
    required property string name
    required property string seriesName
    required property int seasonNumber
    required property int episodeNumber
    required property int year
    required property int runtimeMinutes
    required property url imageUrl
    required property bool played

    signal opened
    signal menuRequested(real x, real y)

    readonly property real thumbnailWidth: Kirigami.Units.gridUnit * 8
    readonly property string title: {
        if (episodeNumber < 0)
            return name;
        const label = EpisodeLabel.label(seasonNumber, episodeNumber);
        const episode = name.length > 0 ? i18nc("episode label and episode title", "%1 — %2", label, name) : label;
        return seriesName.length > 0 ? i18nc("series name and episode", "%1 — %2", seriesName, episode) : episode;
    }
    readonly property string details: {
        const parts = [];
        if (year > 0)
            parts.push(String(year));
        if (runtimeMinutes > 0)
            parts.push(i18n("%1 min", runtimeMinutes));
        return parts.join(i18nc("separator between items of a list", ", "));
    }

    // The row spans the list and its visible part is capped at the shell's reading width and centred: a ListView resets the x of its delegates, so the side margins are insets
    readonly property real sideMargin: Math.max(0, (ListView.view.width - (ListView.view.Window.window?.readingWidth ?? ListView.view.width)) / 2)
    width: ListView.view.width
    leftInset: sideMargin
    rightInset: sideMargin
    leftPadding: sideMargin + Kirigami.Units.smallSpacing
    rightPadding: sideMargin + Kirigami.Units.smallSpacing
    containmentMask: QtObject {
        function contains(point: point): bool {
            return point.x >= row.sideMargin && point.x <= row.width - row.sideMargin;
        }
    }
    // The list is the one tab stop, the arrow keys move inside it
    focusPolicy: Qt.NoFocus
    highlighted: ListView.isCurrentItem && ListView.view.activeFocus
    padding: Kirigami.Units.smallSpacing

    // A click makes this row the current one and gives the list the focus, so the focus comes back to this row after Back
    onPressed: {
        ListView.view.currentIndex = index;
        ListView.view.forceActiveFocus(Qt.MouseFocusReason);
    }
    onClicked: opened()

    TapHandler {
        acceptedButtons: Qt.RightButton
        onTapped: eventPoint => {
            row.ListView.view.currentIndex = row.index;
            row.ListView.view.forceActiveFocus(Qt.MouseFocusReason);
            row.menuRequested(eventPoint.position.x, eventPoint.position.y);
        }
    }

    Accessible.role: Accessible.ListItem
    Accessible.name: i18n("%1, %2 of %3", title, index + 1, ListView.view.count)
    Accessible.description: played ? i18nc("state of an item", "Played") : ""

    contentItem: RowLayout {
        spacing: Kirigami.Units.largeSpacing

        Rectangle {
            Layout.preferredWidth: row.thumbnailWidth
            Layout.preferredHeight: row.thumbnailWidth * 9 / 16
            Layout.alignment: Qt.AlignVCenter
            color: Kirigami.Theme.alternateBackgroundColor
            clip: true

            Kirigami.Icon {
                anchors.centerIn: parent
                width: Kirigami.Units.iconSizes.large
                height: width
                source: "video-x-generic"
                visible: image.opacity < 1
            }

            Image {
                id: image
                anchors.fill: parent
                // The image fades in once loaded, over the fallback icon
                opacity: status === Image.Ready ? 1 : 0
                Behavior on opacity {
                    NumberAnimation { duration: Kirigami.Units.longDuration; easing.type: Easing.OutCubic }
                }
                asynchronous: true
                fillMode: Image.PreserveAspectCrop
                sourceSize.width: width * Screen.devicePixelRatio
                // Only URLs built by JellyfinClient::imageUrl belong here: the image fetcher allows nothing else, but a raw server string must never become a source
                source: row.imageUrl
                Accessible.ignored: true
            }
        }

        // Item and series names come from the server: plain text only
        ColumnLayout {
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignVCenter
            spacing: Kirigami.Units.smallSpacing

            Controls.Label {
                Layout.fillWidth: true
                elide: Text.ElideRight
                maximumLineCount: 1
                font.bold: row.ListView.isCurrentItem
                textFormat: Text.PlainText
                text: row.title
            }

            Controls.Label {
                Layout.fillWidth: true
                visible: text.length > 0
                elide: Text.ElideRight
                maximumLineCount: 1
                opacity: 0.7
                textFormat: Text.PlainText
                text: row.details
            }
        }

        Kirigami.Icon {
            Layout.preferredWidth: Kirigami.Units.iconSizes.smallMedium
            Layout.preferredHeight: Kirigami.Units.iconSizes.smallMedium
            Layout.alignment: Qt.AlignVCenter
            visible: row.played
            source: "checkmark-symbolic"
            Accessible.ignored: true
        }
    }
}
