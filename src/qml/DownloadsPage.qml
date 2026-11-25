// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// The Downloads tab: the active queue first, then what is available on this device. Every row is an item the engine owns; the page never builds a path and never talks to the server. A server that denies downloads still lists what was downloaded before, with the reason at the top
Kirigami.ScrollablePage {
    id: page
    objectName: "downloadsPage"

    // The darker View background on every page: a page without a list (a plain flickable) would get the lighter Window one, and the sections would not match
    Kirigami.Theme.colorSet: Kirigami.Theme.View
    Kirigami.Theme.inherit: false

    required property ConnectionController connection

    readonly property DownloadsController downloads: connection.downloads
    readonly property DownloadsModel downloadsModel: downloads.model
    // The rows, their headers and the notes above them are capped at the shell's reading width and centred, as the other lists are
    readonly property real sideMargin: Math.max(0, (list.width - (page.Window.window?.readingWidth ?? list.width)) / 2)

    title: i18n("Downloads")
    // As the Movies and Shows tabs: no title bar, the toolbar above the list names the page in a Compact window
    globalToolBarStyle: Kirigami.ApplicationHeaderStyle.None
    Accessible.role: Accessible.Pane
    Accessible.name: title

    // The mini player docks above the footer instead of over its counts
    readonly property real miniPlayerClearance: footer.height

    header: PageToolbar {
        title: (page.Window.window?.compactHeader ?? false) ? page.title : ""
        actions: page.actions
        toolbarObjectName: "downloadsToolbar"
        titleObjectName: "downloadsHeading"
    }

    // The rules that add episodes by themselves have a page of their own above this one
    actions: [
        Kirigami.Action {
            objectName: "downloadRulesAction"
            text: i18n("Automatic Downloads")
            icon.name: "download-later-symbolic"
            onTriggered: page.Window.window.openDownloadRules()
        }
    ]

    Component.onCompleted: {
        if (!connection.offline)
            downloads.reconcileOutbox();
    }

    // Read once per change: the box, the header and the resolution share it
    readonly property var outboxConflicts: downloads.outboxConflicts

    // The list takes the keys as soon as the page shows
    function focusPrimary() {
        list.forceActiveFocus(Qt.TabFocusReason);
    }

    function stateText(state) {
        switch (state) {
        case "queued":
            return i18n("Queued");
        case "fetching":
            return i18n("Downloading");
        case "paused":
            return i18n("Paused");
        case "waiting":
            return i18n("Waiting for the network");
        case "error":
            return i18n("Failed");
        case "completed":
            return i18n("Downloaded");
        case "exported":
            return i18n("Exported");
        }
        return "";
    }

    function errorText(error) {
        switch (error) {
        case "network":
            return i18n("No connection to the server: check the network, then retry");
        case "http":
            return i18n("The server refused the download: check that this account may download, then retry");
        case "protocol":
            return i18n("The server’s answer was not understood: retry later");
        case "size":
            return i18n("The file size did not match what the server announced: retry");
        case "changed":
            return i18n("The file changed on the server: retry to download the new version");
        case "io":
            return i18n("The file could not be written: check the permissions and the free space, then retry");
        case "nospace":
            return i18n("Not enough disk space: free some space, then retry");
        case "cancelled":
            return i18n("Cancelled");
        }
        return "";
    }

    function detailText(seriesName, seasonNumber, episodeNumber, year) {
        let line = seriesName;
        if (seasonNumber > 0 && episodeNumber > 0)
            line += (line.length > 0 ? " · " : "") + EpisodeLabel.label(seasonNumber, episodeNumber);
        else if (year > 0)
            line += (line.length > 0 ? " · " : "") + year;
        return line;
    }

    // The speed in the unit family of the sizes: "12.4 MB/s"; empty when nothing moves
    function speedText(bytesPerSecond) {
        return bytesPerSecond > 0 ? i18nc("@info a transfer speed, %1 is a size such as 12.4 MB", "%1/s", page.downloads.formattedSize(bytesPerSecond)) : "";
    }

    // What is left at the current speed, to the minute; empty without a size or a speed
    function timeLeftText(size, bytesWritten, bytesPerSecond) {
        if (size <= 0 || bytesPerSecond <= 0 || bytesWritten >= size)
            return "";
        const minutes = Math.round((size - bytesWritten) / bytesPerSecond / 60);
        if (minutes < 1)
            return i18n("less than a minute left");
        if (minutes < 60)
            return i18n("about %1 min left", minutes);
        if (minutes < 24 * 60)
            return i18nc("@info time left, hours and minutes", "about %1 h %2 min left", Math.floor(minutes / 60), minutes % 60);
        return i18n("more than a day left");
    }

    // The one muted line of a row: while it fetches the parts that are known (bytes of the total, percentage, speed, time left), else the state in plain words
    function rowStatus(state, error, size, bytesWritten, progress, bytesPerSecond) {
        if (state === "fetching") {
            const parts = [];
            if (size > 0) {
                parts.push(i18nc("@info download progress, bytes written of the total size", "%1 of %2", page.downloads.formattedSize(bytesWritten), page.downloads.formattedSize(size)));
                parts.push(i18nc("@info download progress in percent", "%1%", Math.floor(Math.max(0, progress) * 100)));
            } else if (bytesWritten > 0) {
                parts.push(page.downloads.formattedSize(bytesWritten));
            }
            const speed = page.speedText(bytesPerSecond);
            if (speed.length > 0)
                parts.push(speed);
            const left = page.timeLeftText(size, bytesWritten, bytesPerSecond);
            if (left.length > 0)
                parts.push(left);
            return parts.length > 0 ? parts.join(" · ") : page.stateText(state);
        }
        if (state === "completed" || state === "exported")
            return page.downloads.formattedSize(size);
        if (state === "error")
            return page.errorText(error);
        return page.stateText(state);
    }

    function pauseRow(serverId, userId, itemId) {
        page.downloads.pauseDownload(serverId, userId, itemId);
    }

    function resumeRow(serverId, userId, itemId) {
        page.downloads.retryDownload(serverId, userId, itemId);
    }

    function cancelRow(serverId, userId, itemId) {
        page.downloads.removeDownload(serverId, userId, itemId);
    }

    // The conflicts are resolved together: the summary box has no choice per item
    function resolveConflicts(keepServer) {
        for (const conflict of page.outboxConflicts)
            page.downloads.resolveOutboxConflict(conflict.serverId, conflict.userId, conflict.itemId, keepServer);
    }

    function askRemove(serverId, userId, itemId, name) {
        removeDialog.serverId = serverId;
        removeDialog.userId = userId;
        removeDialog.itemId = itemId;
        removeDialog.itemName = name;
        removeDialog.open();
    }

    Kirigami.PromptDialog {
        id: removeDialog
        objectName: "downloadsPageRemoveDialog"

        property string serverId
        property string userId
        property string itemId
        property string itemName

        title: i18n("Delete Download")
        standardButtons: Kirigami.Dialog.Cancel
        customFooterActions: [
            Kirigami.Action {
                objectName: "removeDownloadConfirm"
                text: i18n("Delete Download")
                icon.name: "edit-delete"
                onTriggered: {
                    page.downloads.removeDownload(removeDialog.serverId, removeDialog.userId, removeDialog.itemId);
                    removeDialog.close();
                }
            }
        ]

        Controls.Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
            text: removeDialog.itemName.length > 0
                ? i18n("Delete %1? The file stays on the server", removeDialog.itemName)
                : i18n("Delete this download? The file stays on the server")
        }
    }

    FolderDialog {
        id: exportDialog

        property string serverId
        property string userId
        property string itemId

        title: i18n("Export Download")
        onAccepted: page.connection.downloads.exportDownload(serverId, userId, itemId, selectedFolder)
    }

    ListView {
        id: list
        objectName: "downloadsList"
        // Room under the bar: the rows scroll up to its line, but open clear of it
        topMargin: Kirigami.Units.largeSpacing * 2

        // On the view, not in its content: the shadow stays under the toolbar while the rows scroll
        TopShadow {
            parent: list
            z: 1
            atStart: list.atYBeginning
        }

        model: page.downloadsModel
        activeFocusOnTab: true
        keyNavigationEnabled: true
        // No row is current until the keyboard picks one: the list must not open with a full highlight
        currentIndex: -1
        Accessible.role: Accessible.List
        Accessible.name: page.title

        Keys.onReturnPressed: {
            const item = list.currentItem;
            if (item && item.downloaded)
                page.Window.window.playDownload(item.serverId, item.userId, item.itemId);
        }
        Keys.onMenuPressed: {
            if (list.currentItem)
                list.currentItem.openMenu();
        }
        Keys.onDeletePressed: {
            const item = list.currentItem;
            if (item && item.removable)
                page.askRemove(item.serverId, item.userId, item.itemId, item.name);
        }
        Keys.onPressed: event => {
            if (event.modifiers === Qt.ShiftModifier && event.key === Qt.Key_F10 && list.currentItem) {
                list.currentItem.openMenu();
                event.accepted = true;
            }
        }

        // A row is a record of the queue; the list takes the keys as the Queue page's does: Return plays, Menu or Shift+F10 opens the row's menu, Delete asks to delete
        delegate: Controls.ItemDelegate {
            id: row

            required property int index
            required property string serverId
            required property string userId
            required property string itemId
            required property string name
            required property string seriesName
            required property string type
            required property string primaryTag
            required property int seasonNumber
            required property int episodeNumber
            required property int year
            required property string recordState
            required property string error
            required property real size
            required property real bytesWritten
            required property real progress
            required property real bytesPerSecond
            required property bool active
            required property bool downloaded
            // One status line shared by the row and its accessible description
            readonly property string statusText: page.rowStatus(recordState, error, size, bytesWritten, progress, bytesPerSecond)
            readonly property bool cancellable: ["queued", "fetching", "waiting"].indexOf(recordState) >= 0
            readonly property bool removable: ["paused", "error", "completed", "exported"].indexOf(recordState) >= 0
            readonly property bool hasMenu: cancellable || removable

            function openMenu() {
                if (hasMenu)
                    rowMenu.open();
            }

            objectName: "downloadRow_" + itemId
            // The row spans the list and its visible part is capped at the reading width and centred: a ListView resets the x of its delegates, so the side margins are insets
            width: list.width
            leftInset: page.sideMargin
            rightInset: page.sideMargin
            leftPadding: page.sideMargin + Kirigami.Units.largeSpacing
            rightPadding: page.sideMargin + Kirigami.Units.largeSpacing
            topPadding: Kirigami.Units.smallSpacing
            bottomPadding: Kirigami.Units.smallSpacing
            containmentMask: QtObject {
                function contains(point: point): bool {
                    return point.x >= page.sideMargin && point.x <= row.width - page.sideMargin;
                }
            }
            highlighted: ListView.isCurrentItem && ListView.view.activeFocus && (ListView.view.Window.window?.keyboardDriven ?? true)
            // The style's row background reads this hint, not Kirigami.Theme.useAlternateBackgroundColor, and the row's index
            Kirigami.StyleHints.useAlternateBackgroundColor: true
            // Only a downloaded row acts on a click: no hover feedback elsewhere
            hoverEnabled: downloaded
            onClicked: {
                if (row.downloaded)
                    page.Window.window.playDownload(row.serverId, row.userId, row.itemId);
            }
            Accessible.name: row.name
            Accessible.description: row.statusText

            contentItem: RowLayout {
                spacing: Kirigami.Units.largeSpacing

                Item {
                    Layout.alignment: Qt.AlignVCenter
                    Layout.preferredWidth: Kirigami.Units.gridUnit * 2.25
                    Layout.preferredHeight: Kirigami.Units.gridUnit * 3.375

                    Kirigami.Icon {
                        anchors.centerIn: parent
                        width: Kirigami.Units.iconSizes.medium
                        height: Kirigami.Units.iconSizes.medium
                        source: row.type === "Episode" ? "video-television" : "video-symbolic"
                        visible: poster.status !== Image.Ready
                        Accessible.ignored: true
                    }
                    Image {
                        id: poster
                        objectName: "downloadPoster_" + row.itemId
                        anchors.fill: parent
                        // Only URLs built by JellyfinClient::imageUrl reach the image fetcher's allowlist
                        source: {
                            // The poster may be written after the completion: a finished row follows the queue, a running one has none to find and skips the progress updates
                            if (row.downloaded)
                                page.connection.downloads.revision;
                            return page.connection.downloads.posterSource(row.serverId, row.userId, row.itemId, row.primaryTag);
                        }
                        fillMode: Image.PreserveAspectCrop
                        asynchronous: true
                        sourceSize.width: width * Screen.devicePixelRatio
                        sourceSize.height: height * Screen.devicePixelRatio
                        visible: status === Image.Ready
                    }
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignVCenter
                    spacing: Kirigami.Units.smallSpacing / 2

                    Controls.Label {
                        id: nameLabel
                        Layout.fillWidth: true
                        text: row.name
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                        font.weight: Font.DemiBold
                        color: row.highlighted ? Kirigami.Theme.highlightedTextColor : Kirigami.Theme.textColor
                        HoverHandler { id: nameHover }
                        PlainToolTip {
                            visible: nameLabel.truncated && nameHover.hovered
                            text: nameLabel.text
                        }
                    }
                    Controls.Label {
                        Layout.fillWidth: true
                        visible: text.length > 0
                        text: page.detailText(row.seriesName, row.seasonNumber, row.episodeNumber, row.year)
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                        color: row.highlighted ? Kirigami.Theme.highlightedTextColor : Kirigami.Theme.disabledTextColor
                        font: Kirigami.Theme.smallFont
                    }
                    Controls.ProgressBar {
                        objectName: "downloadProgress_" + row.itemId
                        Layout.fillWidth: true
                        // Every unfinished row keeps its bar, so a queued row that starts does not change the height of the rows below it
                        visible: row.active && row.recordState !== "error"
                        from: 0
                        to: 1
                        // An unknown size has no fraction to show
                        indeterminate: row.recordState === "fetching" && (row.size <= 0 || row.progress < 0)
                        value: Math.max(0, row.progress)
                    }
                    Controls.Label {
                        objectName: "downloadStatus_" + row.itemId
                        Layout.fillWidth: true
                        text: row.statusText
                        textFormat: Text.PlainText
                        // An error ends with what to do: it wraps instead of losing that end
                        wrapMode: row.recordState === "error" ? Text.Wrap : Text.NoWrap
                        maximumLineCount: row.recordState === "error" ? 2 : 1
                        elide: Text.ElideRight
                        color: row.highlighted ? Kirigami.Theme.highlightedTextColor : (row.recordState === "error" ? Kirigami.Theme.negativeTextColor : Kirigami.Theme.disabledTextColor)
                        font: Kirigami.Theme.smallFont
                    }
                }
                Controls.ToolButton {
                    objectName: "downloadPlay_" + row.itemId
                    Layout.alignment: Qt.AlignVCenter
                    // The one action the row's state leads to: Play, Resume, Retry or Pause
                    readonly property string rowAction: row.downloaded ? "play"
                        : row.recordState === "paused" ? "resume"
                        : row.recordState === "error" ? "retry"
                        : row.cancellable ? "pause" : ""
                    visible: rowAction.length > 0
                    text: rowAction === "play" ? i18n("Play")
                        : rowAction === "resume" ? i18n("Resume")
                        : rowAction === "retry" ? i18n("Retry") : i18n("Pause")
                    icon.name: rowAction === "retry" ? "view-refresh" : rowAction === "pause" ? "media-playback-pause" : "media-playback-start"
                    display: Controls.AbstractButton.IconOnly
                    Accessible.name: rowAction === "play" ? i18nc("@action:button %1 is the item", "Play %1", row.name)
                        : rowAction === "resume" ? i18nc("@action:button %1 is the item", "Resume %1", row.name)
                        : rowAction === "retry" ? i18nc("@action:button %1 is the item", "Retry %1", row.name)
                        : i18nc("@action:button %1 is the item", "Pause %1", row.name)
                    Controls.ToolTip.text: text
                    Controls.ToolTip.visible: hovered || visualFocus
                    Controls.ToolTip.delay: Kirigami.Units.toolTipDelay
                    onClicked: {
                        switch (rowAction) {
                        case "play":
                            page.Window.window.playDownload(row.serverId, row.userId, row.itemId);
                            break;
                        case "pause":
                            page.pauseRow(row.serverId, row.userId, row.itemId);
                            break;
                        default:
                            page.resumeRow(row.serverId, row.userId, row.itemId);
                        }
                    }
                }
                Controls.ToolButton {
                    id: rowMore
                    objectName: "downloadMore_" + row.itemId
                    Layout.alignment: Qt.AlignVCenter
                    visible: row.hasMenu
                    text: i18n("More Actions")
                    icon.name: "overflow-menu"
                    display: Controls.AbstractButton.IconOnly
                    Accessible.name: i18nc("@action:button %1 is the item", "More Actions for %1", row.name)
                    Controls.ToolTip.text: text
                    Controls.ToolTip.visible: hovered || visualFocus
                    Controls.ToolTip.delay: Kirigami.Units.toolTipDelay
                    onClicked: rowMenu.open()
                    // Below the button, above it when there is no room below
                    PopupPlacement { popup: rowMenu; anchor: rowMore }
                    Controls.Menu {
                        id: rowMenu
                        Controls.MenuItem {
                            objectName: "downloadExport_" + row.itemId
                            text: i18n("Export…")
                            icon.name: "document-export"
                            visible: row.downloaded
                            height: visible ? implicitHeight : 0
                            onTriggered: {
                                exportDialog.serverId = row.serverId;
                                exportDialog.userId = row.userId;
                                exportDialog.itemId = row.itemId;
                                exportDialog.open();
                            }
                        }
                        Controls.MenuItem {
                            objectName: "downloadCancel_" + row.itemId
                            text: i18n("Cancel Download")
                            icon.name: "process-stop"
                            visible: row.cancellable
                            height: visible ? implicitHeight : 0
                            onTriggered: page.cancelRow(row.serverId, row.userId, row.itemId)
                        }
                        Controls.MenuItem {
                            objectName: "downloadDelete_" + row.itemId
                            text: i18n("Delete Download…")
                            icon.name: "edit-delete"
                            visible: row.removable
                            height: visible ? implicitHeight : 0
                            onTriggered: page.askRemove(row.serverId, row.userId, row.itemId, row.name)
                        }
                    }
                }
            }
        }

        section.property: "section"
        section.delegate: Item {
            width: list.width
            height: sectionHeader.implicitHeight

            Kirigami.ListSectionHeader {
                id: sectionHeader
                x: page.sideMargin
                width: parent.width - page.sideMargin * 2
                text: section === "completed" ? i18n("Available on This Device") : i18n("Active Downloads")
            }
        }

        header: Item {
            width: list.width
            height: visible ? notices.implicitHeight + Kirigami.Units.largeSpacing * 2 : 0
            visible: (!page.downloads.canDownload && !page.connection.offline)
                     || page.downloads.storageFailed
                     || page.outboxConflicts.length > 0

            ColumnLayout {
                id: notices
                anchors {
                    left: parent.left
                    right: parent.right
                    top: parent.top
                    margins: Kirigami.Units.largeSpacing
                    leftMargin: page.sideMargin + Kirigami.Units.largeSpacing
                    rightMargin: page.sideMargin + Kirigami.Units.largeSpacing
                }
                spacing: Kirigami.Units.smallSpacing

                // One box at a time, the first that applies: the conflicts, the storage warning, the permission note
                MessageBox {
                    objectName: "downloadsOutboxConflicts"
                    Layout.fillWidth: true
                    visible: page.outboxConflicts.length > 0
                    type: Kirigami.MessageType.Warning
                    text: page.outboxConflicts.length === 1
                        ? i18n("%1 was played on another device without being marked as watched there. Keep the server’s state or mark it as watched here", page.outboxConflicts[0].name)
                        : i18n("Several downloads were played on another device without being marked as watched there. Keep the server’s state or mark them all as watched here")
                    actions: [
                        Kirigami.Action {
                            text: i18n("Keep Server State")
                            icon.name: "document-save"
                            onTriggered: page.resolveConflicts(true)
                        },
                        Kirigami.Action {
                            text: i18n("Mark as Watched")
                            icon.name: "checkmark-symbolic"
                            onTriggered: page.resolveConflicts(false)
                        }
                    ]
                }
                MessageBox {
                    objectName: "downloadsStorageWarning"
                    Layout.fillWidth: true
                    visible: page.outboxConflicts.length === 0 && page.downloads.storageFailed
                    type: Kirigami.MessageType.Warning
                    text: i18n("The download list could not be saved. Downloads stay available for this session only")
                }
                MessageBox {
                    objectName: "downloadsPermissionNote"
                    Layout.fillWidth: true
                    visible: page.outboxConflicts.length === 0 && !page.downloads.storageFailed && !page.downloads.canDownload && !page.connection.offline
                    type: Kirigami.MessageType.Information
                    text: i18n("This server does not allow downloads for this user. Downloaded items stay available on this device")
                }
            }
        }

        Kirigami.PlaceholderMessage {
            FadeInOnShow {}
            anchors.centerIn: parent
            width: parent.width - Kirigami.Units.gridUnit * 4
            visible: list.count === 0 && (page.downloads.canDownload || page.connection.offline)
            icon.name: "folder-download"
            text: i18n("No downloads yet")
            explanation: i18n("Use Download on a movie or an episode to keep it on this device")
            helpfulAction: Kirigami.Action {
                text: i18n("Browse the Library")
                icon.name: "go-home"
                onTriggered: page.Window.window.goHome()
            }
        }
    }

    footer: Controls.ToolBar {
        objectName: "downloadsFooter"
        // A line over the whole bar, as the bars at the top have one under them
        background: Item {
            Kirigami.Separator {
                anchors {
                    left: parent.left
                    right: parent.right
                    top: parent.top
                }
                Accessible.ignored: true
            }
        }
        // The page margin on both sides, as the bars at the top have it
        leftPadding: Kirigami.Units.largeSpacing
        rightPadding: Kirigami.Units.largeSpacing

        contentItem: RowLayout {
            spacing: Kirigami.Units.largeSpacing

            Controls.Label {
                id: footerCount
                objectName: "downloadsFooterCount"
                Layout.fillWidth: true
                textFormat: Text.PlainText
                elide: Text.ElideRight
                text: page.downloadsModel.count > 0 ? i18np("%1 download", "%1 downloads", page.downloadsModel.count) : i18nc("@info status line under the list of downloads, with none", "No downloads")
            }
            Controls.Label {
                objectName: "downloadsFooterSpeed"
                visible: page.downloads.bytesPerSecond > 0
                textFormat: Text.PlainText
                color: Kirigami.Theme.disabledTextColor
                text: page.speedText(page.downloads.bytesPerSecond)
            }
            Controls.Label {
                visible: page.downloads.pendingSyncCount > 0
                textFormat: Text.PlainText
                color: Kirigami.Theme.disabledTextColor
                text: i18np("%1 item to sync", "%1 items to sync", page.downloads.pendingSyncCount)
            }
            Controls.Label {
                visible: page.downloadsModel.storedBytes > 0
                textFormat: Text.PlainText
                color: Kirigami.Theme.disabledTextColor
                text: i18nc("@info disk space used by the downloaded items", "%1 used", page.downloads.formattedSize(page.downloadsModel.storedBytes))
            }
        }
    }
}
