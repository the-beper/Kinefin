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
    property int seasonNumber: -1
    property string currentKind: "nextN"
    property int currentCount: 3

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
                                               kindBox.currentValue, dialog.seasonNumber,
                                               countBox.value);
                dialog.close();
            }
        }
    ]

    Controls.ComboBox {
        id: kindBox
        Layout.fillWidth: true
        textRole: "text"
        valueRole: "value"
        model: dialog.seasonNumber > 0
            ? [{ text: i18n("Next episodes"), value: "nextN" },
               { text: i18n("Whole season"), value: "season" },
               { text: i18n("New episodes"), value: "newEpisodes" }]
            : [{ text: i18n("Next episodes"), value: "nextN" },
               { text: i18n("New episodes"), value: "newEpisodes" }]
        Component.onCompleted: {
            const index = model.findIndex(entry => entry.value === dialog.currentKind);
            currentIndex = index >= 0 ? index : 0;
        }
    }
    Controls.SpinBox {
        id: countBox
        Layout.fillWidth: true
        from: 1
        to: 3
        value: dialog.currentCount
        visible: kindBox.currentValue === "nextN"
        Accessible.name: i18n("Episodes")
    }
    Controls.Label {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        textFormat: Text.PlainText
        visible: kindBox.currentValue === "season"
        text: dialog.seasonNumber > 0 ? i18n("Season %1", dialog.seasonNumber) : i18n("The whole season")
    }
}
