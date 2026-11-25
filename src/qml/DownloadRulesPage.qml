// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// The automatic download rules, one per series, above the Downloads tab: switch one off to pause it, change what it downloads, or stop it. A rule only adds episodes that are not watched; what it downloaded is on the Downloads tab
Kirigami.ScrollablePage {
    id: page
    objectName: "downloadRulesPage"

    // The darker View background on every page: a page without a list (a plain flickable) would get the lighter Window one, and the sections would not match
    Kirigami.Theme.colorSet: Kirigami.Theme.View
    Kirigami.Theme.inherit: false

    required property ConnectionController connection

    readonly property DownloadsController downloads: connection.downloads

    title: i18n("Automatic Downloads")
    // As the other pages above a tab: the row of the page carries the way back and the name
    globalToolBarStyle: Kirigami.ApplicationHeaderStyle.None
    Accessible.role: Accessible.Pane
    Accessible.name: title

    function ruleKindText(kind, count, seasonNumber) {
        switch (kind) {
        case "nextN":
            return i18np("Next %1 episode", "Next %1 episodes", count);
        case "season":
            return i18n("Season %1", seasonNumber);
        case "newEpisodes":
            return i18n("New episodes");
        }
        return "";
    }

    // The list takes the keys as soon as the page shows
    function focusPrimary() {
        list.forceActiveFocus(Qt.TabFocusReason);
    }

    // The rows follow the rule store in place, so a switch or an edit keeps the scroll position and the current row. `active` is the rule's enabled state: `enabled` would be the delegate's own property
    function syncRules() {
        const next = downloads.seriesRules();
        for (let i = ruleModel.count - 1; i >= 0; --i) {
            if (!next.some(rule => rule.seriesId === ruleModel.get(i).seriesId))
                ruleModel.remove(i);
        }
        next.forEach((rule, at) => {
            const row = { seriesId: rule.seriesId, seriesName: rule.seriesName, kind: rule.kind, seasonNumber: rule.seasonNumber, count: rule.count, active: rule.enabled };
            if (at < ruleModel.count && ruleModel.get(at).seriesId === rule.seriesId)
                ruleModel.set(at, row);
            else
                ruleModel.insert(at, row);
        });
    }

    // The rule, its name and its paused state, as the row shows them
    function ruleSummary(kind, count, seasonNumber, active) {
        const text = ruleKindText(kind, count, seasonNumber);
        return active ? text : i18nc("@info a rule, then its state", "%1 · %2", text, i18n("Paused"));
    }

    // Stop takes the rule away at once and offers to bring it back as it was, a rule on `newEpisodes` taking up from now on
    function stopRule(row) {
        const rule = { seriesId: row.seriesId, seriesName: row.seriesName, kind: row.kind, seasonNumber: row.seasonNumber, count: row.count, active: row.active };
        if (!downloads.removeSeriesRule(rule.seriesId))
            return;
        Window.window.showPassiveNotification(i18n("Automatic downloads stopped"), "long", i18n("Undo"), () => restoreRule(rule));
        Qt.callLater(() => list.forceActiveFocus(Qt.OtherFocusReason));
    }

    function restoreRule(rule) {
        if (downloads.setSeriesRule(rule.seriesId, rule.seriesName, rule.kind, rule.kind === "season" ? rule.seasonNumber : -1, rule.count) && !rule.active)
            downloads.setSeriesRuleEnabled(rule.seriesId, false);
    }

    function editRule(row) {
        ruleDialog.seriesId = row.seriesId;
        ruleDialog.seriesName = row.seriesName;
        ruleDialog.currentKind = row.kind;
        ruleDialog.storedSeason = row.seasonNumber;
        ruleDialog.currentCount = row.count;
        ruleDialog.paused = !row.active;
        ruleDialog.open();
    }

    Component.onCompleted: syncRules()

    Connections {
        target: page.downloads
        function onRulesRevisionChanged() {
            page.syncRules();
        }
    }

    ListModel {
        id: ruleModel
    }

    header: PageToolbar {
        backShown: true
        title: page.title
        toolbarObjectName: "downloadRulesToolbar"
        titleObjectName: "downloadRulesHeading"
    }

    RuleDialog {
        id: ruleDialog
        downloads: page.downloads
    }

    ListView {
        id: list
        objectName: "downloadRulesList"
        // Room under the bar: the rows scroll up to its line, but open clear of it
        topMargin: Kirigami.Units.largeSpacing * 2

        // On the view, not in its content: the shadow stays under the toolbar while the rows scroll
        TopShadow {
            parent: list
            z: 1
            atStart: list.atYBeginning
        }

        model: ruleModel
        activeFocusOnTab: true
        keyNavigationEnabled: true
        // No row is current until the keyboard picks one: the list must not open with a full highlight
        currentIndex: -1
        Accessible.role: Accessible.List
        Accessible.name: page.title

        Keys.onReturnPressed: {
            if (list.currentIndex >= 0)
                page.editRule(ruleModel.get(list.currentIndex));
        }
        Keys.onEnterPressed: {
            if (list.currentIndex >= 0)
                page.editRule(ruleModel.get(list.currentIndex));
        }
        Keys.onDeletePressed: {
            if (list.currentIndex >= 0)
                page.stopRule(ruleModel.get(list.currentIndex));
        }

        delegate: Controls.ItemDelegate {
            id: row

            required property int index
            required property string seriesId
            required property string seriesName
            required property string kind
            required property int seasonNumber
            required property int count
            required property bool active
            readonly property string shownName: seriesName.length > 0 ? seriesName : i18n("Unknown series")

            objectName: "downloadRule_" + seriesId
            width: list.width
            highlighted: ListView.isCurrentItem && ListView.view.activeFocus && (ListView.view.Window.window?.keyboardDriven ?? true)
            // The style's row background reads this hint, not Kirigami.Theme.useAlternateBackgroundColor, and the row's index
            Kirigami.StyleHints.useAlternateBackgroundColor: true
            hoverEnabled: false
            Accessible.name: row.shownName
            Accessible.description: page.ruleSummary(row.kind, row.count, row.seasonNumber, row.active)

            contentItem: RowLayout {
                spacing: Kirigami.Units.largeSpacing

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: Kirigami.Units.smallSpacing / 2

                    Controls.Label {
                        id: nameLabel
                        Layout.fillWidth: true
                        text: row.shownName
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                        font.weight: Font.DemiBold
                        color: row.highlighted ? Kirigami.Theme.highlightedTextColor : Kirigami.Theme.textColor
                        HoverHandler {
                            id: nameHover
                        }
                        PlainToolTip {
                            visible: nameLabel.truncated && nameHover.hovered
                            text: nameLabel.text
                        }
                    }
                    Controls.Label {
                        Layout.fillWidth: true
                        text: page.ruleSummary(row.kind, row.count, row.seasonNumber, row.active)
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                        color: row.highlighted ? Kirigami.Theme.highlightedTextColor : Kirigami.Theme.disabledTextColor
                        font: Kirigami.Theme.smallFont
                    }
                }
                Controls.Switch {
                    objectName: "downloadRuleSwitch_" + row.seriesId
                    checked: row.active
                    // The keys of the list act on the row whose button has the focus
                    onActiveFocusChanged: if (activeFocus) list.currentIndex = row.index
                    Accessible.name: i18nc("@option:check %1 is the series", "Automatic downloads of %1", row.shownName)
                    onToggled: {
                        page.downloads.setSeriesRuleEnabled(row.seriesId, checked);
                        // A refused write leaves the rule as it was: the switch follows it again
                        checked = Qt.binding(() => row.active);
                    }
                }
                Controls.ToolButton {
                    objectName: "downloadRuleEdit_" + row.seriesId
                    text: i18n("Edit Automatic Downloads…")
                    icon.name: "document-edit"
                    display: Controls.AbstractButton.IconOnly
                    onActiveFocusChanged: if (activeFocus) list.currentIndex = row.index
                    Accessible.name: i18nc("@action:button %1 is the series", "Edit Automatic Downloads of %1", row.shownName)
                    Controls.ToolTip.text: text
                    Controls.ToolTip.visible: hovered || visualFocus
                    Controls.ToolTip.delay: Kirigami.Units.toolTipDelay
                    onClicked: page.editRule(ruleModel.get(row.index))
                }
                Controls.ToolButton {
                    objectName: "downloadRuleStop_" + row.seriesId
                    text: i18n("Stop Automatic Downloads")
                    icon.name: "edit-delete-remove"
                    display: Controls.AbstractButton.IconOnly
                    Accessible.name: i18nc("@action:button %1 is the series", "Stop Automatic Downloads of %1", row.shownName)
                    Controls.ToolTip.text: text
                    Controls.ToolTip.visible: hovered || visualFocus
                    Controls.ToolTip.delay: Kirigami.Units.toolTipDelay
                    onActiveFocusChanged: if (activeFocus) list.currentIndex = row.index
                    onClicked: page.stopRule(row)
                }
            }
        }

        Kirigami.PlaceholderMessage {
            FadeInOnShow {}
            objectName: "downloadRulesEmpty"
            anchors.centerIn: parent
            width: parent.width - Kirigami.Units.gridUnit * 4
            visible: ruleModel.count === 0
            icon.name: "download-later-symbolic"
            text: i18n("No series with automatic downloads")
            explanation: i18n("Choose Download, then Automatic Downloads… on a series or a season")
        }
    }
}
