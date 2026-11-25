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
    // The one message box the page shows, the first with something to report in this order: the item, the main section (seasons, collection, episodes), the similar items, the favorite and watched updates, then the partial episodes warning. A section title never stays without its box. Each box shows only when it is the one named here
    readonly property string shownBox: {
        if (details.status === ItemDetailsModel.Failed)
            return "item";
        if (showContent) {
            if (childModel.kind === ItemChildrenModel.Series && childModel.seasonsStatus === ItemChildrenModel.Failed)
                return "seasons";
            if (childModel.kind === ItemChildrenModel.BoxSet && childModel.itemsStatus === ItemChildrenModel.Failed)
                return "collection";
            if (childModel.kind === ItemChildrenModel.Season && childModel.episodes.status === SeasonEpisodesModel.Failed)
                return "episodes";
            if (details.similarStatus === ItemDetailsModel.Failed)
                return "similar";
        }
        if (details.status === ItemDetailsModel.Ready && (details.favoriteError || details.playedError))
            return "update";
        if (childModel.episodes.pageError && childModel.episodes.status !== SeasonEpisodesModel.Failed)
            return "episodesPartial";
        return "";
    }

    // Set by the first Media Info action: the Loader then builds the dialog, which hands itself back in mediaInfoDialog
    property bool mediaInfoWanted: false
    property MediaInfoDialog mediaInfoDialog: null

    // What the main menu's Refresh calls while this page is in front
    function refreshPage() {
        details.reload();
    }

    // The space the page keeps around its content
    readonly property real margin: Kirigami.Units.largeSpacing
    // The episodes and the people under them keep the reading width of the header, centred
    readonly property real listColumnWidth: Math.min(Math.max(0, list.width - margin * 2), page.Window.window?.readingWidth ?? Infinity)
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

    // A series or a season has no stream of its own: Play starts its first episode and Continue the one to go on with (a series' Next Up, a season's first unplayed), each played alone like an episode opened by itself
    readonly property bool playsEpisodes: childModel.kind === ItemChildrenModel.Series || childModel.kind === ItemChildrenModel.Season
    // The buttons wait for the answers of both listings, so their text and target do not change after they show; a listing that failed leaves its episode out
    readonly property bool episodesReady: playsEpisodes && childModel.playTargetsReady
    // Play starts the first episode; when its listing failed the one to go on with stands in
    readonly property var startEpisode: !episodesReady ? ({}) : (childModel.hasFirstEpisode ? childModel.firstEpisode : nextUp)
    // Something was watched: the episode to go on with is in progress, or is not the first one
    readonly property bool continueOffered: episodesReady && childModel.hasNextUp && ((nextUp.positionTicks ?? 0) > 0 || (childModel.hasFirstEpisode && nextUp.itemId !== childModel.firstEpisode.itemId))

    // What a hero button of a series or a season plays, for a screen reader: the button itself says Play or Continue
    function episodeDescription(episode) {
        if (!playsEpisodes || !episode?.itemId)
            return "";
        const label = episode.seasonNumber > 0 && episode.episodeNumber > 0 ? EpisodeLabel.label(episode.seasonNumber, episode.episodeNumber) + " " : "";
        return label + (episode.name ?? "");
    }
    readonly property string playDescription: episodeDescription(continueOffered ? nextUp : startEpisode)
    readonly property string startDescription: episodeDescription(startEpisode)

    // Play is the hero's button, the others are page actions
    readonly property Kirigami.Action playAction: Kirigami.Action {
        objectName: "playAction"
        text: page.continueOffered ? ((page.nextUp.positionTicks ?? 0) > 0 ? i18n("Resume") : i18nc("@action:button", "Continue")) : (page.details.canResume ? i18n("Resume") : i18n("Play"))
        icon.name: "media-playback-start"
        visible: page.details.playable || (page.playsEpisodes && !!page.startEpisode.itemId)
        enabled: page.details.status === ItemDetailsModel.Ready && !!page.Window.window?.player
        onTriggered: {
            if (page.playsEpisodes) {
                const episode = page.continueOffered ? page.nextUp : page.startEpisode;
                page.Window.window.playItemWithStreams(episode.itemId, page.continueOffered ? (episode.positionTicks ?? 0) : 0, page.audioStreamIndex, page.subtitleStreamIndex);
            } else {
                page.Window.window.playItemWithStreams(page.details.itemId, page.details.canResume ? page.details.resumePositionTicks : 0, page.audioStreamIndex, page.subtitleStreamIndex);
            }
        }
    }
    readonly property Kirigami.Action playOnAction: Kirigami.Action {
        objectName: "playOnAction"
        displayHint: Kirigami.DisplayHint.KeepVisible
        text: i18nc("@action:button", "Cast…")
        icon.name: "kinefin-cast"
        visible: page.details.status === ItemDetailsModel.Ready && page.details.playable
        enabled: !!page.Window.window?.player && !page.Window.window?.groupRouting
        tooltip: page.Window.window?.groupRouting ? i18nc("@info:tooltip", "Not available while following a SyncPlay group") : ""
        onTriggered: page.Window.window.playOn(page.details.itemId, page.details.canResume ? page.details.resumePositionTicks : 0)
    }
    // Restart: a movie or an episode in progress starts over from the first second, a series or a season that was watched from its first episode
    readonly property Kirigami.Action playFromBeginningAction: Kirigami.Action {
        objectName: "playFromBeginningAction"
        text: i18n("Play from Beginning")
        icon.name: "media-skip-backward"
        visible: page.details.status === ItemDetailsModel.Ready && ((page.details.playable && page.details.canResume) || page.continueOffered)
        enabled: page.playAction.enabled
        onTriggered: page.Window.window.playItemWithStreams(page.playsEpisodes ? page.startEpisode.itemId : page.details.itemId, 0, page.audioStreamIndex, page.subtitleStreamIndex)
    }
    // The first remote trailer the server offers, in the browser: only https URLs are kept by the parser
    readonly property Kirigami.Action trailerAction: Kirigami.Action {
        objectName: "trailerAction"
        displayHint: Kirigami.DisplayHint.AlwaysHide
        text: i18n("Play Trailer")
        icon.name: "camera-video-symbolic"
        visible: page.details.status === ItemDetailsModel.Ready && page.details.trailers.length > 0
        onTriggered: {
            if (page.details.trailers.length === 0)
                return;
            if (!Qt.openUrlExternally(page.details.trailers[0].url))
                page.Window.window.showPassiveNotification(i18n("Could not open the trailer in the browser"));
        }
    }
    // The token-free static stream URL of the first source, for an external player; the Authorization header carries the token
    readonly property Kirigami.Action copyStreamUrlAction: Kirigami.Action {
        objectName: "copyStreamUrlAction"
        displayHint: Kirigami.DisplayHint.AlwaysHide
        text: i18n("Copy Stream URL")
        icon.name: "edit-copy"
        visible: page.details.status === ItemDetailsModel.Ready && page.details.mediaSources.length > 0
        onTriggered: {
            const source = page.details.mediaSources.length > 0 ? page.details.mediaSources[0] : null;
            const url = source ? page.connection.streamUrl(page.details.itemId, source.id) : "";
            if (url.toString().length === 0)
                return;
            clipboard.text = url.toString();
            clipboard.selectAll();
            clipboard.copy();
            page.Window.window.showPassiveNotification(i18n("Stream URL copied to the clipboard"));
        }
    }
    readonly property Kirigami.Action refreshMetadataAction: Kirigami.Action {
        objectName: "refreshMetadataAction"
        text: i18n("Refresh Metadata")
        icon.name: "view-refresh"
        visible: page.details.status === ItemDetailsModel.Ready
                 && page.connection.currentUserIsAdministrator
        onTriggered: {
            if (page.connection.refreshItem(page.details.itemId))
                page.Window.window.showPassiveNotification(i18n("Metadata refresh requested"));
            else
                page.Window.window.showPassiveNotification(i18n("Could not request a metadata refresh"));
        }
    }
    readonly property Kirigami.Action editMetadataAction: Kirigami.Action {
        objectName: "editMetadataAction"
        text: i18n("Edit Metadata…")
        icon.name: "document-properties"
        visible: page.details.status === ItemDetailsModel.Ready
                 && page.connection.currentUserIsAdministrator
                 && ["Movie", "Series", "Episode", "Season"].indexOf(page.details.type) >= 0
        onTriggered: editMetadataDialog.open()
    }
    readonly property Kirigami.Action identifyAction: Kirigami.Action {
        objectName: "identifyAction"
        text: i18n("Identify…")
        icon.name: "edit-find"
        visible: page.details.status === ItemDetailsModel.Ready
                 && page.connection.currentUserIsAdministrator
                 && (page.details.type === "Movie" || page.details.type === "Series")
        onTriggered: identifyDialog.open()
    }
    readonly property Kirigami.Action editImagesAction: Kirigami.Action {
        objectName: "editImagesAction"
        text: i18n("Edit Images…")
        icon.name: "edit-image"
        visible: page.details.status === ItemDetailsModel.Ready
                 && page.connection.currentUserIsAdministrator
                 && ["Movie", "Series", "Episode", "Season"].indexOf(page.details.type) >= 0
        onTriggered: editImagesDialog.open()
    }
    readonly property Kirigami.Action editSubtitlesAction: Kirigami.Action {
        objectName: "editSubtitlesAction"
        text: i18n("Edit Subtitles…")
        icon.name: "media-view-subtitles-symbolic"
        visible: page.details.status === ItemDetailsModel.Ready
                 && page.connection.currentUserIsAdministrator
                 && ["Movie", "Episode", "Video"].indexOf(page.details.type) >= 0
        onTriggered: editSubtitlesDialog.open()
    }
    readonly property Kirigami.Action playNextAction: Kirigami.Action {
        objectName: "playNextAction"
        displayHint: Kirigami.DisplayHint.AlwaysHide
        text: i18n("Play Next")
        icon.name: "media-skip-forward"
        visible: page.queueable
        // A SyncPlay group's queue does not wait for the user's own
        enabled: !!page.Window.window?.groupRouting || !page.connection.queue.busy
        onTriggered: page.Window.window?.queueItem(page.details.itemId, true)
    }
    readonly property Kirigami.Action addToQueueAction: Kirigami.Action {
        objectName: "addToQueueAction"
        displayHint: Kirigami.DisplayHint.AlwaysHide
        text: i18n("Add to Queue")
        icon.name: "list-add"
        visible: page.queueable
        enabled: !!page.Window.window?.groupRouting || !page.connection.queue.busy
        onTriggered: page.Window.window?.queueItem(page.details.itemId, false)
    }
    readonly property Kirigami.Action addToPlaylistAction: Kirigami.Action {
        objectName: "addToPlaylistAction"
        displayHint: Kirigami.DisplayHint.AlwaysHide
        text: i18nc("@action:inmenu", "Add to Playlist…")
        icon.name: "media-playlist-append"
        visible: page.details.status === ItemDetailsModel.Ready && page.details.playable
        onTriggered: page.Window.window.addToPlaylist(page.details.itemId)
    }
    // A movie or a whole series joins a collection; episodes belong to their series
    readonly property Kirigami.Action addToCollectionAction: Kirigami.Action {
        objectName: "addToCollectionAction"
        displayHint: Kirigami.DisplayHint.AlwaysHide
        text: i18nc("@action:inmenu", "Add to Collection…")
        icon.name: "folder-new"
        visible: page.details.status === ItemDetailsModel.Ready && (page.details.type === "Movie" || page.details.type === "Series")
        onTriggered: page.Window.window.addToCollection(page.details.itemId, page.details.name)
    }
    // A destructive write: administrators only, and only when the server itself says CanDelete
    readonly property Kirigami.Action deleteMediaAction: Kirigami.Action {
        objectName: "deleteMediaAction"
        displayHint: Kirigami.DisplayHint.AlwaysHide
        text: i18n("Delete Media…")
        icon.name: "edit-delete"
        visible: page.details.status === ItemDetailsModel.Ready && page.details.canDelete
                 && page.connection.currentUserIsAdministrator
                 && ["Movie", "Episode", "Video"].indexOf(page.details.type) >= 0
        onTriggered: deleteDialog.open()
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
        displayHint: Kirigami.DisplayHint.KeepVisible
        text: page.downloadRemovable ? i18n("Delete Download…") : page.downloadCancellable ? i18n("Cancel Download") : i18n("Download")
        icon.name: page.downloadRemovable ? "edit-delete" : page.downloadCancellable ? "process-stop" : "download-symbolic"
        visible: page.downloadable
        onTriggered: {
            if (page.downloadRemovable)
                page.Window.window.confirmRemoveDownload(page.details.itemId);
            else if (page.downloadCancellable)
                page.connection.downloads.removeCurrentDownload(page.details.itemId);
            else
                page.connection.downloads.downloadItem(page.details.itemId);
        }
    }
    // Automatic downloads of a series or a season; the rule store has one rule per series
    readonly property int rulesRevision: page.connection.downloads.rulesRevision
    readonly property string ruleSeriesId: page.details.type === "Series" ? page.details.itemId : page.details.seriesId
    readonly property var seriesRule: rulesRevision >= 0 && ruleSeriesId.length > 0
                                      ? page.connection.downloads.seriesRule(ruleSeriesId) : ({})
    readonly property Kirigami.Action automaticDownloadsAction: Kirigami.Action {
        objectName: "automaticDownloadsAction"
        text: page.seriesRule.kind ? i18n("Edit Automatic Downloads…") : i18n("Automatic Downloads…")
        icon.name: "download-later-symbolic"
        visible: page.details.status === ItemDetailsModel.Ready
                 && (page.details.type === "Series" || page.details.type === "Season")
                 && page.connection.downloads.canDownload
        onTriggered: ruleDialog.open()
    }
    // A series or a season is downloaded by its episodes: the top bar has one action with a menu, as the movie page has Download
    readonly property Kirigami.Action downloadAllAction: Kirigami.Action {
        objectName: "downloadAllAction"
        text: i18n("Download All…")
        icon.name: "download-symbolic"
        onTriggered: downloadAllDialog.open()
    }
    readonly property Kirigami.Action downloadMenuAction: Kirigami.Action {
        objectName: "downloadMenuAction"
        displayHint: Kirigami.DisplayHint.KeepVisible
        text: i18n("Download")
        icon.name: "download-symbolic"
        visible: page.automaticDownloadsAction.visible
        children: [page.downloadAllAction, page.automaticDownloadsAction]
    }
    DownloadAllDialog {
        id: downloadAllDialog
        downloads: page.connection.downloads
        containerId: page.details.itemId
        series: page.details.type === "Series"
        onQueued: (count, expected) => {
            if (count < expected)
                page.Window.window.showPassiveNotification(i18n("Only %1 of %2 downloads could be queued", count, expected), "long", i18n("Show Downloads"), () => page.Window.window.openDownloads());
            else if (count > 0)
                page.Window.window.showPassiveNotification(i18np("%1 download queued", "%1 downloads queued", count), "long", i18n("Show Downloads"), () => page.Window.window.openDownloads());
        }
    }
    readonly property Kirigami.Action stopDownloadsAction: Kirigami.Action {
        objectName: "stopDownloadsAction"
        displayHint: Kirigami.DisplayHint.AlwaysHide
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
        paused: page.seriesRule.enabled === false
    }
    readonly property Kirigami.Action favoriteAction: Kirigami.Action {
        objectName: "toggleFavoriteAction"
        displayHint: Kirigami.DisplayHint.KeepVisible
        text: page.details.favorite ? i18n("Remove from Favorites") : i18n("Add to Favorites")
        // The icon shows the state too: a narrow toolbar may show the icon alone
        icon.name: page.details.favorite ? "starred-symbolic" : "non-starred-symbolic"
        visible: page.details.status === ItemDetailsModel.Ready
        enabled: !page.details.favoriteBusy
        onTriggered: page.details.toggleFavorite()
    }

    title: details.name
    // The bar keeps the frequent actions (Cast, Download, the favorite toggle, watched state, Media Info); the ⋮ menu holds the rest, the edits of the server folded into the Edit submenu that shows only while one of its entries does, and Delete Media last behind a separator that shows only when something stands above it
    readonly property Kirigami.Action serverEditMenuAction: Kirigami.Action {
        objectName: "serverEditMenuAction"
        displayHint: Kirigami.DisplayHint.AlwaysHide
        text: i18nc("@action:inmenu submenu of the details menu", "Edit")
        icon.name: "document-edit"
        visible: page.editMetadataAction.visible || page.identifyAction.visible || page.editImagesAction.visible || page.editSubtitlesAction.visible || page.refreshMetadataAction.visible
        children: [page.editMetadataAction, page.identifyAction, page.editImagesAction, page.editSubtitlesAction, page.refreshMetadataAction]
    }
    readonly property bool overflowHasEntries: trailerAction.visible || playNextAction.visible || addToQueueAction.visible || stopDownloadsAction.visible || addToPlaylistAction.visible || addToCollectionAction.visible || serverEditMenuAction.visible || copyStreamUrlAction.visible
    readonly property Kirigami.Action separatorDelete: Kirigami.Action {
        objectName: "overflowSeparatorDelete"
        separator: true
        displayHint: Kirigami.DisplayHint.AlwaysHide
        visible: page.deleteMediaAction.visible && page.overflowHasEntries
    }

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

    // The page opens on its primary action, so Up and Down start where the user acts; the list takes the focus until Play is ready
    property bool playFocusPending: false
    // The hero's buttons cannot be reached by id from here: they hand themselves over when they are created
    property Item widePlayButton: null
    property Item narrowPlayButton: null
    onWidePlayButtonChanged: Qt.callLater(page.focusPendingPlay)
    onNarrowPlayButtonChanged: Qt.callLater(page.focusPendingPlay)
    function focusPlay() {
        const button = widePlayButton?.visible ? widePlayButton : (narrowPlayButton?.visible ? narrowPlayButton : null);
        if (!button || !button.enabled)
            return false;
        button.forceActiveFocus(Qt.TabFocusReason);
        playFocusPending = false;
        return true;
    }
    function focusPrimary() {
        if (focusPlay())
            return;
        playFocusPending = true;
        list.forceActiveFocus(Qt.TabFocusReason);
    }
    function focusPendingPlay() {
        if (!playFocusPending || !isCurrentPage)
            return;
        const now = page.Window.activeFocusItem;
        // The user moved the focus elsewhere while the details loaded, or down the episode list: leave it there
        if (now && now !== page && now !== list && now !== page.Window.window)
            return;
        if (now === list && list.currentIndex > 0)
            return;
        focusPlay();
    }
    Connections {
        target: page.details
        function onStatusChanged() { Qt.callLater(page.focusPendingPlay); }
    }
    Connections {
        target: page.playAction
        function onEnabledChanged() { Qt.callLater(page.focusPendingPlay); }
        function onVisibleChanged() { Qt.callLater(page.focusPendingPlay); }
    }

    function runtimeText(minutes) {
        return minutes >= 60 ? i18n("%1 h %2 min", Math.floor(minutes / 60), minutes % 60) : i18n("%1 min", minutes);
    }

    // The tracks of the item's first media source, picked before playback; the indexes are the server's
    readonly property var mediaSource: page.details.mediaSources.length > 0 ? page.details.mediaSources[0] : null
    readonly property var audioTracks: page.mediaSource ? page.mediaSource.audio : []
    readonly property var subtitleTracks: page.mediaSource ? page.mediaSource.subtitles : []
    property int audioStreamIndex: -1
    // -2 leaves the choice to mpv or the server, -1 turns subtitles off
    property int subtitleStreamIndex: -2
    Connections {
        target: page.details
        // A reload can replace the streams: a chosen index no longer listed goes back to the default
        function onDetailsChanged() {
            if (!page.audioChoices.some(choice => choice.index === page.audioStreamIndex))
                page.audioStreamIndex = -1;
            if (!page.subtitleChoices.some(choice => choice.index === page.subtitleStreamIndex))
                page.subtitleStreamIndex = -2;
        }
    }

    function trackLabel(track) {
        const title = track.title.length > 0 ? track.title
                    : track.language.length > 0 ? track.language
                    : track.codec.length > 0 ? track.codec
                    : i18n("Track %1", track.index);
        const extra = [];
        if (track.title.length > 0 && track.language.length > 0)
            extra.push(track.language);
        if (track.isForced)
            extra.push(i18n("Forced"));
        if (track.isExternal)
            extra.push(i18n("External"));
        return extra.length > 0 ? title + " (" + extra.join(", ") + ")" : title;
    }
    readonly property var audioChoices: {
        const choices = [{ index: -1, label: i18n("Default") }];
        for (const track of page.audioTracks) {
            if (track.index >= 0)
                choices.push({ index: track.index, label: page.trackLabel(track) });
        }
        return choices;
    }
    readonly property var subtitleChoices: {
        const choices = [{ index: -2, label: i18n("Default") }, { index: -1, label: i18n("Off") }];
        for (const track of page.subtitleTracks) {
            if (track.index >= 0)
                choices.push({ index: track.index, label: page.trackLabel(track) });
        }
        return choices;
    }
    // The longest name of a list of choices, to size the list that opens under a box that cuts it
    function longestLabel(choices) {
        return choices.reduce((longest, choice) => choice.label.length > longest.length ? choice.label : longest, "");
    }
    function choiceIndex(choices, value) {
        const index = choices.findIndex(choice => choice.index === value);
        return index < 0 ? 0 : index;
    }
    function sameId(first, second) {
        return first.toLowerCase().replace(/-/g, "") === second.toLowerCase().replace(/-/g, "");
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
        displayHint: Kirigami.DisplayHint.KeepVisible
        // A rewatch stopped halfway is in progress, not watched, as on its card
        text: page.details.played && !page.details.canResume ? i18n("Mark as Unwatched") : i18n("Mark as Watched")
        icon.name: page.details.played && !page.details.canResume ? "view-hidden" : "checkmark-symbolic"
        visible: page.details.status === ItemDetailsModel.Ready && page.details.canMarkPlayed
        enabled: !page.details.playedBusy
        onTriggered: page.details.togglePlayed()
    }
    readonly property Kirigami.Action mediaInfoAction: Kirigami.Action {
        objectName: "mediaInfoAction"
        displayHint: Kirigami.DisplayHint.KeepVisible
        text: i18n("Show Media Info")
        icon.name: "documentinfo-symbolic"
        visible: page.details.mediaSources.length > 0
        onTriggered: {
            if (page.mediaInfoDialog)
                page.mediaInfoDialog.open();
            else
                page.mediaInfoWanted = true;
        }
    }

    actions: [page.playOnAction, page.downloadAction, page.downloadMenuAction, page.favoriteAction, page.togglePlayedAction, page.mediaInfoAction, page.trailerAction, page.playNextAction, page.addToQueueAction, page.addToPlaylistAction, page.addToCollectionAction, page.stopDownloadsAction, page.serverEditMenuAction, page.copyStreamUrlAction, page.separatorDelete, page.deleteMediaAction]
    // The row of the page carries the way back and the actions, as on the Movies and Shows tabs; the hero names the item
    globalToolBarStyle: Kirigami.ApplicationHeaderStyle.None
    // Without Kirigami's title row the page names itself to a screen reader
    Accessible.role: Accessible.Pane
    Accessible.name: title
    header: PageToolbar {
        // Over the content: the hero's backdrop, and what scrolls up, pass under the row's shadow
        z: 1
        shadowBelow: true
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

    // The people of the item: the details page puts the row in its header, a season below its episodes
    component CastCrewSection: ColumnLayout {
        id: castCrew

        property var people: []

        Layout.fillWidth: true
        visible: people.length > 0
        spacing: Kirigami.Units.largeSpacing

        Kirigami.Heading {
            Layout.fillWidth: true
            level: 2
            text: i18n("Cast and Crew")
        }

        CardRow {
            Layout.fillWidth: true
            model: castCrew.people
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
                // The list is the one tab stop of its row
                focusPolicy: Qt.NoFocus
                highlighted: ListView.isCurrentItem && ListView.view.activeFocus
                padding: Kirigami.Units.smallSpacing
                Accessible.name: subtitle.length > 0 ? modelData.name + ", " + subtitle : modelData.name
                onClicked: page.Window.window.openPerson(modelData.id, modelData.name, page)

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
    }

    // Qt Quick has no clipboard API: a hidden TextEdit selects its text and copies it
    TextEdit {
        id: clipboard
        objectName: "detailsClipboard"
        visible: false
        textFormat: TextEdit.PlainText
        Accessible.ignored: true
    }

    // A refresh that failed after the request was accepted: the action's notification already said it started
    Connections {
        target: page.connection
        function onItemRefreshFinished(itemId, ok, status) {
            if (!ok && itemId === page.details.itemId && page.Window.window)
                page.Window.window.showPassiveNotification(i18n("The metadata refresh failed"));
        }
        function onItemDeleted(itemId) {
            if (!page.sameId(itemId, page.details.itemId))
                return;
            page.Window.window.showPassiveNotification(i18n("Deleted"));
            if (page.isCurrentPage)
                page.Window.window.escapeBack();
        }
        function onItemDeleteFailed(itemId, status) {
            if (!page.sameId(itemId, page.details.itemId))
                return;
            if (status === 403)
                page.Window.window.showPassiveNotification(i18n("The server did not allow the deletion of this item"));
            else
                page.Window.window.showPassiveNotification(i18n("The item could not be deleted: check the connection to the server and try again"));
        }
        function onItemUpdated(itemId) {
            if (!page.sameId(itemId, page.details.itemId))
                return;
            page.details.reload();
            page.Window.window.showPassiveNotification(i18n("Metadata saved"));
        }
        function onItemUpdateFailed(itemId, status) {
            if (page.sameId(itemId, page.details.itemId))
                page.Window.window.showPassiveNotification(i18n("The metadata could not be saved: check the connection to the server and try again"));
        }
    }

    EditMetadataDialog {
        id: editMetadataDialog
        connection: page.connection
        details: page.details
        shell: page.Window.window
    }

    IdentifyDialog {
        id: identifyDialog
        connection: page.connection
        details: page.details
        shell: page.Window.window
    }

    EditImagesDialog {
        id: editImagesDialog
        connection: page.connection
        details: page.details
        shell: page.Window.window
    }

    EditSubtitlesDialog {
        id: editSubtitlesDialog
        connection: page.connection
        details: page.details
        shell: page.Window.window
    }

    Kirigami.PromptDialog {
        id: deleteDialog
        objectName: "deleteMediaDialog"

        function confirm() {
            close();
            if (!page.connection.deleteItem(page.details.itemId))
                page.Window.window.showPassiveNotification(i18n("The item could not be deleted: wait for the changes on their way and try again"));
        }

        title: i18n("Delete Media")
        standardButtons: Kirigami.Dialog.Cancel
        customFooterActions: [
            Kirigami.Action {
                text: i18n("Delete")
                icon.name: "edit-delete"
                onTriggered: deleteDialog.confirm()
            }
        ]

        Controls.Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
            text: i18n("Delete this item from the server? The file is removed and cannot be recovered")
        }
    }

    ListView {
        id: list

        activeFocusOnTab: count > 0
        keyNavigationEnabled: true
        // Nothing is current until the user moves into the rows: a season opens without an episode lit
        currentIndex: -1
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
                    visible: page.shownBox === "item"
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

                // One box for both updates, so the two failures never stack: Retry repeats whichever failed
                MessageBox {
                    objectName: "detailsUpdateErrorBox"
                    visible: page.shownBox === "update"
                    type: Kirigami.MessageType.Error
                    text: {
                        if (page.details.favoriteError && page.details.playedError)
                            return i18n("Could not update Favorites and watched status. Check the connection and try again");
                        return page.details.favoriteError ? i18n("Could not update Favorites. Check the connection and try again") : i18n("Could not update watched status. Check the connection and try again");
                    }
                    actions: [
                        Kirigami.Action {
                            text: i18n("Retry")
                            icon.name: "view-refresh"
                            onTriggered: {
                                if (page.details.favoriteError)
                                    page.details.toggleFavorite();
                                if (page.details.playedError)
                                    page.details.togglePlayed();
                            }
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
                                // The fade above the text: up to seven grid units, less where the image has no more room above the text block
                                readonly property real fadeLength: hero.overlay ? Math.max(Kirigami.Units.gridUnit * 3, Math.min(parent.height - heroText.implicitHeight - page.margin, Kirigami.Units.gridUnit * 7)) : Kirigami.Units.gridUnit * 4
                                // Where the text block starts inside the scrim: the fade is measured from it, not from the image's bottom
                                readonly property real textTop: Math.max(0, height - page.margin - heroText.implicitHeight)
                                readonly property real fadeTop: hero.overlay ? Math.min(1, textTop / height) : 1
                                // How opaque the fade is where it ends, at the top of the text block; under the text it goes on to the full colour
                                readonly property real topAlpha: hero.overlay ? 0.75 : 1

                                anchors {
                                    left: parent.left
                                    right: parent.right
                                    bottom: parent.bottom
                                }
                                height: hero.overlay ? Math.min(parent.height, heroText.implicitHeight + page.margin + fadeLength) : fadeLength
                                // A smoothstep ramp (3t² - 2t³) sampled at t = 0, .15, .3, .45, .6, .75, .9, 1: it starts and ends level, so neither end of the fade shows as an edge
                                gradient: Gradient {
                                    GradientStop {
                                        position: 0
                                        color: Qt.alpha(scrim.fade, 0)
                                    }
                                    GradientStop {
                                        position: scrim.fadeTop * 0.15
                                        color: Qt.alpha(scrim.fade, scrim.topAlpha * 0.06)
                                    }
                                    GradientStop {
                                        position: scrim.fadeTop * 0.3
                                        color: Qt.alpha(scrim.fade, scrim.topAlpha * 0.22)
                                    }
                                    GradientStop {
                                        position: scrim.fadeTop * 0.45
                                        color: Qt.alpha(scrim.fade, scrim.topAlpha * 0.43)
                                    }
                                    GradientStop {
                                        position: scrim.fadeTop * 0.6
                                        color: Qt.alpha(scrim.fade, scrim.topAlpha * 0.65)
                                    }
                                    GradientStop {
                                        position: scrim.fadeTop * 0.75
                                        color: Qt.alpha(scrim.fade, scrim.topAlpha * 0.84)
                                    }
                                    GradientStop {
                                        position: scrim.fadeTop * 0.9
                                        color: Qt.alpha(scrim.fade, scrim.topAlpha * 0.97)
                                    }
                                    GradientStop {
                                        position: scrim.fadeTop
                                        color: Qt.alpha(scrim.fade, scrim.topAlpha)
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

                            // A season reads as a path on one line, its series then its name; any other page stacks the name over the links
                            GridLayout {
                                id: titleBlock

                                readonly property bool seriesBeside: page.details.type === "Season" && page.showSeriesLink && !hero.narrow

                                Layout.fillWidth: true
                                columns: seriesBeside ? 3 : 1
                                columnSpacing: Kirigami.Units.largeSpacing
                                rowSpacing: heroText.spacing

                                // An episode or a season shows its series' logo: its own name stays under it, a size smaller
                                Kirigami.Heading {
                                    id: detailsTitle
                                    objectName: "detailsTitle"
                                    readonly property bool underParentLogo: logo.status === Image.Ready && (page.details.type === "Episode" || page.details.type === "Season")
                                    Layout.fillWidth: true
                                    Layout.row: 0
                                    Layout.column: titleBlock.seriesBeside ? 2 : 0
                                    Layout.alignment: titleBlock.seriesBeside ? Qt.AlignBottom : Qt.AlignTop
                                    visible: logo.status !== Image.Ready || underParentLogo
                                    level: underParentLogo ? 2 : 1
                                    wrapMode: Text.Wrap
                                    // Item names come from the server: plain text only
                                    textFormat: Text.PlainText
                                    text: page.details.name
                                }

                                Flow {
                                    Layout.fillWidth: !titleBlock.seriesBeside
                                    Layout.row: titleBlock.seriesBeside ? 0 : 1
                                    Layout.column: 0
                                    Layout.alignment: titleBlock.seriesBeside ? Qt.AlignBottom : Qt.AlignTop
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
                                    Layout.row: 0
                                    Layout.column: 1
                                    Layout.alignment: Qt.AlignBottom
                                    visible: titleBlock.seriesBeside
                                    opacity: 0.7
                                    textFormat: Text.PlainText
                                    text: "›"
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

                                // Beside the main action, on its left
                                ActionButton {
                                    objectName: "detailsPlayFromBeginning"
                                    visible: !hero.narrow && command.visible
                                    command: page.playFromBeginningAction
                                    Accessible.description: page.startDescription
                                }

                                ActionButton {
                                    id: playWide
                                    objectName: "detailsPlay"
                                    visible: !hero.narrow && command.visible
                                    command: page.playAction
                                    Accessible.description: page.playDescription
                                    highlighted: true
                                    Component.onCompleted: page.widePlayButton = playWide
                                }
                            }
                        }
                    }

                    // The buttons of the hero on a narrow window, above the track selectors, sharing the width: the other actions are in the page toolbar
                    RowLayout {
                        Layout.fillWidth: true
                        visible: hero.narrow && (page.playAction.visible || page.playFromBeginningAction.visible)
                        spacing: Kirigami.Units.smallSpacing

                        ActionButton {
                            objectName: "detailsPlayFromBeginningNarrow"
                            Layout.fillWidth: true
                            Layout.preferredWidth: 0
                            visible: command.visible
                            command: page.playFromBeginningAction
                            Accessible.description: page.startDescription
                        }

                        ActionButton {
                            id: playNarrow
                            objectName: "detailsPlayNarrow"
                            Layout.fillWidth: true
                            Layout.preferredWidth: 0
                            visible: command.visible
                            command: page.playAction
                            Accessible.description: page.playDescription
                            highlighted: true
                            Component.onCompleted: page.narrowPlayButton = playNarrow
                        }
                    }

                    // The tracks picked before playback: the server's indexes go in the PlaybackInfo request and select the track once the file loads
                    Flow {
                        id: streamSelectors
                        Layout.fillWidth: true
                        visible: page.audioChoices.length > 1 || page.subtitleChoices.length > 2
                        // The two selectors stand apart, each with its label right before its box
                        spacing: Kirigami.Units.gridUnit
                        // The two boxes share one capped width so they line up; the full track names live in the wider popup
                        readonly property real boxWidth: Math.min(Kirigami.Units.gridUnit * 13, Math.max(Kirigami.Units.gridUnit * 6, width - Kirigami.Units.gridUnit * 6))

                        RowLayout {
                            spacing: Kirigami.Units.largeSpacing

                            RowLayout {
                                spacing: Kirigami.Units.smallSpacing
                                visible: page.audioChoices.length > 1

                                Kirigami.Icon {
                                    source: "audio-headphones"
                                    implicitWidth: Kirigami.Units.iconSizes.small
                                    implicitHeight: Kirigami.Units.iconSizes.small
                                    Accessible.ignored: true
                                }
                                Controls.Label {
                                    id: audioLabel
                                    text: i18n("Audio")
                                }
                            }
                            TextMetrics {
                                id: audioNameMetrics
                                font: audioSelector.font
                                text: page.longestLabel(page.audioChoices)
                            }
                            Controls.ComboBox {
                                id: audioSelector
                                objectName: "audioSelector"
                                visible: page.audioChoices.length > 1
                                enabled: !(page.Window.window?.groupRouting ?? false)
                                Layout.preferredWidth: streamSelectors.boxWidth
                                Layout.maximumWidth: streamSelectors.boxWidth
                                // The list may go wider than the box, so long track names stay readable, and never wider than the page
                                popup.width: Math.min(Math.max(audioSelector.width, audioNameMetrics.advanceWidth + Kirigami.Units.gridUnit * 3), page.width - Kirigami.Units.gridUnit * 2)
                                model: page.audioChoices
                                textRole: "label"
                                Accessible.name: i18n("Audio track")
                                onActivated: index => page.audioStreamIndex = page.audioChoices[index].index
                                // The list opens below the box and above it when there is no room below, never over it
                                PopupPlacement { popup: audioSelector.popup; anchor: audioSelector }
                                // Written by the Binding so a reload that drops the choice can move it back
                                Binding {
                                    target: audioSelector
                                    property: "currentIndex"
                                    value: page.choiceIndex(page.audioChoices, page.audioStreamIndex)
                                    restoreMode: Binding.RestoreNone
                                }
                                // The style's own rows read server titles as mnemonic rich text: a plain label keeps them literal
                                delegate: Controls.MenuItem {
                                    id: audioChoice

                                    required property int index
                                    required property var modelData

                                    objectName: "audioChoice_" + index
                                    text: modelData.label
                                    highlighted: audioSelector.highlightedIndex === index
                                    Kirigami.MnemonicData.enabled: false
                                    contentItem: Controls.Label {
                                        text: audioChoice.text
                                        textFormat: Text.PlainText
                                        elide: Text.ElideRight
                                        font: audioChoice.font
                                        verticalAlignment: Text.AlignVCenter
                                        color: audioChoice.highlighted || audioChoice.pressed ? Kirigami.Theme.highlightedTextColor : Kirigami.Theme.textColor
                                    }
                                }
                            }
                        }

                        RowLayout {
                            spacing: Kirigami.Units.largeSpacing

                            RowLayout {
                                spacing: Kirigami.Units.smallSpacing
                                visible: page.subtitleChoices.length > 2

                                Kirigami.Icon {
                                    source: "media-view-subtitles-symbolic"
                                    implicitWidth: Kirigami.Units.iconSizes.small
                                    implicitHeight: Kirigami.Units.iconSizes.small
                                    Accessible.ignored: true
                                }
                                Controls.Label {
                                    id: subtitleLabel
                                    text: i18n("Subtitles")
                                }
                            }
                            TextMetrics {
                                id: subtitleNameMetrics
                                font: subtitleSelector.font
                                text: page.longestLabel(page.subtitleChoices)
                            }
                            Controls.ComboBox {
                                id: subtitleSelector
                                objectName: "subtitleSelector"
                                visible: page.subtitleChoices.length > 2
                                enabled: !(page.Window.window?.groupRouting ?? false)
                                Layout.preferredWidth: streamSelectors.boxWidth
                                Layout.maximumWidth: streamSelectors.boxWidth
                                // The list may go wider than the box, so long track names stay readable, and never wider than the page
                                popup.width: Math.min(Math.max(subtitleSelector.width, subtitleNameMetrics.advanceWidth + Kirigami.Units.gridUnit * 3), page.width - Kirigami.Units.gridUnit * 2)
                                model: page.subtitleChoices
                                textRole: "label"
                                Accessible.name: i18n("Subtitle track")
                                onActivated: index => page.subtitleStreamIndex = page.subtitleChoices[index].index
                                // The list opens below the box and above it when there is no room below, never over it
                                PopupPlacement { popup: subtitleSelector.popup; anchor: subtitleSelector }
                                // Written by the Binding so a reload that drops the choice can move it back
                                Binding {
                                    target: subtitleSelector
                                    property: "currentIndex"
                                    value: page.choiceIndex(page.subtitleChoices, page.subtitleStreamIndex)
                                    restoreMode: Binding.RestoreNone
                                }
                                // The style's own rows read server titles as mnemonic rich text: a plain label keeps them literal
                                delegate: Controls.MenuItem {
                                    id: subtitleChoice

                                    required property int index
                                    required property var modelData

                                    objectName: "subtitleChoice_" + index
                                    text: modelData.label
                                    highlighted: subtitleSelector.highlightedIndex === index
                                    Kirigami.MnemonicData.enabled: false
                                    contentItem: Controls.Label {
                                        text: subtitleChoice.text
                                        textFormat: Text.PlainText
                                        elide: Text.ElideRight
                                        font: subtitleChoice.font
                                        verticalAlignment: Text.AlignVCenter
                                        color: subtitleChoice.highlighted || subtitleChoice.pressed ? Kirigami.Theme.highlightedTextColor : Kirigami.Theme.textColor
                                    }
                                }
                            }
                        }
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
                        virtualItem: page.nextUp.virtualItem ?? false
                        unaired: page.nextUp.unaired ?? false
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
                        visible: page.shownBox === "seasons"
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
                        visible: page.shownBox === "collection"
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

                    CastCrewSection {
                        Layout.fillWidth: true
                        people: page.childModel.kind === ItemChildrenModel.Season ? [] : page.details.people
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
                        visible: page.shownBox === "similar"
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
                        visible: page.shownBox === "episodes"
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
            x: (list.width - width) / 2
            width: page.listColumnWidth
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
            virtualItem: model.virtualItem
            unaired: model.unaired
            loaded: model.loaded
            onClicked: page.Window.window.openItem(itemId, page)
        }

        footer: Item {
            width: list.width
            // The warning has an action at its trailing end, where the mini player docks: it lifts the box above the dock; the people of a season follow the episodes, with the page's bottom margin
            height: footerContent.implicitHeight + page.margin + (footerCast.visible ? page.margin : 0) + (episodesWarning.visible ? (page.Window.window?.miniPlayerSpace ?? 0) : 0)

            ColumnLayout {
                id: footerContent

                x: (parent.width - width) / 2
                y: page.margin
                width: page.listColumnWidth

                MessageBox {
                    id: episodesWarning
                    visible: page.shownBox === "episodesPartial"
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

                CastCrewSection {
                    id: footerCast
                    Layout.fillWidth: true
                    people: page.childModel.kind === ItemChildrenModel.Season ? page.details.people : []
                }
            }
        }
    }
}
