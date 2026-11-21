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

    required property ConnectionController connection

    readonly property DownloadsController downloads: connection.downloads
    readonly property DownloadsModel downloadsModel: downloads.model

    title: i18n("Downloads")
    // As the Movies and Shows tabs: no title bar, the toolbar above the list names the page in a Compact window
    globalToolBarStyle: Kirigami.ApplicationHeaderStyle.None
    Accessible.role: Accessible.Pane
    Accessible.name: title

    // The mini player docks above the footer instead of over its counts
    readonly property real miniPlayerClearance: footer.height

    header: PageToolbar {
        title: (page.Window.window?.compactHeader ?? false) ? page.title : ""
        toolbarObjectName: "downloadsToolbar"
        titleObjectName: "downloadsHeading"
    }

    Component.onCompleted: {
        if (!connection.offline)
            downloads.reconcileOutbox();
    }

    readonly property int rulesRevision: downloads.rulesRevision
    // The revision is part of the expression, so the binding follows the rule store
    readonly property var trackedRules: rulesRevision >= 0 ? downloads.seriesRules() : []

    function ruleKindText(rule) {
        switch (rule.kind) {
        case "nextN":
            return i18np("Next %1 episode", "Next %1 episodes", rule.count);
        case "season":
            return i18n("Season %1", rule.seasonNumber);
        case "newEpisodes":
            return i18n("New episodes");
        }
        return "";
    }

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

    function rowStatus(state, error, size, bytesWritten) {
        if (state === "fetching" && size > 0)
            return i18nc("@info download progress, bytes written of the total size", "%1 of %2", page.downloads.formattedSize(bytesWritten), page.downloads.formattedSize(size));
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
            required property bool active
            required property bool downloaded
            readonly property bool cancellable: ["queued", "fetching", "waiting"].indexOf(recordState) >= 0
            readonly property bool removable: ["paused", "error", "completed", "exported"].indexOf(recordState) >= 0
            readonly property bool hasMenu: cancellable || removable

            function openMenu() {
                if (hasMenu)
                    rowMenu.popup(rowMore, 0, rowMore.height);
            }

            objectName: "downloadRow_" + itemId
            width: list.width
            highlighted: ListView.isCurrentItem && ListView.view.activeFocus
            // Only a downloaded row acts on a click: no hover feedback elsewhere
            hoverEnabled: downloaded
            onClicked: {
                if (row.downloaded)
                    page.Window.window.playDownload(row.serverId, row.userId, row.itemId);
            }
            Accessible.name: row.name
            Accessible.description: page.rowStatus(row.recordState, row.error, row.size, row.bytesWritten)

            contentItem: RowLayout {
                spacing: Kirigami.Units.largeSpacing

                Item {
                    Layout.alignment: Qt.AlignVCenter
                    Layout.preferredWidth: 40
                    Layout.preferredHeight: 60

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
                            page.connection.downloads.revision; // the poster may have been written after the completion
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
                    spacing: Kirigami.Units.smallSpacing / 2

                    Controls.Label {
                        Layout.fillWidth: true
                        text: row.name
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                        font.weight: Font.DemiBold
                        color: row.highlighted ? Kirigami.Theme.highlightedTextColor : Kirigami.Theme.textColor
                        Controls.ToolTip.text: text
                        Controls.ToolTip.visible: truncated && nameHover.hovered
                        Controls.ToolTip.delay: Kirigami.Units.toolTipDelay
                        HoverHandler { id: nameHover }
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
                        Layout.fillWidth: true
                        Layout.maximumWidth: Kirigami.Units.gridUnit * 20
                        visible: row.recordState === "fetching"
                        from: 0
                        to: 1
                        // An unknown size has no fraction to show
                        indeterminate: row.size <= 0 || row.progress < 0
                        value: Math.max(0, row.progress)
                    }
                    Controls.Label {
                        Layout.fillWidth: true
                        text: page.rowStatus(row.recordState, row.error, row.size, row.bytesWritten)
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
                    onClicked: rowMenu.popup(rowMore, 0, rowMore.height)
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
        section.delegate: Kirigami.ListSectionHeader {
            width: list.width
            text: section === "completed" ? i18n("Available on This Device") : i18n("Active Downloads")
        }

        header: Item {
            width: list.width
            height: visible ? notices.implicitHeight + Kirigami.Units.largeSpacing * 2 : 0
            visible: (!page.downloads.canDownload && !page.connection.offline)
                     || page.downloads.storageFailed
                     || page.downloads.outboxConflicts.length > 0
                     || page.trackedRules.length > 0

            ColumnLayout {
                id: notices
                anchors {
                    left: parent.left
                    right: parent.right
                    top: parent.top
                    margins: Kirigami.Units.largeSpacing
                }
                spacing: Kirigami.Units.smallSpacing

                MessageBox {
                    objectName: "downloadsPermissionNote"
                    Layout.fillWidth: true
                    visible: !page.downloads.canDownload && !page.connection.offline
                    type: Kirigami.MessageType.Information
                    text: i18n("This server does not allow downloads for this user. Downloaded items stay available on this device")
                }
                MessageBox {
                    objectName: "downloadsStorageWarning"
                    Layout.fillWidth: true
                    visible: page.downloads.storageFailed
                    type: Kirigami.MessageType.Warning
                    text: i18n("The download list could not be saved. Downloads stay available for this session only")
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    visible: page.trackedRules.length > 0
                    spacing: 0

                    Kirigami.ListSectionHeader {
                        Layout.fillWidth: true
                        text: i18n("Tracked Series")
                    }
                    Repeater {
                        objectName: "trackedRulesRepeater"
                        model: page.trackedRules
                        delegate: RowLayout {
                            required property var modelData
                            required property int index
                            Layout.fillWidth: true
                            spacing: Kirigami.Units.largeSpacing

                            Controls.Label {
                                Layout.fillWidth: true
                                textFormat: Text.PlainText
                                elide: Text.ElideRight
                                text: modelData.seriesName.length > 0 ? modelData.seriesName : i18n("Unknown series")
                            }
                            Controls.Label {
                                textFormat: Text.PlainText
                                color: Kirigami.Theme.disabledTextColor
                                text: page.ruleKindText(modelData)
                            }
                            Controls.ToolButton {
                                objectName: "stopSeriesRule_" + index
                                text: i18n("Stop Automatic Downloads")
                                icon.name: "edit-delete-remove"
                                display: Controls.AbstractButton.IconOnly
                                Accessible.name: i18nc("@action:button %1 is the series", "Stop Automatic Downloads of %1", modelData.seriesName.length > 0 ? modelData.seriesName : i18n("Unknown series"))
                                Controls.ToolTip.text: text
                                Controls.ToolTip.visible: hovered || visualFocus
                                Controls.ToolTip.delay: Kirigami.Units.toolTipDelay
                                onClicked: page.downloads.removeSeriesRule(modelData.seriesId)
                            }
                        }
                    }
                }
                Repeater {
                    model: page.downloads.outboxConflicts
                    delegate: MessageBox {
                        required property var modelData
                        Layout.fillWidth: true
                        type: Kirigami.MessageType.Warning
                        text: i18n("%1 was played on another device without being marked as watched there. Keep the server’s state or mark it as watched here", modelData.name)
                        actions: [
                            Kirigami.Action {
                                text: i18n("Keep Server State")
                                icon.name: "document-save"
                                onTriggered: page.downloads.resolveOutboxConflict(modelData.serverId, modelData.userId, modelData.itemId, true)
                            },
                            Kirigami.Action {
                                text: i18n("Mark as Watched")
                                icon.name: "emblem-checked"
                                onTriggered: page.downloads.resolveOutboxConflict(modelData.serverId, modelData.userId, modelData.itemId, false)
                            }
                        ]
                    }
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
        // The label starts at the same distance from the left edge as its text does from the top: the content row centres it vertically
        leftPadding: topPadding + footerCount.y
        rightPadding: leftPadding

        contentItem: RowLayout {
            spacing: Kirigami.Units.largeSpacing

            Controls.Label {
                id: footerCount
                objectName: "downloadsFooterCount"
                Layout.fillWidth: true
                textFormat: Text.PlainText
                elide: Text.ElideRight
                text: i18np("%1 download", "%1 downloads", page.downloadsModel.count)
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
