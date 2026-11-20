// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// The renderers a video can play on: This Computer first, then the DLNA devices found while the dialog is open. A pick closes the dialog
Kirigami.Dialog {
    id: dialog
    objectName: "castDialog"

    required property var player
    property string itemId: ""
    property real startTicks: 0

    readonly property var cast: player ? player.cast : null
    readonly property var targets: cast ? cast.targets : null
    readonly property bool searching: cast ? cast.searching : false

    // `udn` is "local" for This Computer; `name` is the device's name, empty for This Computer
    signal targetChosen(string udn, string name)

    title: i18nc("@title:window", "Play On")
    preferredWidth: Kirigami.Units.gridUnit * 24
    maximumHeight: absoluteMaximumHeight - Kirigami.Units.gridUnit * 4
    standardButtons: Kirigami.Dialog.Cancel
    padding: 0

    onOpened: {
        if (cast)
            cast.search();
        // The keys start on the device the video plays on, else on This Computer
        const current = targets && player ? targets.indexOf(player.castTargetId) : -1;
        if (current >= 0) {
            list.currentIndex = current;
            list.forceActiveFocus(Qt.PopupFocusReason);
        } else {
            thisComputer.forceActiveFocus(Qt.PopupFocusReason);
        }
    }
    onClosed: {
        if (cast)
            cast.stopSearch();
    }

    function choose(udn, name) {
        close();
        targetChosen(udn, name);
    }

    // A row of the list: a check mark on the one the video plays on now
    component TargetRow: Controls.ItemDelegate {
        rightPadding: Kirigami.Units.iconSizes.small + Kirigami.Units.largeSpacing * 2
        Accessible.checkable: true
        Accessible.checked: checked
        indicator: Kirigami.Icon {
            x: parent.width - width - Kirigami.Units.largeSpacing
            y: (parent.height - height) / 2
            width: Kirigami.Units.iconSizes.small
            height: width
            source: "object-select-symbolic"
            visible: parent.checked
        }
    }

    ColumnLayout {
        spacing: 0

        // Always the first row: while a device plays it is the way back
        TargetRow {
            id: thisComputer
            objectName: "castThisComputer"
            Layout.fillWidth: true
            text: i18nc("@item:inlistbox the local player", "This Computer")
            icon.name: "computer"
            checked: !dialog.player || dialog.player.castTargetId === ""
            onClicked: dialog.choose("local", "")
        }

        Kirigami.Separator {
            Layout.fillWidth: true
        }

        ListView {
            id: list
            objectName: "castTargets"
            Layout.fillWidth: true
            implicitWidth: Kirigami.Units.gridUnit * 24
            implicitHeight: Math.max(contentHeight, Kirigami.Units.gridUnit * 8)
            clip: true
            model: dialog.targets
            keyNavigationEnabled: true
            activeFocusOnTab: true
            Accessible.role: Accessible.List
            Accessible.name: dialog.title

            function activateCurrent() {
                if (list.currentItem)
                    dialog.choose(list.currentItem.udn, list.currentItem.name);
            }
            Keys.onReturnPressed: activateCurrent()
            Keys.onEnterPressed: activateCurrent()

            delegate: TargetRow {
                required property string name
                required property string detail
                required property string udn
                width: list.width
                // The list is the one tab stop, the arrow keys move inside it
                focusPolicy: Qt.NoFocus
                highlighted: ListView.isCurrentItem && ListView.view.activeFocus
                text: name
                checked: !!dialog.player && dialog.player.castTargetId === udn
                Accessible.name: name
                Accessible.description: detail
                onClicked: dialog.choose(udn, name)
                contentItem: ColumnLayout {
                    spacing: 0
                    Controls.Label {
                        Layout.fillWidth: true
                        text: name
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                    }
                    Controls.Label {
                        Layout.fillWidth: true
                        text: detail
                        textFormat: Text.PlainText
                        elide: Text.ElideRight
                        opacity: 0.7
                        font: Kirigami.Theme.smallFont
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.margins: Kirigami.Units.largeSpacing
            visible: dialog.searching
            Controls.BusyIndicator {
                running: dialog.searching
                implicitWidth: Kirigami.Units.iconSizes.small
                implicitHeight: Kirigami.Units.iconSizes.small
            }
            Controls.Label {
                text: i18nc("@info:status", "Searching for devices…")
                textFormat: Text.PlainText
                Layout.fillWidth: true
            }
        }

        // The search could not start on this network
        MessageBox {
            id: castError
            objectName: "castError"
            Layout.fillWidth: true
            Layout.margins: Kirigami.Units.largeSpacing
            visible: text.length > 0
            type: Kirigami.MessageType.Error
            text: dialog.cast ? dialog.cast.error : ""
            actions: Kirigami.Action {
                text: i18nc("@action:button", "Search Again")
                icon.name: "view-refresh"
                onTriggered: dialog.cast?.search()
            }
        }

        Kirigami.PlaceholderMessage {
            Layout.fillWidth: true
            Layout.margins: Kirigami.Units.largeSpacing
            visible: !dialog.searching && list.count === 0 && !castError.visible
            icon.name: "video-television"
            text: i18nc("@info:placeholder", "No devices found")
            explanation: i18nc("@info:placeholder", "TVs and players that accept video over DLNA appear here when they are on this network.")
            helpfulAction: Kirigami.Action {
                text: i18nc("@action:button", "Search Again")
                icon.name: "view-refresh"
                onTriggered: dialog.cast?.search()
            }
        }
    }
}
