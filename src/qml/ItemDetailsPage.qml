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
    // The kinds the play queue takes: what plays, and what stands for a run of it
    readonly property bool queueable: ["Movie", "Episode", "Video", "Season", "Series", "BoxSet", "Playlist"].indexOf(details.type) >= 0
    readonly property bool showContent: details.status === ItemDetailsModel.Ready || (hasData && details.status !== ItemDetailsModel.NotFound)
    readonly property bool showLoading: !showContent && (details.status === ItemDetailsModel.Loading || details.status === ItemDetailsModel.Idle)
    // The images are dropped while the page is out of view, which frees their textures
    readonly property bool inView: Kirigami.ColumnView.inViewport
    readonly property bool showSimilar: details.similarStatus === ItemDetailsModel.Loading || details.similarStatus === ItemDetailsModel.Failed || (details.similarStatus === ItemDetailsModel.Ready && details.similar.count > 0)

    // Set by the first Media Info action: the Loader then builds the dialog, which hands itself back in mediaInfoDialog
    property bool mediaInfoWanted: false
    property MediaInfoDialog mediaInfoDialog: null

    // What the main menu's Refresh calls while this page is in front
    function refreshPage() {
        details.reload();
    }

    // The space the page keeps around its content
    readonly property real margin: Kirigami.Units.largeSpacing
    readonly property ItemChildrenModel childModel: details.below
    // Empty when the series has no episode to continue with
    readonly property var nextUp: childModel.nextUp
    readonly property bool showSeriesLink: details.seriesName.length > 0 && details.seriesId.length > 0
    readonly property bool showSeasonLink: details.seasonId.length > 0

    // The poster ratios match MediaCard: the height is the poster plus the two text lines
    readonly property real posterCardWidth: Kirigami.Units.gridUnit * 8
    readonly property real posterCardHeight: Math.round((posterCardWidth - Kirigami.Units.smallSpacing * 2) * 3 / 2) + Kirigami.Units.gridUnit * 3.5
    readonly property real personCardWidth: Kirigami.Units.gridUnit * 7
    readonly property real personCardHeight: Math.round((personCardWidth - Kirigami.Units.smallSpacing * 2) * 3 / 2) + Kirigami.Units.gridUnit * 3.5
    readonly property real similarCardWidth: Kirigami.Units.gridUnit * (details.similar.landscape ? 14 : 8)
    readonly property real similarCardHeight: Math.round((similarCardWidth - Kirigami.Units.smallSpacing * 2) * (details.similar.landscape ? 9 / 16 : 3 / 2)) + Kirigami.Units.gridUnit * 3.5

    readonly property bool isSeries: details.type === "Series"
    readonly property bool isSeason: details.type === "Season"
    readonly property bool showSeasonCount: childModel.kind === ItemChildrenModel.Series && childModel.seasons.count > 0
    readonly property bool showEpisodeCount: childModel.kind === ItemChildrenModel.Season && childModel.episodes.status === SeasonEpisodesModel.Ready && childModel.episodes.totalCount > 0
    // Runtime and the end time belong to what plays
    readonly property bool showsEndsAt: details.playable && details.runtimeMinutes > 0
    property bool overviewExpanded: false
    // The clock the end time counts from: it moves once a minute while the page is in view
    property var now: new Date()

    readonly property string episodeCode: {
        if (details.type === "Season" || details.episodeNumber < 0)
            return "";
        return EpisodeLabel.label(details.seasonNumber, details.episodeNumber, details.episodeNumberEnd);
    }
    readonly property int startYear: details.year > 0 ? details.year : (dateOf(details.premiereDate)?.getFullYear() ?? 0)
    readonly property int endYear: dateOf(details.endDate)?.getFullYear() ?? 0
    // A running series reads "2015–present", an ended one "2015–2022"; an episode shows the day it aired
    readonly property string yearText: {
        if (isSeries && startYear > 0) {
            if (details.seriesStatus === "Continuing")
                return i18nc("years of a series that is still running", "%1–present", String(startYear));
            return endYear > startYear ? i18nc("years of a series", "%1–%2", String(startYear), String(endYear)) : String(startYear);
        }
        const aired = dateOf(details.premiereDate);
        if (details.type === "Episode" && aired)
            return aired.toLocaleDateString(Qt.locale(), Locale.ShortFormat);
        return details.year > 0 ? String(details.year) : "";
    }
    // The status the years do not already say
    readonly property string statusText: {
        if (details.seriesStatus === "Unreleased")
            return i18nc("status of a series", "Unreleased");
        if (details.seriesStatus === "Ended" && !(endYear > startYear))
            return i18nc("status of a series", "Ended");
        return "";
    }

    // Play is the hero's button, the others are page actions
    readonly property Kirigami.Action playAction: Kirigami.Action {
        objectName: "playAction"
        text: page.details.canResume ? i18n("Resume") : i18n("Play")
        icon.name: "media-playback-start"
        visible: page.details.playable
        enabled: page.details.status === ItemDetailsModel.Ready && !!page.Window.window?.player
        onTriggered: page.Window.window.playItem(page.details.itemId, page.details.canResume ? page.details.resumePositionTicks : 0)
    }
    readonly property Kirigami.Action playOnAction: Kirigami.Action {
        objectName: "playOnAction"
        text: i18nc("@action:button", "Cast…")
        icon.name: "kinefin-cast"
        visible: page.details.status === ItemDetailsModel.Ready && page.details.playable
        enabled: !!page.Window.window?.player && !page.Window.window?.groupRouting
        tooltip: page.Window.window?.groupRouting ? i18nc("@info:tooltip", "Not available while following a SyncPlay group") : ""
        onTriggered: page.Window.window.playOn(page.details.itemId, page.details.canResume ? page.details.resumePositionTicks : 0)
    }
    readonly property Kirigami.Action playNextAction: Kirigami.Action {
        objectName: "playNextAction"
        text: i18n("Play Next")
        icon.name: "media-skip-forward"
        visible: page.queueable
        // A SyncPlay group's queue does not wait for the user's own
        enabled: !!page.Window.window?.groupRouting || !page.connection.queue.busy
        onTriggered: page.Window.window?.queueItem(page.details.itemId, true)
    }
    readonly property Kirigami.Action addToQueueAction: Kirigami.Action {
        objectName: "addToQueueAction"
        text: i18n("Add to Queue")
        icon.name: "list-add"
        visible: page.queueable
        enabled: !!page.Window.window?.groupRouting || !page.connection.queue.busy
        onTriggered: page.Window.window?.queueItem(page.details.itemId, false)
    }
    readonly property Kirigami.Action addToPlaylistAction: Kirigami.Action {
        objectName: "addToPlaylistAction"
        text: i18n("Add to Playlist…")
        icon.name: "media-playlist-append"
        visible: page.details.status === ItemDetailsModel.Ready && page.details.playable
        onTriggered: page.Window.window.addToPlaylist(page.details.itemId)
    }
    // The download record of this item for the signed-in user; the engine owns it, the page only names the action
    readonly property int downloadsRevision: page.connection.downloads.revision
    readonly property string downloadState: {
        downloadsRevision; // the binding follows every queue change
        return page.connection.downloads.canDownload && page.details.status === ItemDetailsModel.Ready ? page.connection.downloads.downloadState(page.details.itemId) : "";
    }
    readonly property bool downloadable: page.connection.downloads.canDownload && page.details.canDownload && ["Movie", "Episode", "Video"].indexOf(page.details.type) >= 0
    readonly property bool downloadCancellable: ["queued", "fetching", "waiting"].indexOf(downloadState) >= 0
    readonly property bool downloadRemovable: ["paused", "error", "completed", "exported"].indexOf(downloadState) >= 0
    readonly property Kirigami.Action downloadAction: Kirigami.Action {
        objectName: "downloadAction"
        text: page.downloadRemovable ? i18n("Delete Download…") : page.downloadCancellable ? i18n("Cancel Download") : i18n("Download")
        icon.name: page.downloadRemovable ? "edit-delete" : page.downloadCancellable ? "process-stop" : "folder-download"
        visible: page.downloadable
        onTriggered: {
            if (page.downloadRemovable)
                page.Window.window.confirmRemoveDownload(page.details.itemId);
            else if (page.downloadCancellable)
                page.connection.downloads.removeCurrentDownload(page.details.itemId);
            else
                page.Window.window.askDownload(page.details.itemId);
        }
    }
    // Automatic downloads of a series or a season; the rule store has one rule per series
    readonly property int rulesRevision: page.connection.downloads.rulesRevision
    readonly property string ruleSeriesId: page.details.type === "Series" ? page.details.itemId : page.details.seriesId
    readonly property var seriesRule: {
        rulesRevision; // the binding follows the rule store
        return ruleSeriesId.length > 0 ? page.connection.downloads.seriesRule(ruleSeriesId) : ({});
    }
    readonly property Kirigami.Action automaticDownloadsAction: Kirigami.Action {
        objectName: "automaticDownloadsAction"
        text: page.seriesRule.kind ? i18n("Edit Automatic Downloads…") : i18n("Automatic Downloads…")
        icon.name: "folder-download"
        visible: page.details.status === ItemDetailsModel.Ready
                 && (page.details.type === "Series" || page.details.type === "Season")
                 && page.connection.downloads.canDownload
        onTriggered: ruleDialog.open()
    }
    readonly property Kirigami.Action stopDownloadsAction: Kirigami.Action {
        objectName: "stopDownloadsAction"
        text: i18n("Stop Automatic Downloads")
        icon.name: "edit-delete-remove"
        visible: page.automaticDownloadsAction.visible && page.seriesRule.kind !== undefined
        onTriggered: page.connection.downloads.removeSeriesRule(page.ruleSeriesId)
    }
    RuleDialog {
        id: ruleDialog
        downloads: page.connection.downloads
        seriesId: page.ruleSeriesId
        seriesName: page.details.type === "Series" ? page.details.name : page.details.seriesName
        seasonNumber: page.details.seasonNumber
        currentKind: page.seriesRule.kind ?? "nextN"
        storedSeason: page.seriesRule.seasonNumber ?? -1
        currentCount: page.seriesRule.count ?? 3
    }
    readonly property Kirigami.Action favoriteAction: Kirigami.Action {
        objectName: "toggleFavoriteAction"
        text: page.details.favorite ? i18n("Remove from Favorites") : i18n("Add to Favorites")
        // The icon shows the state too: a narrow toolbar may show the icon alone
        icon.name: page.details.favorite ? "starred-symbolic" : "non-starred-symbolic"
        visible: page.details.status === ItemDetailsModel.Ready
        enabled: !page.details.favoriteBusy
        onTriggered: page.details.toggleFavorite()
    }

    title: details.name

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

    // The local date of a "yyyy-MM-dd" string, null for anything else
    function dateOf(iso) {
        const parts = iso.split("-");
        return parts.length === 3 ? new Date(Number(parts[0]), Number(parts[1]) - 1, Number(parts[2])) : null;
    }

    // When the item would end if it played from the resume point at `now`, as a clock time; empty when it has no runtime or nothing is left
    function endsAtText(now) {
        if (!showsEndsAt)
            return "";
        const resumed = details.canResume ? details.resumePositionTicks / 600000000 : 0;
        const remaining = details.runtimeMinutes - resumed;
        if (remaining <= 0)
            return "";
        const end = new Date(now.getTime() + remaining * 60000);
        // A clock time to the minute, whatever the locale's short format shows
        end.setSeconds(0, 0);
        return i18n("Ends at %1", end.toLocaleTimeString(Qt.locale(), Locale.ShortFormat));
    }

    readonly property Kirigami.Action togglePlayedAction: Kirigami.Action {
        objectName: "togglePlayedAction"
        // A rewatch stopped halfway is in progress, not watched, as on its card
        text: page.details.played && !page.details.canResume ? i18n("Mark as Unwatched") : i18n("Mark as Watched")
        icon.name: page.details.played && !page.details.canResume ? "view-hidden" : "checkmark-symbolic"
        visible: page.details.status === ItemDetailsModel.Ready && page.details.playable
        enabled: !page.details.playedBusy
        onTriggered: page.details.togglePlayed()
    }
    readonly property Kirigami.Action mediaInfoAction: Kirigami.Action {
        objectName: "mediaInfoAction"
        text: i18n("Media Info")
        icon.name: "documentinfo"
        visible: page.details.mediaSources.length > 0
        onTriggered: {
            if (page.mediaInfoDialog)
                page.mediaInfoDialog.open();
            else
                page.mediaInfoWanted = true;
        }
    }

    actions: [page.playOnAction, page.playNextAction, page.addToQueueAction, page.addToPlaylistAction, page.downloadAction, page.automaticDownloadsAction, page.stopDownloadsAction, page.favoriteAction, page.togglePlayedAction, page.mediaInfoAction]
    // The row of the page carries the way back and the actions, as on the Movies and Shows tabs; the hero names the item
    globalToolBarStyle: Kirigami.ApplicationHeaderStyle.None
    // Without Kirigami's title row the page names itself to a screen reader
    Accessible.role: Accessible.Pane
    Accessible.name: title
    header: PageToolbar {
        // Over the content: the hero's backdrop, and what scrolls up, fade into the row instead of meeting it at an edge
        z: 1
        fadeBelow: true
        backShown: true
        actions: page.actions
        toolbarObjectName: "detailsToolbar"
    }

    Timer {
        objectName: "endsAtTimer"
        interval: 60000
        repeat: true
        running: page.inView && page.showsEndsAt
        onTriggered: page.now = new Date()
    }

    onInViewChanged: {
        if (inView)
            now = new Date();
    }

    // A bordered label for a short fact; `accessibleName` says what it is when the caption alone does not
    component Badge: Rectangle {
        id: badge

        property string caption
        property string accessibleName: caption

        implicitWidth: badgeText.implicitWidth + Kirigami.Units.largeSpacing
        implicitHeight: badgeText.implicitHeight + Kirigami.Units.smallSpacing
        color: "transparent"
        radius: Kirigami.Units.cornerRadius
        border.width: 1
        border.color: Qt.alpha(Kirigami.Theme.textColor, 0.6)
        Accessible.role: Accessible.StaticText
        Accessible.name: accessibleName

        // The caption can come from the server: plain text only
        Controls.Label {
            id: badgeText
            anchors.centerIn: parent
            textFormat: Text.PlainText
            text: badge.caption
            Accessible.ignored: true
        }
    }

    // A button that runs a Kirigami.Action and follows its text, icon and state
    component ActionButton: Controls.Button {
        required property Kirigami.Action command

        visible: command.visible
        enabled: command.enabled
        text: command.text
        icon.name: command.icon.name
        onClicked: command.trigger()
    }

    ListView {
        id: list

        activeFocusOnTab: count > 0
        keyNavigationEnabled: true
        currentIndex: 0
        objectName: "episodesList"
        // Only a season has episodes: the header carries the rest of the page
        model: page.childModel.episodes
        Accessible.role: Accessible.List
        // Only a season has episodes: on any other page the list is empty and silent
        Accessible.name: page.childModel.kind === ItemChildrenModel.Season ? i18n("Episodes") : ""
        Accessible.ignored: count === 0
        // The page marks its flickable as a tab stop when it completes, which would undo the binding: set it again afterwards
        Component.onCompleted: Qt.callLater(() => list.activeFocusOnTab = Qt.binding(() => list.count > 0))

        // The rows are not tab stops: the list is, and it opens the current episode
        function openCurrent() {
            if (list.currentItem?.enabled)
                page.Window.window.openItem(list.currentItem.itemId, page);
        }
        Keys.onReturnPressed: openCurrent()
        Keys.onEnterPressed: openCurrent()
        Keys.onSpacePressed: openCurrent()

        header: Item {
            width: list.width
            height: headerContent.implicitHeight + page.margin

            // Built on the first use of the Media Info action and kept
            Loader {
                id: mediaInfo
                objectName: "mediaInfoLoader"

                active: page.mediaInfoWanted
                sourceComponent: MediaInfoDialog {
                    objectName: "mediaInfoDialog"
                    sources: page.details.mediaSources
                    window: page.Window.window
                }
                onLoaded: {
                    page.mediaInfoDialog = item;
                    item.open();
                }
            }

            ColumnLayout {
                id: headerContent
                objectName: "detailsHeaderContent"

                // A backdrop starts right under the page row and fades into it: no strip between them
                y: hero.hasBackdrop ? 0 : page.margin
                // Prose and metadata stay readable on ultrawide monitors: the cap is the shell's reading width
                width: Math.min(Math.max(0, parent.width - page.margin * 2), page.Window.window?.readingWidth ?? Infinity)
                x: (parent.width - width) / 2
                spacing: Kirigami.Units.largeSpacing

                Kirigami.LoadingPlaceholder {
                    FadeInOnShow {}
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

                MessageBox {
                    visible: page.details.favoriteError && page.details.status === ItemDetailsModel.Ready
                    type: Kirigami.MessageType.Error
                    text: i18n("Could not update Favorites. Check the connection and try again")
                    actions: [
                        Kirigami.Action {
                            text: i18n("Retry")
                            icon.name: "view-refresh"
                            onTriggered: page.details.toggleFavorite()
                        }
                    ]
                }

                MessageBox {
                    visible: page.details.playedError && page.details.status === ItemDetailsModel.Ready
                    type: Kirigami.MessageType.Error
                    text: i18n("Could not update watched status. Check the connection and try again")
                    actions: [
                        Kirigami.Action {
                            text: i18n("Retry")
                            icon.name: "view-refresh"
                            onTriggered: page.details.togglePlayed()
                        }
                    ]
                }

                Kirigami.PlaceholderMessage {
                    FadeInOnShow {}
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

                    Item {
                        id: hero
                        objectName: "detailsHero"

                        readonly property bool hasBackdrop: page.details.backdropUrl.toString().length > 0
                        // Below this width the text leaves the image and the backdrop shrinks (the shell's wide limit)
                        readonly property bool narrow: width < Kirigami.Units.gridUnit * 36
                        readonly property bool overlay: hasBackdrop && !narrow
                        // Over the image the backdrop grows with the title block: the block and the fade above it must fit
                        readonly property real imageHeight: !hasBackdrop ? 0 : (overlay ? Math.max(Math.min(width * 9 / 16, Kirigami.Units.gridUnit * 24), heroText.implicitHeight + Kirigami.Units.gridUnit * 6) : Kirigami.Units.gridUnit * 9)

                        Layout.fillWidth: true
                        Layout.preferredHeight: overlay ? imageHeight : imageHeight + heroText.implicitHeight + (hasBackdrop ? headerContent.spacing : 0)

                        // The backdrop spans the whole page, edge to edge, outside the margins and the reading width the text keeps
                        Rectangle {
                            objectName: "detailsBackdrop"
                            visible: hero.hasBackdrop
                            x: -headerContent.x
                            width: list.width
                            height: hero.imageHeight
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

                            // The image fades into the window colour; the text over it sits on the nearly opaque part
                            Rectangle {
                                id: scrim

                                readonly property color fade: Kirigami.Theme.backgroundColor

                                anchors {
                                    left: parent.left
                                    right: parent.right
                                    bottom: parent.bottom
                                }
                                // As little of the image as the text needs: an eased rise, opaque only at the image's last line so its edge leaves no seam
                                height: Math.min(parent.height, hero.overlay ? heroText.implicitHeight + Kirigami.Units.gridUnit * 2 : Kirigami.Units.gridUnit * 2)
                                gradient: Gradient {
                                    GradientStop {
                                        position: 0
                                        color: Qt.alpha(scrim.fade, 0)
                                    }
                                    GradientStop {
                                        position: 0.3
                                        color: Qt.alpha(scrim.fade, 0.12)
                                    }
                                    GradientStop {
                                        position: 0.55
                                        color: Qt.alpha(scrim.fade, 0.4)
                                    }
                                    GradientStop {
                                        position: 0.8
                                        color: Qt.alpha(scrim.fade, 0.75)
                                    }
                                    GradientStop {
                                        position: 1
                                        color: scrim.fade
                                    }
                                }
                            }
                        }

                        ColumnLayout {
                            id: heroText
                            objectName: "detailsHeroText"

                            x: hero.overlay ? page.margin : 0
                            y: hero.overlay ? hero.height - height - page.margin : hero.imageHeight + (hero.hasBackdrop ? headerContent.spacing : 0)
                            width: hero.width - (hero.overlay ? page.margin * 2 : 0)
                            spacing: Kirigami.Units.smallSpacing

                            Image {
                                id: logo

                                Layout.alignment: Qt.AlignLeft
                                Layout.preferredWidth: Math.min(heroText.width * 0.6, Kirigami.Units.gridUnit * 16)
                                Layout.preferredHeight: Kirigami.Units.gridUnit * 6
                                // The title stands in until the logo has loaded, and when the item has none
                                visible: status === Image.Ready
                                asynchronous: true
                                fillMode: Image.PreserveAspectFit
                                horizontalAlignment: Image.AlignLeft
                                verticalAlignment: Image.AlignBottom
                                sourceSize.width: Kirigami.Units.gridUnit * 16 * Screen.devicePixelRatio
                                source: page.inView ? page.details.logoUrl : ""
                                Accessible.role: Accessible.Graphic
                                // The series' logo over an episode or a season: its name, not the one the heading under it reads
                                Accessible.name: detailsTitle.underParentLogo ? page.details.seriesName : page.details.name
                            }

                            // An episode or a season shows its series' logo: its own name stays under it, a size smaller
                            Kirigami.Heading {
                                id: detailsTitle
                                objectName: "detailsTitle"
                                readonly property bool underParentLogo: logo.status === Image.Ready && (page.details.type === "Episode" || page.details.type === "Season")
                                Layout.fillWidth: true
                                visible: logo.status !== Image.Ready || underParentLogo
                                level: underParentLogo ? 2 : 1
                                wrapMode: Text.Wrap
                                // Item names come from the server: plain text only
                                textFormat: Text.PlainText
                                text: page.details.name
                            }

                            Flow {
                                Layout.fillWidth: true
                                spacing: Kirigami.Units.largeSpacing
                                // The data decides, not the links: a child reads back its effective visibility, which follows this Flow
                                visible: page.showSeriesLink || page.showSeasonLink

                                Kirigami.LinkButton {
                                    id: seriesLink
                                    objectName: "seriesLink"
                                    visible: page.showSeriesLink
                                    textFormat: Text.PlainText
                                    text: page.details.seriesName
                                    onClicked: page.Window.window.openAncestor(page.details.seriesId, page)
                                }

                                Kirigami.LinkButton {
                                    id: seasonLink
                                    objectName: "seasonLink"
                                    visible: page.showSeasonLink
                                    textFormat: Text.PlainText
                                    text: page.details.seasonNumber > 0 ? i18n("Season %1", page.details.seasonNumber) : (page.details.seasonNumber === 0 ? i18n("Specials") : i18n("Season"))
                                    onClicked: page.Window.window.openAncestor(page.details.seasonId, page)
                                }
                            }

                            Controls.Label {
                                Layout.fillWidth: true
                                visible: page.details.seriesName.length > 0 && page.details.seriesId.length === 0
                                wrapMode: Text.Wrap
                                textFormat: Text.PlainText
                                text: page.details.seriesName
                            }

                            // On a wide window Play sits at the end of the facts row and uses the room beside them; a narrow one gives it the whole width below
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: Kirigami.Units.largeSpacing

                                Flow {
                                    id: facts
                                    objectName: "detailsFacts"

                                    // Every fact has the height of a badge, so a row of mixed facts stays level
                                    readonly property real rowHeight: Kirigami.Units.gridUnit + Kirigami.Units.smallSpacing

                                    Layout.fillWidth: true
                                    spacing: Kirigami.Units.largeSpacing * 2

                                    Badge {
                                        objectName: "factEpisode"
                                        height: facts.rowHeight
                                        visible: page.episodeCode.length > 0
                                        caption: page.episodeCode
                                    }

                                    Controls.Label {
                                        objectName: "factYear"
                                        height: facts.rowHeight
                                        verticalAlignment: Text.AlignVCenter
                                        visible: text.length > 0
                                        textFormat: Text.PlainText
                                        text: page.yearText
                                    }

                                    Controls.Label {
                                        objectName: "factStatus"
                                        height: facts.rowHeight
                                        verticalAlignment: Text.AlignVCenter
                                        visible: text.length > 0
                                        textFormat: Text.PlainText
                                        text: page.statusText
                                    }

                                    Badge {
                                        objectName: "certificateBadge"
                                        height: facts.rowHeight
                                        visible: page.details.officialRating.length > 0
                                        caption: page.details.officialRating
                                        accessibleName: i18n("Rated %1", page.details.officialRating)
                                    }

                                    RowLayout {
                                        id: ratingRow
                                        objectName: "factRating"
                                        height: facts.rowHeight
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

                                    // The critics' score (the Tomatometer the server reads) behind a ripe tomato from 60%, a squashed one under it
                                    RowLayout {
                                        objectName: "factCritic"
                                        readonly property bool fresh: page.details.criticRating >= 60
                                        readonly property alias text: criticLabel.text
                                        height: facts.rowHeight
                                        visible: page.details.criticRating >= 0
                                        spacing: Kirigami.Units.smallSpacing
                                        Accessible.role: Accessible.StaticText
                                        Accessible.name: i18n("Critic Rating: %1", criticLabel.text)

                                        Image {
                                            objectName: "factCriticIcon"
                                            Layout.preferredWidth: Kirigami.Units.iconSizes.small
                                            Layout.preferredHeight: Kirigami.Units.iconSizes.small
                                            sourceSize.width: Kirigami.Units.iconSizes.small * Screen.devicePixelRatio
                                            sourceSize.height: Kirigami.Units.iconSizes.small * Screen.devicePixelRatio
                                            // Images of the module itself, never a server address
                                            source: parent.fresh ? "icons/critic-fresh.svg" : "icons/critic-rotten.svg"
                                            Accessible.ignored: true
                                        }

                                        Controls.Label {
                                            id: criticLabel
                                            textFormat: Text.PlainText
                                            text: i18nc("critic rating in percent", "%1%", Math.round(page.details.criticRating))
                                        }
                                    }

                                    Controls.Label {
                                        objectName: "factSeasons"
                                        height: facts.rowHeight
                                        verticalAlignment: Text.AlignVCenter
                                        visible: page.showSeasonCount
                                        textFormat: Text.PlainText
                                        text: i18np("%1 season", "%1 seasons", page.childModel.seasons.count)
                                    }

                                    Controls.Label {
                                        objectName: "factEpisodes"
                                        height: facts.rowHeight
                                        verticalAlignment: Text.AlignVCenter
                                        visible: page.showEpisodeCount
                                        textFormat: Text.PlainText
                                        text: i18np("%1 episode", "%1 episodes", page.childModel.episodes.totalCount)
                                    }

                                    Controls.Label {
                                        objectName: "factUnplayed"
                                        height: facts.rowHeight
                                        verticalAlignment: Text.AlignVCenter
                                        visible: (page.isSeries || page.isSeason) && page.details.unplayedCount > 0
                                        textFormat: Text.PlainText
                                        text: i18np("%1 unplayed", "%1 unplayed", page.details.unplayedCount)
                                    }

                                    Controls.Label {
                                        objectName: "factRuntime"
                                        height: facts.rowHeight
                                        verticalAlignment: Text.AlignVCenter
                                        visible: page.showsEndsAt
                                        textFormat: Text.PlainText
                                        text: page.runtimeText(page.details.runtimeMinutes)
                                    }

                                    Controls.Label {
                                        objectName: "factEndsAt"
                                        height: facts.rowHeight
                                        verticalAlignment: Text.AlignVCenter
                                        visible: text.length > 0
                                        textFormat: Text.PlainText
                                        text: page.endsAtText(page.now)
                                    }

                                    RowLayout {
                                        objectName: "factPlayed"
                                        height: facts.rowHeight
                                        visible: page.details.played && !page.details.canResume
                                        spacing: Kirigami.Units.smallSpacing
                                        Accessible.role: Accessible.StaticText
                                        Accessible.name: playedLabel.text

                                        Kirigami.Icon {
                                            Layout.preferredWidth: Kirigami.Units.iconSizes.small
                                            Layout.preferredHeight: Kirigami.Units.iconSizes.small
                                            source: "emblem-checked"
                                            Accessible.ignored: true
                                        }

                                        Controls.Label {
                                            id: playedLabel
                                            textFormat: Text.PlainText
                                            text: i18nc("state of an item", "Played")
                                        }
                                    }

                                    RowLayout {
                                        objectName: "factFavorite"
                                        height: facts.rowHeight
                                        visible: page.details.favorite
                                        spacing: Kirigami.Units.smallSpacing
                                        Accessible.role: Accessible.StaticText
                                        Accessible.name: favoriteLabel.text

                                        Kirigami.Icon {
                                            Layout.preferredWidth: Kirigami.Units.iconSizes.small
                                            Layout.preferredHeight: Kirigami.Units.iconSizes.small
                                            source: "emblem-favorite"
                                            Accessible.ignored: true
                                        }

                                        Controls.Label {
                                            id: favoriteLabel
                                            textFormat: Text.PlainText
                                            text: i18n("Favorite")
                                        }
                                    }
                                }

                                ActionButton {
                                    objectName: "detailsPlay"
                                    visible: !hero.narrow && command.visible
                                    command: page.playAction
                                    highlighted: true
                                }
                            }
                        }
                    }

                    // The one button of the hero on a narrow window, the whole width under the facts: the other actions are in the page toolbar
                    ActionButton {
                        objectName: "detailsPlayNarrow"
                        Layout.fillWidth: true
                        visible: hero.narrow && command.visible
                        command: page.playAction
                        highlighted: true
                    }

                    Flow {
                        objectName: "detailsBadges"

                        Layout.fillWidth: true
                        spacing: Kirigami.Units.smallSpacing
                        visible: page.details.mediaBadges.length > 0

                        Repeater {
                            model: page.details.mediaBadges
                            delegate: Badge {
                                id: mediaBadge

                                required property var modelData

                                caption: modelData.kind === "subtitles" ? i18np("%1 subtitle", "%1 subtitles", Number(modelData.text)) : modelData.text
                                height: Kirigami.Units.gridUnit + Kirigami.Units.smallSpacing
                            }
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

                    // The whole width of the column, with room at its end; four lines when folded, but a text that would hide only a line or two shows whole
                    ColumnLayout {
                        id: overviewBlock

                        readonly property int foldedLines: 4
                        readonly property bool foldable: overviewMeasure.lineCount > foldedLines + 2
                        readonly property bool folded: foldable && !page.overviewExpanded
                        // Where the view stood below the start of the list when the text opened or folded (-1: nothing to keep)
                        property real keptOffset: -1

                        function keepPlace() {
                            const most = Math.max(0, list.contentHeight - list.height);
                            list.contentY = list.originY + Math.min(keptOffset, most);
                        }
                        // The list lays the new header out over a few passes and moves its origin each time, and a list with no rows of its own (a movie: everything is its header) goes back to its start when it lays out: the place is put back after each, for a moment
                        Connections {
                            target: list
                            enabled: overviewBlock.keptOffset >= 0
                            function onOriginYChanged() {
                                overviewBlock.keepPlace();
                            }
                            function onContentHeightChanged() {
                                overviewBlock.keepPlace();
                            }
                            function onContentYChanged() {
                                if (overviewBlock.keptOffset > 0 && Math.abs(list.contentY - list.originY) < 0.5)
                                    overviewBlock.keepPlace();
                            }
                        }
                        Timer {
                            id: keepPlaceWindow
                            interval: Kirigami.Units.longDuration
                            onTriggered: overviewBlock.keptOffset = -1
                        }

                        Layout.fillWidth: true
                        Layout.rightMargin: Kirigami.Units.gridUnit * 2
                        visible: overviewLabel.text.length > 0
                        spacing: 0

                        Controls.Label {
                            id: overviewLabel
                            objectName: "overviewLabel"

                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            maximumLineCount: overviewBlock.folded ? overviewBlock.foldedLines : 100000
                            textFormat: Text.PlainText
                            text: page.details.overview

                            // The same text unfolded and never seen: how many lines it takes at this width
                            Controls.Label {
                                id: overviewMeasure
                                width: overviewLabel.width
                                opacity: 0
                                wrapMode: Text.Wrap
                                textFormat: Text.PlainText
                                font: overviewLabel.font
                                text: overviewLabel.text
                                Accessible.ignored: true
                            }

                            // The last folded line fades out: the text goes on
                            Rectangle {
                                objectName: "overviewFade"
                                visible: overviewBlock.folded
                                anchors {
                                    left: parent.left
                                    right: parent.right
                                    bottom: parent.bottom
                                }
                                height: Math.round(parent.height / overviewBlock.foldedLines * 1.25)
                                gradient: Gradient {
                                    GradientStop {
                                        position: 0
                                        color: Qt.alpha(Kirigami.Theme.backgroundColor, 0)
                                    }
                                    GradientStop {
                                        position: 1
                                        color: Kirigami.Theme.backgroundColor
                                    }
                                }
                            }
                        }

                        Controls.Button {
                            objectName: "overviewToggle"
                            Layout.alignment: Qt.AlignRight
                            flat: true
                            visible: overviewBlock.foldable
                            text: page.overviewExpanded ? i18n("Show Less") : i18n("Show More")
                            icon.name: page.overviewExpanded ? "go-up" : "go-down"
                            // The page keeps its place: the text grows or shrinks under the same view
                            onClicked: {
                                overviewBlock.keptOffset = list.contentY - list.originY;
                                page.overviewExpanded = !page.overviewExpanded;
                                overviewBlock.keepPlace();
                                keepPlaceWindow.restart();
                            }
                        }
                    }

                    GridLayout {
                        objectName: "detailsCredits"

                        Layout.fillWidth: true
                        Layout.maximumWidth: Kirigami.Units.gridUnit * 43
                        visible: page.details.genres.length > 0 || page.details.studios.length > 0 || page.details.directors.length > 0 || page.details.writers.length > 0
                        columns: 2
                        columnSpacing: Kirigami.Units.largeSpacing
                        rowSpacing: Kirigami.Units.smallSpacing

                        Controls.Label {
                            objectName: "creditsGenresLabel"
                            Layout.alignment: Qt.AlignTop
                            visible: page.details.genres.length > 0
                            opacity: 0.7
                            textFormat: Text.PlainText
                            text: i18np("Genre", "Genres", page.details.genres.length)
                        }
                        Controls.Label {
                            objectName: "creditsGenres"
                            Layout.fillWidth: true
                            visible: page.details.genres.length > 0
                            wrapMode: Text.Wrap
                            textFormat: Text.PlainText
                            text: page.details.genres.join(", ")
                        }

                        Controls.Label {
                            objectName: "creditsStudiosLabel"
                            Layout.alignment: Qt.AlignTop
                            visible: page.details.studios.length > 0
                            opacity: 0.7
                            textFormat: Text.PlainText
                            text: page.isSeries ? i18np("Network", "Networks", page.details.studios.length) : i18np("Studio", "Studios", page.details.studios.length)
                        }
                        Controls.Label {
                            objectName: "creditsStudios"
                            Layout.fillWidth: true
                            visible: page.details.studios.length > 0
                            wrapMode: Text.Wrap
                            textFormat: Text.PlainText
                            text: page.details.studios.join(", ")
                        }

                        Controls.Label {
                            objectName: "creditsDirectorsLabel"
                            Layout.alignment: Qt.AlignTop
                            visible: page.details.directors.length > 0
                            opacity: 0.7
                            textFormat: Text.PlainText
                            text: i18np("Director", "Directors", page.details.directors.length)
                        }
                        Controls.Label {
                            objectName: "creditsDirectors"
                            Layout.fillWidth: true
                            visible: page.details.directors.length > 0
                            wrapMode: Text.Wrap
                            textFormat: Text.PlainText
                            text: page.details.directors.join(", ")
                        }

                        Controls.Label {
                            objectName: "creditsWritersLabel"
                            Layout.alignment: Qt.AlignTop
                            visible: page.details.writers.length > 0
                            opacity: 0.7
                            textFormat: Text.PlainText
                            text: i18np("Writer", "Writers", page.details.writers.length)
                        }
                        Controls.Label {
                            objectName: "creditsWriters"
                            Layout.fillWidth: true
                            visible: page.details.writers.length > 0
                            wrapMode: Text.Wrap
                            textFormat: Text.PlainText
                            text: page.details.writers.join(", ")
                        }
                    }

                    Kirigami.Heading {
                        Layout.fillWidth: true
                        visible: page.childModel.kind === ItemChildrenModel.Series && page.childModel.hasNextUp
                        level: 2
                        text: i18n("Next Up")
                    }

                    EpisodeRow {
                        Layout.fillWidth: true
                        visible: page.childModel.kind === ItemChildrenModel.Series && page.childModel.hasNextUp
                        showSeason: true
                        itemId: page.nextUp.itemId ?? ""
                        name: page.nextUp.name ?? ""
                        seasonNumber: page.nextUp.seasonNumber ?? -1
                        episodeNumber: page.nextUp.episodeNumber ?? -1
                        episodeNumberEnd: page.nextUp.episodeNumberEnd ?? -1
                        runtimeMinutes: page.nextUp.runtimeMinutes ?? -1
                        overview: page.nextUp.overview ?? ""
                        imageUrl: page.nextUp.imageUrl ?? ""
                        played: page.nextUp.played ?? false
                        playedPercentage: page.nextUp.playedPercentage ?? 0
                        favorite: page.nextUp.favorite ?? false
                        loaded: page.nextUp.loaded ?? false
                        onClicked: page.Window.window.openItem(itemId, page)
                    }

                    Kirigami.Heading {
                        Layout.fillWidth: true
                        visible: page.childModel.kind === ItemChildrenModel.Series && (page.childModel.seasonsStatus === ItemChildrenModel.Loading || page.childModel.seasonsStatus === ItemChildrenModel.Failed || page.childModel.seasons.count > 0)
                        level: 2
                        text: i18n("Seasons")
                    }

                    Controls.BusyIndicator {
                        FadeInOnShow {}
                        Layout.preferredWidth: Kirigami.Units.gridUnit * 2
                        Layout.preferredHeight: Kirigami.Units.gridUnit * 2
                        visible: page.childModel.kind === ItemChildrenModel.Series && page.childModel.seasonsStatus === ItemChildrenModel.Loading && page.childModel.seasons.count === 0
                        running: visible
                        Accessible.name: i18n("Loading…")
                    }

                    MessageBox {
                        visible: page.childModel.kind === ItemChildrenModel.Series && page.childModel.seasonsStatus === ItemChildrenModel.Failed
                        type: Kirigami.MessageType.Error
                        text: i18n("Could not load the seasons")
                        actions: [
                            Kirigami.Action {
                                text: i18n("Retry")
                                icon.name: "view-refresh"
                                onTriggered: page.childModel.retry()
                            }
                        ]
                    }

                    CardRow {
                        visible: page.childModel.kind === ItemChildrenModel.Series && page.childModel.seasons.count > 0
                        model: page.childModel.seasons
                        listObjectName: "seasonsRow"
                        cardHeight: page.posterCardHeight
                        accessibleName: i18n("Seasons")
                        pageFlickable: page.flickable

                        delegate: MediaCard {
                            width: page.posterCardWidth
                            height: page.posterCardHeight
                            // The list is the one tab stop of its row
                            focusPolicy: Qt.NoFocus
                            loaded: true
                            highlighted: ListView.isCurrentItem && ListView.view.activeFocus
                            onClicked: page.Window.window.openItem(itemId, page)
                        }
                    }

                    Kirigami.Heading {
                        Layout.fillWidth: true
                        visible: page.childModel.kind === ItemChildrenModel.BoxSet && (page.childModel.itemsStatus === ItemChildrenModel.Loading || page.childModel.itemsStatus === ItemChildrenModel.Failed || page.childModel.items.count > 0)
                        level: 2
                        text: i18n("In This Collection")
                    }

                    Controls.Label {
                        Layout.fillWidth: true
                        visible: page.childModel.kind === ItemChildrenModel.BoxSet && page.childModel.items.count > 0 && page.childModel.itemsTotal > page.childModel.items.count
                        opacity: 0.7
                        textFormat: Text.PlainText
                        text: i18n("%1 of %2 items", page.childModel.items.count, page.childModel.itemsTotal)
                    }

                    Controls.BusyIndicator {
                        FadeInOnShow {}
                        Layout.preferredWidth: Kirigami.Units.gridUnit * 2
                        Layout.preferredHeight: Kirigami.Units.gridUnit * 2
                        visible: page.childModel.kind === ItemChildrenModel.BoxSet && page.childModel.itemsStatus === ItemChildrenModel.Loading && page.childModel.items.count === 0
                        running: visible
                        Accessible.name: i18n("Loading…")
                    }

                    MessageBox {
                        visible: page.childModel.kind === ItemChildrenModel.BoxSet && page.childModel.itemsStatus === ItemChildrenModel.Failed
                        type: Kirigami.MessageType.Error
                        text: i18n("Could not load the items of this collection")
                        actions: [
                            Kirigami.Action {
                                text: i18n("Retry")
                                icon.name: "view-refresh"
                                onTriggered: page.childModel.retry()
                            }
                        ]
                    }

                    CardRow {
                        visible: page.childModel.kind === ItemChildrenModel.BoxSet && page.childModel.items.count > 0
                        model: page.childModel.items
                        listObjectName: "collectionRow"
                        cardHeight: page.childModel.items.landscape ? page.similarCardHeight : page.posterCardHeight
                        accessibleName: i18n("In This Collection")
                        pageFlickable: page.flickable

                        delegate: MediaCard {
                            width: page.childModel.items.landscape ? page.similarCardWidth : page.posterCardWidth
                            height: page.childModel.items.landscape ? page.similarCardHeight : page.posterCardHeight
                            // The list is the one tab stop of its row
                            focusPolicy: Qt.NoFocus
                            landscape: page.childModel.items.landscape
                            loaded: true
                            highlighted: ListView.isCurrentItem && ListView.view.activeFocus
                            onClicked: page.Window.window.openItem(itemId, page)
                        }
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
                        visible: page.showSimilar
                        level: 2
                        text: i18n("More Like This")
                    }

                    Controls.BusyIndicator {
                        FadeInOnShow {}
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

                    Kirigami.Heading {
                        Layout.fillWidth: true
                        visible: page.childModel.kind === ItemChildrenModel.Season
                        level: 2
                        text: i18n("Episodes")
                    }

                    Kirigami.LoadingPlaceholder {
                        FadeInOnShow {}
                        Layout.fillWidth: true
                        visible: page.childModel.kind === ItemChildrenModel.Season && page.childModel.episodes.status === SeasonEpisodesModel.Loading && list.count === 0
                        text: i18n("Loading…")
                    }

                    MessageBox {
                        visible: page.childModel.kind === ItemChildrenModel.Season && page.childModel.episodes.status === SeasonEpisodesModel.Failed
                        type: Kirigami.MessageType.Error
                        text: i18n("Could not load the episodes: check the connection to the server and try again")
                        actions: [
                            Kirigami.Action {
                                text: i18n("Retry")
                                icon.name: "view-refresh"
                                onTriggered: page.childModel.episodes.retry()
                            }
                        ]
                    }

                    Kirigami.PlaceholderMessage {
                        FadeInOnShow {}
                        Layout.fillWidth: true
                        visible: page.childModel.kind === ItemChildrenModel.Season && page.childModel.episodes.status === SeasonEpisodesModel.Ready && page.childModel.episodes.totalCount === 0
                        icon.name: "video-x-generic"
                        text: i18n("This season has no episodes")
                    }
                }
            }
        }

        delegate: EpisodeRow {
            x: page.margin
            width: list.width - page.margin * 2
            // The list is the one tab stop, the arrow keys move inside it
            focusPolicy: Qt.NoFocus
            highlighted: ListView.isCurrentItem && list.activeFocus
            itemId: model.itemId
            name: model.name
            seasonNumber: model.seasonNumber
            episodeNumber: model.episodeNumber
            episodeNumberEnd: model.episodeNumberEnd
            runtimeMinutes: model.runtimeMinutes
            overview: model.overview
            imageUrl: model.imageUrl
            played: model.played
            playedPercentage: model.playedPercentage
            favorite: model.favorite
            loaded: model.loaded
            onClicked: page.Window.window.openItem(itemId, page)
        }

        footer: Item {
            width: list.width
            // The warning has an action at its trailing end, where the mini player docks: it lifts the box above the dock
            height: footerContent.implicitHeight + page.margin + (episodesWarning.visible ? (page.Window.window?.miniPlayerSpace ?? 0) : 0)

            ColumnLayout {
                id: footerContent

                x: page.margin
                y: page.margin
                width: parent.width - page.margin * 2

                MessageBox {
                    id: episodesWarning
                    visible: page.childModel.episodes.pageError && page.childModel.episodes.status !== SeasonEpisodesModel.Failed
                    type: Kirigami.MessageType.Warning
                    text: i18n("Some episodes could not be loaded")
                    actions: [
                        Kirigami.Action {
                            text: i18n("Retry")
                            icon.name: "view-refresh"
                            onTriggered: page.childModel.episodes.retry()
                        }
                    ]
                }
            }
        }
    }
}
