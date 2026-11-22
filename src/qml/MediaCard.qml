// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// A poster tile for one library or home row item: the size comes from the view that places it, the poster takes what the two label lines leave
Controls.ItemDelegate {
    id: card
    objectName: "mediaCard_" + itemId
    // A placeholder tile has nothing to open and does not answer the pointer or the keys
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
    // The community rating, 0 to 10, and the minutes left to watch; -1 when unknown
    required property real rating
    required property int remainingMinutes
    required property bool isNew
    // A series only: how many seasons and episodes it has (-1 otherwise), and the share of its episodes watched, 0 to 1 (-1 otherwise)
    required property int seasonCount
    required property int episodeCount
    required property real seriesProgress
    // The saved playback position in ticks, 0 when there is none
    required property real resumeTicks

    // A 16:9 image area instead of the 2:3 poster
    property bool landscape: false
    // The second text line, the year when empty
    property string subtitle: ""
    // What a screen reader says in place of `subtitle` when that is not readable aloud
    property string spokenSubtitle: subtitle
    // How a library page presents the card: the height of the image area over its width (0: from `landscape`), the image fitted instead of cropped, the title and year lines, and the text footer at all
    property real imageAspect: 0
    // A playable card of Continue Watching: a Play button shows over the poster while the mouse is over the card, and the card menu has the same action
    property bool quickPlay: false
    // Continue Watching: a button in the poster's corner, shown with the Play button, marks the item watched and so takes it off the row; the card menu has the same action
    property bool quickMarkWatched: false
    property bool fitImage: false
    property bool showTitle: true
    property bool showYear: true
    property bool cardLayout: true
    // False for a card whose item has no card menu (a network)
    property bool hasMenu: true
    // The focus ring waits for a user move in a view that starts with a current card (the library grids)
    property bool focusVisible: true

    readonly property real posterAspect: imageAspect > 0 ? imageAspect : (landscape ? 9 / 16 : 3 / 2)
    readonly property string countsLine: seasonCount > 0 && episodeCount > 0 ? i18nc("seasons and episodes of a series", "%1 · %2", i18np("%1 season", "%1 seasons", seasonCount), i18np("%1 episode", "%1 episodes", episodeCount)) : ""
    readonly property string secondLine: countsLine.length > 0 ? countsLine : (subtitle.length > 0 ? subtitle : (year > 0 ? String(year) : ""))

    // The watched share, 0 to 1: a series counts its episodes, anything else its playback position
    readonly property real progress: seriesProgress >= 0 ? seriesProgress : playedPercentage / 100
    readonly property bool inProgress: loaded && progress > 0 && progress < 1
    readonly property real progressHeight: Math.max(2, Kirigami.Units.smallSpacing / 2)
    readonly property bool showsNew: loaded && isNew && (Window.window?.newBadgeEnabled ?? false)

    // The kinds the card menu plays: keep the two lists in step
    readonly property bool playable: ["Movie", "Episode", "Video"].indexOf(type) >= 0
    readonly property bool quickPlayEnabled: quickPlay && loaded && playable
    // A rewatch stopped halfway is in progress, not watched: the server keeps Played on it, the card shows its progress and no check
    readonly property bool watched: played && !(resumeTicks > 0)
    readonly property bool quickMarkWatchedEnabled: quickMarkWatched && loaded && playable && !watched
    // Asks the owner to start the item, from its saved position when it has one
    signal quickPlayRequested
    signal markWatchedRequested

    // Opens the card menu of the window: at the given point of the card, below it when none; from the card's menu button when one is given, below the button and never over it
    function openMenu(x, y, button) {
        if (!card.hasMenu)
            return;
        const resume = card.quickPlayEnabled ? resumeTicks : -1;
        if (button)
            Window.window?.openCardMenuFrom?.(button, card, itemId, type, seriesId, resume, favorite, played);
        else
            Window.window?.openCardMenu?.(card, itemId, type, seriesId, x ?? 0, y ?? card.height, resume, favorite, played);
    }

    // The description lists only what applies to the item, and stays empty for a placeholder row
    readonly property string description: {
        if (!loaded)
            return "";
        const parts = [];
        if (watched)
            parts.push(i18nc("state of an item", "Played"));
        if (favorite)
            parts.push(i18n("Favorite"));
        if (showsNew)
            parts.push(i18nc("badge on a recently added item", "New"));
        if (rating > 0)
            parts.push(i18n("Rated %1 out of 10", Number(rating).toLocaleString(Qt.locale(), "f", 1)));
        if (!played && unplayedCount > 0)
            parts.push(i18np("%1 unplayed item", "%1 unplayed items", unplayedCount));
        if (inProgress)
            parts.push(i18np("%1 percent watched", "%1 percent watched", Math.round(progress * 100)));
        if (remainingMinutes > 0)
            parts.push(i18np("%1 minute left", "%1 minutes left", remainingMinutes));
        return parts.join(", ");
    }

    // The button is for the mouse alone: keyboard focus and touch leave it hidden, the card menu has the same action
    HoverHandler {
        id: cardHover
        objectName: "cardHover"
        enabled: card.quickPlayEnabled || card.quickMarkWatchedEnabled
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
    }

    TapHandler {
        acceptedButtons: Qt.RightButton
        onTapped: eventPoint => {
            card.makeCurrent();
            card.openMenu(eventPoint.position.x, eventPoint.position.y);
        }
    }

    // A click makes this tile the current one and gives its view the focus, so the focus comes back to this tile after Back
    function makeCurrent() {
        const view = ListView.view ?? GridView.view;
        if (!view)
            return;
        view.currentIndex = card.index;
        view.forceActiveFocus(Qt.MouseFocusReason);
    }

    onPressed: makeCurrent()

    // A dark capsule over the poster with an optional icon and a short text, readable on any image
    component Pill: Rectangle {
        id: pill

        property string iconName: ""
        property string text: ""

        implicitWidth: pillRow.implicitWidth + Kirigami.Units.largeSpacing
        implicitHeight: pillRow.implicitHeight + Kirigami.Units.smallSpacing
        radius: height / 2
        color: Qt.rgba(0, 0, 0, 0.6)

        RowLayout {
            id: pillRow
            anchors.centerIn: parent
            spacing: Kirigami.Units.smallSpacing / 2

            Kirigami.Icon {
                Layout.preferredWidth: Kirigami.Units.iconSizes.small
                Layout.preferredHeight: Kirigami.Units.iconSizes.small
                visible: source.toString().length > 0
                source: pill.iconName
                color: "white"
            }

            Controls.Label {
                color: "white"
                font: Kirigami.Theme.smallFont
                textFormat: Text.PlainText
                text: pill.text
            }
        }
    }

    padding: Kirigami.Units.smallSpacing
    Accessible.ignored: !loaded
    Accessible.name: countsLine.length > 0 ? name + ", " + countsLine : (spokenSubtitle.length > 0 ? name + ", " + spokenSubtitle : (year > 0 ? name + " (" + year + ")" : name))
    Accessible.description: description

    // The default delegate background paints the focus ring as a filled highlight; here the ring is only a border, and only once the user moves
    background: Rectangle {
        readonly property bool ring: card.highlighted || (card.visualFocus && card.focusVisible)
        color: card.hovered ? Qt.alpha(Kirigami.Theme.highlightColor, 0.15) : "transparent"
        radius: Kirigami.Units.cornerRadius
        border.width: ring ? 2 : 0
        border.color: Kirigami.Theme.highlightColor
    }

    contentItem: ColumnLayout {
        spacing: 0

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.preferredHeight: width * card.posterAspect
            Layout.bottomMargin: Kirigami.Units.smallSpacing
            color: Kirigami.Theme.alternateBackgroundColor
            radius: Kirigami.Units.cornerRadius
            clip: true

            Kirigami.Icon {
                anchors.centerIn: parent
                width: Kirigami.Units.iconSizes.huge
                height: width
                source: "video-x-generic"
                visible: poster.opacity < 1
            }

            Image {
                id: poster
                anchors.fill: parent
                // The image fades in once loaded, over the fallback icon
                opacity: status === Image.Ready ? 1 : 0
                Behavior on opacity {
                    NumberAnimation { duration: Kirigami.Units.longDuration; easing.type: Easing.OutCubic }
                }
                asynchronous: true
                fillMode: card.fitImage ? Image.PreserveAspectFit : Image.PreserveAspectCrop
                sourceSize.width: width * Screen.devicePixelRatio
                // A placeholder row has no image to fetch. Only URLs built by JellyfinClient::imageUrl belong here: the image fetcher allows nothing else, but a raw server string must never become a source
                source: card.loaded ? card.imageUrl : ""
            }

            // The overlays are built only while they show: a grid fills with dozens of cards at once, and each hidden overlay would still cost its creation
            Loader {
                anchors {
                    left: parent.left
                    right: parent.right
                    bottom: parent.bottom
                }
                height: card.progressHeight
                active: card.inProgress
                sourceComponent: Rectangle {
                    objectName: "cardProgress"
                    color: Qt.rgba(0, 0, 0, 0.6)

                    Rectangle {
                        height: parent.height
                        width: parent.width * card.progress
                        color: Kirigami.Theme.highlightColor
                    }
                }
            }

            RowLayout {
                anchors {
                    left: parent.left
                    right: parent.right
                    bottom: parent.bottom
                    margins: Kirigami.Units.smallSpacing
                    bottomMargin: Kirigami.Units.smallSpacing + (card.inProgress ? card.progressHeight : 0)
                }
                spacing: Kirigami.Units.smallSpacing

                Loader {
                    active: card.loaded && card.rating > 0
                    visible: active
                    sourceComponent: Pill {
                        objectName: "cardRating"
                        iconName: "starred-symbolic"
                        text: card.rating.toLocaleString(Qt.locale(), "f", 1)
                    }
                }

                Item {
                    Layout.fillWidth: true
                }

                Loader {
                    active: card.loaded && card.remainingMinutes > 0
                    visible: active
                    sourceComponent: Pill {
                        objectName: "cardRemaining"
                        text: i18np("%1 min left", "%1 min left", card.remainingMinutes)
                    }
                }
            }

            Row {
                anchors {
                    top: parent.top
                    left: parent.left
                    margins: Kirigami.Units.smallSpacing
                }
                spacing: Kirigami.Units.smallSpacing

                Loader {
                    active: card.showsNew
                    visible: active
                    sourceComponent: Pill {
                        objectName: "cardNew"
                        text: i18nc("badge on a recently added item", "New")
                    }
                }

                Loader {
                    active: card.loaded && card.favorite
                    visible: active
                    sourceComponent: Rectangle {
                        objectName: "cardFavorite"
                        width: Kirigami.Units.iconSizes.smallMedium
                        height: width
                        radius: width / 2
                        color: Qt.rgba(0, 0, 0, 0.6)

                        Kirigami.Icon {
                            anchors.fill: parent
                            anchors.margins: Kirigami.Units.smallSpacing / 2
                            source: "emblem-favorite"
                        }
                    }
                }
            }

            Loader {
                anchors {
                    top: parent.top
                    right: parent.right
                    margins: Kirigami.Units.smallSpacing
                }
                active: card.loaded && card.watched
                sourceComponent: Rectangle {
                    objectName: "cardPlayed"
                    width: Kirigami.Units.iconSizes.smallMedium
                    height: width
                    radius: width / 2
                    color: Kirigami.Theme.highlightColor

                    Kirigami.Icon {
                        anchors.fill: parent
                        anchors.margins: Kirigami.Units.smallSpacing / 2
                        source: "checkmark-symbolic"
                        color: Kirigami.Theme.highlightedTextColor
                    }
                }
            }

            Loader {
                anchors {
                    top: parent.top
                    right: parent.right
                    margins: Kirigami.Units.smallSpacing
                }
                active: card.loaded && !card.played && card.unplayedCount > 0
                sourceComponent: Rectangle {
                    objectName: "cardUnplayed"
                    width: Math.max(height, unplayedLabel.implicitWidth + Kirigami.Units.largeSpacing)
                    height: unplayedLabel.implicitHeight + Kirigami.Units.smallSpacing
                    radius: height / 2
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

            // The Mark as Watched button of a Continue Watching card, in the corner where the watched mark shows, with the Play button while the mouse is over the card
            Loader {
                anchors {
                    top: parent.top
                    right: parent.right
                    margins: Kirigami.Units.smallSpacing
                }
                width: Kirigami.Units.iconSizes.medium
                height: width
                active: card.quickMarkWatchedEnabled
                sourceComponent: Controls.RoundButton {
                    id: markWatchedButton
                    objectName: "cardMarkWatched"
                    opacity: cardHover.hovered ? 1 : 0
                    visible: opacity > 0
                    hoverEnabled: true
                    focusPolicy: Qt.NoFocus
                    display: Controls.AbstractButton.IconOnly
                    icon.name: "checkmark-symbolic"
                    icon.color: "white"
                    text: i18n("Mark as Watched")
                    // The card name is plain server text
                    Accessible.name: i18nc("@action:button %1 is the name of a movie or an episode", "Mark %1 as Watched", card.name)
                    Controls.ToolTip.text: text
                    Controls.ToolTip.visible: hovered
                    Controls.ToolTip.delay: Kirigami.Units.toolTipDelay
                    padding: 0
                    background: Rectangle {
                        radius: width / 2
                        color: markWatchedButton.hovered || markWatchedButton.down ? Kirigami.Theme.highlightColor : Qt.rgba(0, 0, 0, 0.6)

                        Behavior on color {
                            ColorAnimation {
                                duration: Kirigami.Units.shortDuration
                            }
                        }
                    }
                    contentItem: Item {
                        Kirigami.Icon {
                            x: Math.round((parent.width - width) / 2)
                            y: Math.round((parent.height - height) / 2)
                            width: Math.round(markWatchedButton.width * 0.6)
                            height: width
                            source: markWatchedButton.icon.name
                            isMask: true
                            color: markWatchedButton.icon.color
                        }
                    }

                    Behavior on opacity {
                        NumberAnimation {
                            duration: Kirigami.Units.longDuration
                            easing.type: Easing.InOutQuad
                        }
                    }

                    onClicked: card.markWatchedRequested()
                }
            }

            // Only a Continue Watching card has the button
            Loader {
                readonly property real diameter: Math.round(Math.min(parent.width, parent.height) * 0.4)
                anchors.centerIn: parent
                width: Math.max(diameter, Kirigami.Units.iconSizes.large)
                height: width
                active: card.quickPlayEnabled
                sourceComponent: Controls.RoundButton {
                    id: quickPlayButton
                    objectName: "cardQuickPlay"
                    // Fades in while the pointer is over the card and lights up while it is over the button itself, as streaming players do
                    opacity: cardHover.hovered ? 1 : 0
                    visible: opacity > 0
                    scale: hovered || down ? 1.08 : 1
                    hoverEnabled: true
                    focusPolicy: Qt.NoFocus
                    display: Controls.AbstractButton.IconOnly
                    icon.name: "media-playback-start"
                    // Over the poster, like the pills: white on a dark disc, the highlight colour under the pointer
                    icon.color: "white"
                    background: Rectangle {
                        radius: width / 2
                        color: quickPlayButton.hovered || quickPlayButton.down ? Kirigami.Theme.highlightColor : Qt.rgba(0, 0, 0, 0.6)

                        Behavior on color {
                            ColorAnimation {
                                duration: Kirigami.Units.shortDuration
                            }
                        }
                    }

                    Behavior on opacity {
                        NumberAnimation {
                            duration: Kirigami.Units.longDuration
                            easing.type: Easing.InOutQuad
                        }
                    }
                    Behavior on scale {
                        NumberAnimation {
                            duration: Kirigami.Units.shortDuration
                        }
                    }
                    padding: 0
                    // The glyph drawn here, not by the style, which offsets it inside its own margins
                    contentItem: Item {
                        Kirigami.Icon {
                            // Whole pixels, or an odd button blurs the glyph. Breeze's triangle fills its box but its weight sits left of the middle: a small shift to the right centres it to the eye
                            x: Math.round((parent.width - width) / 2 + width * 0.07)
                            y: Math.round((parent.height - height) / 2)
                            width: Math.round(quickPlayButton.width / 2)
                            height: width
                            source: quickPlayButton.icon.name
                            isMask: true
                            color: quickPlayButton.icon.color
                        }
                    }
                    text: card.resumeTicks > 0 ? i18n("Resume") : i18n("Play")
                    // No tooltip: the disc on the poster says it all and a popup covers the card. The card name is plain server text
                    Accessible.name: card.resumeTicks > 0 ? i18n("Resume %1", card.name) : i18n("Play %1", card.name)

                    // The press never reaches the card, so the card becomes current here and Back returns to it
                    onClicked: {
                        card.makeCurrent();
                        card.quickPlayRequested();
                    }
                }
            }

            // The poster covers a border drawn on this rectangle: the contour is a transparent overlay on top
            Rectangle {
                anchors.fill: parent
                color: "transparent"
                radius: Kirigami.Units.cornerRadius
                border.width: 1
                border.color: Qt.alpha(Kirigami.Theme.textColor, 0.3)
                Accessible.ignored: true
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 0
            visible: card.cardLayout

            // Item names come from the server: plain text only
            Controls.Label {
                id: nameLabel
                Layout.fillWidth: true
                visible: card.showTitle
                elide: Text.ElideRight
                maximumLineCount: 1
                textFormat: Text.PlainText
                text: card.loaded ? card.name : ""
            }

            // Without the title the button keeps the right edge
            Item {
                Layout.fillWidth: true
                visible: !nameLabel.visible
            }

            // The pointer and keyboard ways to the menu are the right click and the Menu key: this button is for the pointer alone, and the line keeps its height
            Controls.ToolButton {
                id: menuButton
                objectName: "cardMenuButton"
                Layout.preferredWidth: Math.round(nameLabel.implicitHeight)
                Layout.preferredHeight: Math.round(nameLabel.implicitHeight)
                focusPolicy: Qt.NoFocus
                padding: 0
                leftPadding: 0
                rightPadding: 0
                topPadding: 0
                bottomPadding: 0
                visible: card.loaded && card.hasMenu
                display: Controls.AbstractButton.IconOnly
                // No icon of the button's own: the desktop style paints it in the background, off centre in a button this small, and the glyph below would be a second row of dots
                contentItem: Item {
                    Kirigami.Icon {
                        objectName: "cardMenuGlyph"
                        x: Math.round((parent.width - width) / 2)
                        y: Math.round((parent.height - height) / 2)
                        width: Kirigami.Units.iconSizes.small
                        height: Kirigami.Units.iconSizes.small
                        source: "overflow-menu"
                    }
                }
                // No text either: with no icon the style would paint it, elided to a row of dots. The tooltip has the short text, a screen reader hears which card the button belongs to
                Accessible.name: i18n("More Actions for %1", card.name)
                Controls.ToolTip.text: i18n("More Actions")
                Controls.ToolTip.visible: hovered
                Controls.ToolTip.delay: Kirigami.Units.toolTipDelay
                onClicked: card.openMenu(0, 0, menuButton)
            }
        }

        Controls.Label {
            Layout.fillWidth: true
            visible: card.cardLayout && card.showYear
            elide: Text.ElideRight
            maximumLineCount: 1
            opacity: 0.7
            textFormat: Text.PlainText
            text: card.loaded ? card.secondLine : ""
        }
    }
}
