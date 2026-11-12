// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// One item of a library list: a small image, the name with its year or season and episode counts, the watched state and the card menu. The list around it is the one tab stop: the row takes the highlight from `ListView.isCurrentItem && ListView.view.activeFocus`
Controls.ItemDelegate {
    id: row
    objectName: "listRow_" + itemId
    // A placeholder row has nothing to open and does not answer the pointer or the keys
    enabled: loaded

    required property int index
    required property string itemId
    required property string name
    required property int year
    required property url imageUrl
    required property bool played
    required property int unplayedCount
    required property real playedPercentage
    required property bool favorite
    required property bool loaded
    required property string type
    required property string seriesId
    required property real rating
    required property int seasonCount
    required property int episodeCount
    required property real seriesProgress
    required property string seriesName
    required property int seasonNumber
    required property int episodeNumber

    readonly property string countsLine: seasonCount > 0 && episodeCount > 0 ? i18nc("seasons and episodes of a series", "%1 · %2", i18np("%1 season", "%1 seasons", seasonCount), i18np("%1 episode", "%1 episodes", episodeCount)) : ""
    // An episode is named by its title: the series and the numbers are its second line
    readonly property string episodeLine: {
        if (type !== "Episode" || episodeNumber < 0)
            return "";
        const label = seasonNumber >= 0 ? i18nc("season and episode number", "S%1:E%2", seasonNumber, episodeNumber) : i18nc("episode number", "E%1", episodeNumber);
        return seriesName.length > 0 ? i18nc("series name and episode label", "%1 · %2", seriesName, label) : label;
    }
    // What a screen reader says for the episode line: "S1:E3" would be spelled out
    readonly property string episodeSpoken: {
        if (episodeLine.length === 0)
            return "";
        const label = seasonNumber >= 0 ? i18nc("season and episode number spoken", "Season %1, Episode %2", seasonNumber, episodeNumber) : i18nc("episode number spoken", "Episode %1", episodeNumber);
        return seriesName.length > 0 ? i18nc("series name and episode label", "%1 · %2", seriesName, label) : label;
    }
    readonly property string secondLine: countsLine.length > 0 ? countsLine : (episodeLine.length > 0 ? episodeLine : (year > 0 ? String(year) : ""))
    // The watched share, 0 to 1: a series counts its episodes, anything else its playback position
    readonly property real progress: seriesProgress >= 0 ? seriesProgress : playedPercentage / 100
    readonly property bool inProgress: loaded && progress > 0 && progress < 1
    readonly property string description: {
        if (!loaded)
            return "";
        const parts = [];
        if (played)
            parts.push(i18nc("state of an item", "Played"));
        if (favorite)
            parts.push(i18n("Favorite"));
        if (rating > 0)
            parts.push(i18n("Rated %1 out of 10", Number(rating).toLocaleString(Qt.locale(), "f", 1)));
        if (!played && unplayedCount > 0)
            parts.push(i18np("%1 unplayed item", "%1 unplayed items", unplayedCount));
        if (inProgress)
            parts.push(i18np("%1 percent watched", "%1 percent watched", Math.round(progress * 100)));
        return parts.join(", ");
    }

    // Opens the card menu of the window: at the given point of the row, below it when none
    function openMenu(x, y) {
        Window.window?.openCardMenu?.(row, itemId, type, seriesId, x ?? 0, y ?? row.height);
    }

    // A click makes this row the current one and gives its list the focus, so the focus comes back to this row after Back
    function makeCurrent() {
        const view = ListView.view;
        if (!view)
            return;
        view.currentIndex = row.index;
        view.forceActiveFocus(Qt.MouseFocusReason);
    }

    onPressed: makeCurrent()

    TapHandler {
        acceptedButtons: Qt.RightButton
        onTapped: eventPoint => {
            row.makeCurrent();
            row.openMenu(eventPoint.position.x, eventPoint.position.y);
        }
    }

    padding: Kirigami.Units.smallSpacing
    Accessible.ignored: !loaded
    Accessible.name: countsLine.length > 0 ? name + ", " + countsLine : (episodeSpoken.length > 0 ? name + ", " + episodeSpoken : (year > 0 ? name + " (" + year + ")" : name))
    Accessible.description: description

    contentItem: RowLayout {
        spacing: Kirigami.Units.largeSpacing

        Rectangle {
            Layout.preferredWidth: Kirigami.Units.gridUnit * 3
            Layout.preferredHeight: Kirigami.Units.gridUnit * 4.5
            color: Kirigami.Theme.alternateBackgroundColor
            clip: true

            Kirigami.Icon {
                anchors.centerIn: parent
                width: Kirigami.Units.iconSizes.medium
                height: width
                source: "video-x-generic"
                visible: thumb.status !== Image.Ready
            }

            Image {
                id: thumb
                anchors.fill: parent
                asynchronous: true
                fillMode: Image.PreserveAspectFit
                sourceSize.width: width * Screen.devicePixelRatio
                // Only URLs built by JellyfinClient::imageUrl belong here: a raw server string must never become a source
                source: row.loaded ? row.imageUrl : ""
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 0

            // Item names come from the server: plain text only
            Controls.Label {
                Layout.fillWidth: true
                elide: Text.ElideRight
                maximumLineCount: 1
                textFormat: Text.PlainText
                text: row.loaded ? row.name : ""
            }

            Controls.Label {
                Layout.fillWidth: true
                elide: Text.ElideRight
                maximumLineCount: 1
                opacity: 0.7
                textFormat: Text.PlainText
                text: row.loaded ? row.secondLine : ""
            }

            Rectangle {
                objectName: "listRowProgress"
                Layout.fillWidth: true
                Layout.topMargin: Kirigami.Units.smallSpacing
                Layout.preferredHeight: Math.max(2, Kirigami.Units.smallSpacing / 2)
                visible: row.inProgress
                color: Kirigami.Theme.alternateBackgroundColor

                Rectangle {
                    height: parent.height
                    width: parent.width * row.progress
                    color: Kirigami.Theme.highlightColor
                }
            }
        }

        Kirigami.Icon {
            objectName: "listRowFavorite"
            Layout.preferredWidth: Kirigami.Units.iconSizes.small
            Layout.preferredHeight: Kirigami.Units.iconSizes.small
            visible: row.loaded && row.favorite
            source: "emblem-favorite"
        }

        Controls.Label {
            objectName: "listRowRating"
            visible: row.loaded && row.rating > 0
            opacity: 0.7
            textFormat: Text.PlainText
            text: row.rating.toLocaleString(Qt.locale(), "f", 1)
        }

        Kirigami.Icon {
            objectName: "listRowPlayed"
            Layout.preferredWidth: Kirigami.Units.iconSizes.smallMedium
            Layout.preferredHeight: Kirigami.Units.iconSizes.smallMedium
            visible: row.loaded && row.played
            source: "checkmark-symbolic"
        }

        Controls.Label {
            objectName: "listRowUnplayed"
            visible: row.loaded && !row.played && row.unplayedCount > 0
            font.bold: true
            textFormat: Text.PlainText
            text: String(row.unplayedCount)
        }

        // The pointer and keyboard ways to the menu are the right click and the Menu key: this button is for the pointer alone
        Controls.ToolButton {
            objectName: "listRowMenuButton"
            focusPolicy: Qt.NoFocus
            visible: row.loaded
            display: Controls.AbstractButton.IconOnly
            icon.name: "overflow-menu"
            text: i18n("More Actions")
            // The tooltip keeps the short text: a screen reader hears which item the button belongs to
            Accessible.name: i18n("More Actions for %1", row.name)
            Controls.ToolTip.text: text
            Controls.ToolTip.visible: hovered
            Controls.ToolTip.delay: Kirigami.Units.toolTipDelay
            onClicked: {
                const point = mapToItem(row, 0, height);
                row.openMenu(point.x, point.y);
            }
        }
    }
}
