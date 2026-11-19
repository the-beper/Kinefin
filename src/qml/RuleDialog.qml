// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// The automatic-download rule of one series: kind, number of episodes and the season it applies to
Kirigami.PromptDialog {
    id: dialog
    objectName: "ruleDialog"

    required property var downloads
    property string seriesId
    property string seriesName
    // The season of the page that opened the dialog, -1 on the series page
    property int seasonNumber: -1
    // The season of the stored rule, kept when a season rule is edited from the series page
    property int storedSeason: -1
    property string currentKind: "nextN"
    property int currentCount: 3

    // The kind the radio buttons show; set from the stored rule each time the dialog opens, since the page's details may still have been loading at creation
    property string selectedKind: "nextN"
    readonly property int ruleSeason: seasonNumber > 0 ? seasonNumber : (currentKind === "season" ? storedSeason : -1)
    readonly property bool seasonAvailable: ruleSeason > 0

    title: i18n("Automatic Downloads")
    preferredWidth: Kirigami.Units.gridUnit * 26
    standardButtons: Kirigami.Dialog.Cancel
    customFooterActions: [
        Kirigami.Action {
            objectName: "ruleDialogSave"
            text: i18n("Save")
            icon.name: "document-save"
            onTriggered: {
                dialog.downloads.setSeriesRule(dialog.seriesId, dialog.seriesName,
                                               dialog.selectedKind,
                                               dialog.selectedKind === "season" ? dialog.ruleSeason : -1,
                                               countBox.value);
                dialog.close();
            }
        }
    ]

    onAboutToShow: {
        selectedKind = currentKind === "season" && !seasonAvailable ? "nextN" : currentKind;
        countBox.value = currentCount;
    }

    Controls.Label {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        textFormat: Text.PlainText
        text: i18n("Matching episodes are downloaded automatically")
    }
    // A click breaks a radio button's own binding: the handler keeps them in step with the kind, also across reopenings
    onSelectedKindChanged: {
        ruleKindNextN.checked = selectedKind === "nextN";
        ruleKindSeason.checked = selectedKind === "season";
        ruleKindNewEpisodes.checked = selectedKind === "newEpisodes";
    }

    Kirigami.FormLayout {
        Layout.fillWidth: true

        Controls.RadioButton {
            id: ruleKindNextN
            objectName: "ruleKindNextN"
            Kirigami.FormData.label: i18nc("@label:radiobuttons", "Download:")
            text: i18n("Next episodes")
            checked: dialog.selectedKind === "nextN"
            onToggled: dialog.selectedKind = "nextN"
        }
        Controls.RadioButton {
            id: ruleKindSeason
            objectName: "ruleKindSeason"
            visible: dialog.seasonAvailable
            text: i18n("Whole season")
            checked: dialog.selectedKind === "season"
            onToggled: dialog.selectedKind = "season"
        }
        Controls.RadioButton {
            id: ruleKindNewEpisodes
            objectName: "ruleKindNewEpisodes"
            text: i18n("New episodes")
            checked: dialog.selectedKind === "newEpisodes"
            onToggled: dialog.selectedKind = "newEpisodes"
        }
        Controls.SpinBox {
            id: countBox
            objectName: "ruleCount"
            Kirigami.FormData.label: i18nc("@label:spinbox", "Episodes:")
            from: 1
            to: 3
            value: dialog.currentCount
            visible: dialog.selectedKind === "nextN"
            Accessible.name: i18n("Episodes")
        }
    }
}
