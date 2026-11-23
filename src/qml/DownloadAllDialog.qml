// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// Download All of a series or a season: what the batch holds, with the choice to leave the watched episodes out. The episodes are read when the dialog opens and nothing is queued before Download
Kirigami.PromptDialog {
    id: dialog
    objectName: "downloadAllDialog"

    required property var downloads
    property string containerId
    // A series covers every season, a season its own episodes
    property bool series: false

    // How many entered the queue, and how many the dialog had counted
    signal queued(int count, int expected)

    readonly property var plan: downloads.downloadAllPlan
    readonly property bool reading: plan.state === "reading"
    readonly property bool ready: plan.state === "ready"
    readonly property bool failed: plan.state === "failed"

    title: i18n("Download All")
    preferredWidth: Kirigami.Units.gridUnit * 26
    standardButtons: Kirigami.Dialog.Cancel
    customFooterActions: [
        Kirigami.Action {
            objectName: "downloadAllConfirm"
            text: i18n("Download")
            icon.name: "download-symbolic"
            enabled: dialog.ready && dialog.plan.count > 0
            onTriggered: {
                const expected = dialog.plan.count;
                const count = dialog.downloads.startDownloadAll();
                dialog.close();
                dialog.queued(count, expected);
            }
        }
    ]

    onAboutToShow: downloads.planDownloadAll(containerId, series)
    onClosed: downloads.cancelDownloadAll()

    Controls.BusyIndicator {
        objectName: "downloadAllReading"
        Layout.alignment: Qt.AlignHCenter
        visible: dialog.reading
        running: visible
        Accessible.name: i18n("Reading the episodes")
    }

    MessageBox {
        objectName: "downloadAllFailed"
        Layout.fillWidth: true
        Layout.topMargin: Kirigami.Units.largeSpacing
        visible: dialog.failed
        type: Kirigami.MessageType.Error
        text: i18n("The episodes could not be read from the server: check the connection and try again")
    }

    Controls.Label {
        objectName: "downloadAllSummary"
        Layout.fillWidth: true
        visible: dialog.ready
        wrapMode: Text.Wrap
        textFormat: Text.PlainText
        text: {
            if (dialog.plan.total === 0)
                return i18n("There are no episodes to download");
            if (dialog.plan.count === 0) {
                if (dialog.plan.watchedSkipped > 0 && dialog.plan.alreadyDownloaded === 0)
                    return i18n("Every episode has been watched: switch off Only unwatched episodes to download them again");
                if (dialog.plan.alreadyDownloaded > 0 && dialog.plan.watchedSkipped === 0)
                    return i18n("Every episode is already on this device");
                if (dialog.plan.alreadyDownloaded + dialog.plan.watchedSkipped > 0)
                    return i18n("Nothing is left to download: the episodes are already on this device or have been watched");
                return i18n("None of the episodes can be downloaded");
            }
            const episodes = i18np("%1 episode will be downloaded", "%1 episodes will be downloaded", dialog.plan.count);
            return dialog.plan.sizeKnown ? i18nc("@info number of episodes, then the size they take together", "%1 (%2 in all)", episodes, dialog.downloads.formattedSize(dialog.plan.size)) : episodes;
        }
    }

    Controls.CheckBox {
        objectName: "downloadAllUnwatchedOnly"
        visible: dialog.ready && dialog.plan.total > 0
        text: i18n("Only unwatched episodes")
        checked: dialog.plan.unwatchedOnly
        onToggled: dialog.downloads.setDownloadAllUnwatchedOnly(checked)
    }

    Controls.Label {
        objectName: "downloadAllTruncated"
        Layout.fillWidth: true
        visible: dialog.ready && dialog.plan.truncated
        wrapMode: Text.Wrap
        textFormat: Text.PlainText
        text: i18np("Only the first episode is included", "Only the first %1 episodes are included", dialog.plan.limit)
    }

    MessageBox {
        objectName: "downloadAllMetered"
        Layout.fillWidth: true
        Layout.topMargin: Kirigami.Units.largeSpacing
        visible: dialog.ready && dialog.plan.count > 0 && dialog.downloads.metered
        type: Kirigami.MessageType.Warning
        text: i18n("This network may charge for data")
    }
}
