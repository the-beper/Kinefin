// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Kirigami.ScrollablePage {
    id: page
    objectName: "itemDetailsPage"

    required property ConnectionController connection
    required property string itemId

    // The model is a child of this page and starts loading at once
    readonly property ItemDetailsModel details: connection.createItemDetails(itemId, page)
    // A reload keeps the old data on screen: the content stays up while it has a name, except for an item the server no longer has
    readonly property bool hasData: details.name.length > 0
    readonly property bool showContent: details.status === ItemDetailsModel.Ready || (hasData && details.status !== ItemDetailsModel.NotFound)
    readonly property bool showLoading: !showContent && (details.status === ItemDetailsModel.Loading || details.status === ItemDetailsModel.Idle)
    // The images are dropped while the page is out of view, which frees their textures
    readonly property bool inView: Kirigami.ColumnView.inViewport
    readonly property bool showSimilar: details.similarStatus === ItemDetailsModel.Loading || details.similarStatus === ItemDetailsModel.Failed || (details.similarStatus === ItemDetailsModel.Ready && details.similar.count > 0)

    // The poster ratios match MediaCard: the height is the poster plus the two text lines
    readonly property real personCardWidth: Kirigami.Units.gridUnit * 7
    readonly property real personCardHeight: Math.round((personCardWidth - Kirigami.Units.smallSpacing * 2) * 3 / 2) + Kirigami.Units.gridUnit * 3.5
    readonly property real similarCardWidth: Kirigami.Units.gridUnit * (details.similar.landscape ? 14 : 8)
    readonly property real similarCardHeight: Math.round((similarCardWidth - Kirigami.Units.smallSpacing * 2) * (details.similar.landscape ? 9 / 16 : 3 / 2)) + Kirigami.Units.gridUnit * 3.5

    readonly property string metaText: {
        const parts = [];
        if (details.episodeNumber >= 0)
            parts.push(details.seasonNumber >= 0 ? i18nc("season and episode number", "S%1:E%2", details.seasonNumber, details.episodeNumber) : i18nc("episode number", "E%1", details.episodeNumber));
        if (details.year > 0)
            parts.push(String(details.year));
        if (details.runtimeMinutes > 0)
            parts.push(runtimeText(details.runtimeMinutes));
        if (details.officialRating.length > 0)
            parts.push(details.officialRating);
        return parts.join(i18nc("separator between metadata items", " · "));
    }
    readonly property string stateText: {
        const parts = [];
        if (details.played)
            parts.push(i18nc("state of an item", "Played"));
        if (details.favorite)
            parts.push(i18n("Favorite"));
        return parts.join(i18nc("separator between items of a list", ", "));
    }

    title: details.name
    // Take the width the previous page leaves instead of a fixed column
    Kirigami.ColumnView.fillWidth: true

    // The kinds of crew the server names with its own words, for a person with no role
    function personKind(type) {
        switch (type) {
        case "Actor":
            return i18nc("kind of cast or crew member", "Actor");
        case "GuestStar":
            return i18nc("kind of cast or crew member", "Guest star");
        case "Director":
            return i18nc("kind of cast or crew member", "Director");
        case "Writer":
            return i18nc("kind of cast or crew member", "Writer");
        case "Producer":
            return i18nc("kind of cast or crew member", "Producer");
        case "Composer":
            return i18nc("kind of cast or crew member", "Composer");
        default:
            return "";
        }
    }

    function runtimeText(minutes) {
        return minutes >= 60 ? i18n("%1 h %2 min", Math.floor(minutes / 60), minutes % 60) : i18n("%1 min", minutes);
    }

    // One line per stream: codec, resolution or channels, language, title and flags
    function streamText(stream, kind) {
        const parts = [];
        if (stream.codec.length > 0)
            parts.push(stream.codec.toUpperCase());
        if (kind === "video" && stream.width > 0 && stream.height > 0)
            parts.push(stream.width + "×" + stream.height);
        if (kind === "audio" && stream.channels > 0)
            parts.push(i18np("%1 channel", "%1 channels", stream.channels));
        if (stream.language.length > 0)
            parts.push(stream.language);
        if (stream.title.length > 0)
            parts.push(stream.title);
        if (stream.isDefault)
            parts.push(i18n("Default"));
        if (stream.isForced)
            parts.push(i18n("Forced"));
        if (stream.isExternal)
            parts.push(i18n("External"));
        return parts.join(" · ");
    }

    // The rows of the media info form of one source: label and value lines
    function sourceRows(source) {
        const rows = [];
        if (source.container.length > 0)
            rows.push({ label: i18n("Container"), text: source.container });
        if (source.size > 0)
            rows.push({ label: i18n("Size"), text: Qt.locale().formattedDataSize(source.size, 1) });
        if (source.bitrate > 0)
            rows.push({ label: i18n("Bitrate"), text: i18n("%1 Mbps", (source.bitrate / 1000000).toLocaleString(Qt.locale(), "f", 1)) });
        const streams = [
            { label: i18n("Video"), kind: "video", list: source.video },
            { label: i18n("Audio"), kind: "audio", list: source.audio },
            { label: i18n("Subtitles"), kind: "subtitles", list: source.subtitles }
        ];
        for (const entry of streams) {
            if (entry.list.length > 0)
                rows.push({ label: entry.label, text: entry.list.map(stream => streamText(stream, entry.kind)).join("\n") });
        }
        return rows;
    }

    actions: [
        Kirigami.Action {
            text: page.details.canResume ? i18n("Resume") : i18n("Play")
            icon.name: "media-playback-start"
            visible: page.details.playable
            enabled: false
            tooltip: i18n("Playback is not available yet")
        },
        Kirigami.Action {
            text: i18n("Refresh")
            icon.name: "view-refresh"
            // Home binds the same key: only the page in front answers it, the button stays usable
            shortcut: page.isCurrentPage ? StandardKey.Refresh : ""
            onTriggered: page.details.reload()
        }
    ]

    ColumnLayout {
        spacing: Kirigami.Units.largeSpacing

        Kirigami.LoadingPlaceholder {
            Layout.fillWidth: true
            visible: page.showLoading
            text: i18n("Loading…")
        }

        MessageBox {
            visible: page.details.status === ItemDetailsModel.Failed
            type: Kirigami.MessageType.Error
            text: i18n("Could not load this item: check the connection to the server and try again")
            actions: [
                Kirigami.Action {
                    text: i18n("Retry")
                    icon.name: "view-refresh"
                    onTriggered: page.details.reload()
                }
            ]
        }

        Kirigami.PlaceholderMessage {
            Layout.fillWidth: true
            visible: page.details.status === ItemDetailsModel.NotFound
            icon.name: "video-x-generic"
            text: i18n("This item is not available")
            explanation: i18n("It may have been removed from the server")
        }

        ColumnLayout {
            Layout.fillWidth: true
            visible: page.showContent
            spacing: Kirigami.Units.largeSpacing

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(width * 9 / 16, Kirigami.Units.gridUnit * 20)
                visible: page.details.backdropUrl.toString().length > 0 || page.details.logoUrl.toString().length > 0
                color: Kirigami.Theme.alternateBackgroundColor
                clip: true

                // Only URLs built by JellyfinClient::imageUrl belong here: the image fetcher allows nothing else, but a raw server string must never become a source
                Image {
                    anchors.fill: parent
                    asynchronous: true
                    fillMode: Image.PreserveAspectCrop
                    sourceSize.width: width * Screen.devicePixelRatio
                    source: page.inView ? page.details.backdropUrl : ""
                    Accessible.ignored: true
                }

                Image {
                    anchors {
                        left: parent.left
                        bottom: parent.bottom
                        margins: Kirigami.Units.largeSpacing
                    }
                    width: Math.min(parent.width / 2, Kirigami.Units.gridUnit * 16)
                    height: Math.min(parent.height / 3, Kirigami.Units.gridUnit * 6)
                    asynchronous: true
                    fillMode: Image.PreserveAspectFit
                    horizontalAlignment: Image.AlignLeft
                    verticalAlignment: Image.AlignBottom
                    sourceSize.width: width * Screen.devicePixelRatio
                    source: page.inView ? page.details.logoUrl : ""
                    Accessible.ignored: true
                }
            }

            Kirigami.Heading {
                Layout.fillWidth: true
                level: 1
                wrapMode: Text.Wrap
                // Item names come from the server: plain text only
                textFormat: Text.PlainText
                text: page.details.name
            }

            Kirigami.LinkButton {
                Layout.fillWidth: true
                Layout.maximumWidth: implicitWidth
                visible: page.details.seriesName.length > 0 && page.details.seriesId.length > 0
                horizontalAlignment: Text.AlignLeft
                textFormat: Text.PlainText
                text: page.details.seriesName
                onClicked: page.Window.window.openItem(page.details.seriesId, page)
            }

            Controls.Label {
                Layout.fillWidth: true
                visible: page.details.seriesName.length > 0 && page.details.seriesId.length === 0
                wrapMode: Text.Wrap
                textFormat: Text.PlainText
                text: page.details.seriesName
            }

            Flow {
                Layout.fillWidth: true
                spacing: Kirigami.Units.largeSpacing
                visible: metaLabel.visible || ratingRow.visible || stateLabel.visible

                Controls.Label {
                    id: metaLabel
                    visible: text.length > 0
                    textFormat: Text.PlainText
                    text: page.metaText
                }

                RowLayout {
                    id: ratingRow
                    visible: page.details.communityRating > 0
                    spacing: Kirigami.Units.smallSpacing
                    Accessible.role: Accessible.StaticText
                    Accessible.name: i18n("Community Rating")
                    Accessible.description: ratingLabel.text

                    Kirigami.Icon {
                        Layout.preferredWidth: Kirigami.Units.iconSizes.small
                        Layout.preferredHeight: Kirigami.Units.iconSizes.small
                        source: "starred-symbolic"
                        Accessible.ignored: true
                    }

                    Controls.Label {
                        id: ratingLabel
                        textFormat: Text.PlainText
                        text: page.details.communityRating.toLocaleString(Qt.locale(), "f", 1)
                    }
                }

                Controls.Label {
                    id: stateLabel
                    visible: text.length > 0
                    opacity: 0.7
                    textFormat: Text.PlainText
                    text: page.stateText
                }
            }

            Controls.Label {
                Layout.fillWidth: true
                visible: text.length > 0
                wrapMode: Text.Wrap
                font.italic: true
                textFormat: Text.PlainText
                text: page.details.tagline
            }

            Controls.Label {
                Layout.fillWidth: true
                visible: page.details.directors.length > 0
                wrapMode: Text.Wrap
                textFormat: Text.PlainText
                text: i18np("Director: %2", "Directors: %2", page.details.directors.length, page.details.directors.join(", "))
            }

            Controls.Label {
                Layout.fillWidth: true
                visible: page.details.writers.length > 0
                wrapMode: Text.Wrap
                textFormat: Text.PlainText
                text: i18np("Writer: %2", "Writers: %2", page.details.writers.length, page.details.writers.join(", "))
            }

            Controls.Label {
                Layout.fillWidth: true
                visible: page.details.genres.length > 0
                wrapMode: Text.Wrap
                textFormat: Text.PlainText
                text: i18np("Genre: %2", "Genres: %2", page.details.genres.length, page.details.genres.join(", "))
            }

            Controls.Label {
                Layout.fillWidth: true
                visible: text.length > 0
                Layout.maximumWidth: Kirigami.Units.gridUnit * 35
                wrapMode: Text.Wrap
                textFormat: Text.PlainText
                text: page.details.overview
            }

            Kirigami.Heading {
                Layout.fillWidth: true
                visible: page.details.people.length > 0
                level: 2
                text: i18n("Cast and Crew")
            }

            CardRow {
                visible: page.details.people.length > 0
                model: page.details.people
                cardHeight: page.personCardHeight
                accessibleName: i18n("Cast and Crew")
                pageFlickable: page.flickable

                delegate: Controls.ItemDelegate {
                    id: person

                    required property var modelData

                    // The role of an actor, or the kind of crew when the server gives no role
                    readonly property string subtitle: modelData.role.length > 0 ? modelData.role : page.personKind(modelData.type)

                    width: page.personCardWidth
                    height: page.personCardHeight
                    // The list is the one tab stop of its row, and a person has nothing to open
                    focusPolicy: Qt.NoFocus
                    hoverEnabled: false
                    highlighted: ListView.isCurrentItem && ListView.view.activeFocus
                    padding: Kirigami.Units.smallSpacing
                    Accessible.name: subtitle.length > 0 ? modelData.name + ", " + subtitle : modelData.name

                    contentItem: ColumnLayout {
                        spacing: 0

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            Layout.preferredHeight: width * 3 / 2
                            Layout.bottomMargin: Kirigami.Units.smallSpacing
                            radius: Kirigami.Units.cornerRadius
                            color: Kirigami.Theme.alternateBackgroundColor

                            Kirigami.Icon {
                                anchors.centerIn: parent
                                width: Kirigami.Units.iconSizes.huge
                                height: width
                                source: "user-identity"
                                visible: photo.status !== Image.Ready
                            }

                            Kirigami.ShadowedImage {
                                id: photo
                                anchors.fill: parent
                                radius: parent.radius
                                asynchronous: true
                                fillMode: Image.PreserveAspectCrop
                                sourceSize.width: width * Screen.devicePixelRatio
                                // Only URLs built by JellyfinClient::imageUrl belong here
                                source: page.inView ? person.modelData.imageUrl : ""
                                Accessible.ignored: true
                            }
                        }

                        // Names and roles come from the server: plain text only
                        Controls.Label {
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                            maximumLineCount: 1
                            textFormat: Text.PlainText
                            text: person.modelData.name
                        }

                        Controls.Label {
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                            maximumLineCount: 1
                            opacity: 0.7
                            textFormat: Text.PlainText
                            text: person.subtitle
                        }
                    }
                }
            }

            Kirigami.Heading {
                Layout.fillWidth: true
                visible: page.details.chapters.length > 0
                level: 2
                text: i18n("Chapters")
            }

            ColumnLayout {
                Layout.fillWidth: true
                visible: page.details.chapters.length > 0
                spacing: Kirigami.Units.smallSpacing

                Repeater {
                    model: page.details.chapters
                    delegate: RowLayout {
                        id: chapter
                        required property var modelData

                        Layout.fillWidth: true
                        spacing: Kirigami.Units.largeSpacing
                        Accessible.role: Accessible.StaticText
                        Accessible.name: chapter.modelData.time + " " + chapter.modelData.name

                        Controls.Label {
                            Layout.preferredWidth: Kirigami.Units.gridUnit * 4
                            opacity: 0.7
                            textFormat: Text.PlainText
                            text: chapter.modelData.time
                        }

                        Controls.Label {
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            textFormat: Text.PlainText
                            text: chapter.modelData.name
                        }
                    }
                }
            }

            Kirigami.Heading {
                Layout.fillWidth: true
                visible: page.details.mediaSources.length > 0
                level: 2
                text: i18n("Media Info")
            }

            Repeater {
                model: page.details.mediaSources
                delegate: ColumnLayout {
                    id: mediaSource
                    required property var modelData

                    Layout.fillWidth: true
                    spacing: Kirigami.Units.smallSpacing

                    Controls.Label {
                        Layout.fillWidth: true
                        visible: text.length > 0
                        wrapMode: Text.Wrap
                        font.bold: true
                        textFormat: Text.PlainText
                        text: mediaSource.modelData.name
                    }

                    Kirigami.FormLayout {
                        Layout.fillWidth: true

                        Repeater {
                            model: page.sourceRows(mediaSource.modelData)
                            delegate: Controls.Label {
                                required property var modelData

                                Kirigami.FormData.label: modelData.label
                                Kirigami.FormData.labelAlignment: Qt.AlignTop
                                Layout.fillWidth: true
                                wrapMode: Text.Wrap
                                textFormat: Text.PlainText
                                text: modelData.text
                            }
                        }
                    }
                }
            }

            Kirigami.Heading {
                Layout.fillWidth: true
                visible: page.showSimilar
                level: 2
                text: i18n("More Like This")
            }

            Controls.BusyIndicator {
                Layout.preferredWidth: Kirigami.Units.gridUnit * 2
                Layout.preferredHeight: Kirigami.Units.gridUnit * 2
                visible: page.details.similarStatus === ItemDetailsModel.Loading
                running: visible
                Accessible.name: i18n("Loading…")
            }

            MessageBox {
                visible: page.details.similarStatus === ItemDetailsModel.Failed
                type: Kirigami.MessageType.Error
                text: i18n("Could not load similar items")
                actions: [
                    Kirigami.Action {
                        text: i18n("Retry")
                        icon.name: "view-refresh"
                        onTriggered: page.details.retrySimilar()
                    }
                ]
            }

            CardRow {
                visible: page.details.similar.count > 0
                model: page.details.similar
                cardHeight: page.similarCardHeight
                accessibleName: i18n("More Like This")
                pageFlickable: page.flickable

                delegate: MediaCard {
                    width: page.similarCardWidth
                    height: page.similarCardHeight
                    // The list is the one tab stop of its row
                    focusPolicy: Qt.NoFocus
                    landscape: page.details.similar.landscape
                    loaded: true
                    highlighted: ListView.isCurrentItem && ListView.view.activeFocus
                    onClicked: page.Window.window.openItem(itemId, page)
                }
            }
        }
    }
}
