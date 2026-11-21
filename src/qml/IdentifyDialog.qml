// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// Identify: search the metadata providers of a movie or a series and apply one match
Kirigami.Dialog {
    id: dialog
    objectName: "identifyDialog"

    required property ConnectionController connection
    required property ItemDetailsModel details
    // The window shows the notifications
    required property var shell

    property var results: []
    property bool searching: false
    property string problem: ""

    title: i18n("Identify")
    preferredWidth: Kirigami.Units.gridUnit * 30 - Kirigami.Units.largeSpacing * 2
    maximumHeight: absoluteMaximumHeight - Kirigami.Units.gridUnit * 4
    standardButtons: Kirigami.Dialog.Cancel
    padding: Kirigami.Units.largeSpacing

    function search() {
        if (searching)
            return;
        searching = true;
        problem = "";
        results = [];
        if (!connection.remoteSearch(details.itemId, details.type, nameField.text, yearField.value)) {
            searching = false;
            problem = i18n("The search could not be started: wait for the previous one or check the connection");
        }
    }

    function applyResult(index) {
        if (searching || index < 0 || index >= results.length)
            return;
        if (!connection.applyRemoteSearch(details.itemId, results[index]))
            shell.showPassiveNotification(i18n("The match could not be applied: wait for the previous change or check the connection"));
    }

    onOpened: {
        nameField.text = details.name;
        yearField.value = details.year;
        search();
    }

    Connections {
        target: dialog.connection
        function onRemoteSearchFinished(itemId, results) {
            if (!dialog.sameItem(itemId))
                return;
            dialog.searching = false;
            dialog.results = results;
            list.currentIndex = results.length > 0 ? 0 : -1;
            if (results.length > 0)
                list.forceActiveFocus(Qt.PopupFocusReason);
            dialog.problem = results.length === 0 ? i18n("No matches") : "";
        }
        function onRemoteSearchFailed(itemId, status) {
            if (!dialog.sameItem(itemId))
                return;
            dialog.searching = false;
            dialog.problem = i18n("The search failed: check the server's metadata providers and the connection");
        }
        function onRemoteSearchApplied(itemId) {
            if (!dialog.sameItem(itemId))
                return;
            dialog.details.reload();
            dialog.close();
            dialog.shell.showPassiveNotification(i18n("Metadata updated from the selected match"));
        }
        function onRemoteSearchApplyFailed(itemId, status) {
            if (!dialog.sameItem(itemId))
                return;
            dialog.shell.showPassiveNotification(i18n("The match could not be applied: check the connection to the server and try again"));
        }
    }

    function sameItem(itemId) {
        return itemId.toLowerCase().replace(/-/g, "") === dialog.details.itemId.toLowerCase().replace(/-/g, "");
    }

    ColumnLayout {
        Layout.fillWidth: true

        Kirigami.FormLayout {
            Layout.fillWidth: true

            Controls.TextField {
                id: nameField
                objectName: "identifyName"
                Kirigami.FormData.label: i18nc("@label:textbox", "Name:")
                Layout.fillWidth: true
                maximumLength: 512
                Accessible.name: i18n("Name")
                onAccepted: dialog.search()
            }
            Controls.SpinBox {
                id: yearField
                objectName: "identifyYear"
                Kirigami.FormData.label: i18nc("@label:spinbox", "Year:")
                from: 0
                to: 9999
                Accessible.name: i18n("Year")
            }
        }

        Controls.Button {
            objectName: "identifySearch"
            Layout.alignment: Qt.AlignRight
            text: i18nc("@action:button", "Search")
            icon.name: "edit-find"
            enabled: !dialog.searching && nameField.text.trim().length > 0
            onClicked: dialog.search()
        }

        Controls.Label {
            Layout.fillWidth: true
            visible: dialog.problem.length > 0
            wrapMode: Text.Wrap
            textFormat: Text.PlainText
            text: dialog.problem
        }

        Controls.BusyIndicator {
            Layout.alignment: Qt.AlignHCenter
            running: dialog.searching
            visible: dialog.searching
        }

        ListView {
            id: list
            objectName: "identifyResults"
            Layout.fillWidth: true
            // As tall as the matches, within a bound: a short search leaves no empty panel, a long one scrolls
            Layout.preferredHeight: Math.min(contentHeight, Kirigami.Units.gridUnit * 12)
            visible: list.count > 0
            clip: true
            model: dialog.results
            keyNavigationEnabled: true
            activeFocusOnTab: true
            Accessible.role: Accessible.List
            Accessible.name: i18n("Matches")
            Keys.onReturnPressed: dialog.applyResult(list.currentIndex)
            Keys.onEnterPressed: dialog.applyResult(list.currentIndex)
            Controls.ScrollBar.vertical: Controls.ScrollBar {
                policy: Controls.ScrollBar.AsNeeded
            }

            delegate: Controls.ItemDelegate {
                id: row

                required property int index
                required property var modelData

                objectName: "identifyRow_" + index
                width: ListView.view.width
                focusPolicy: Qt.NoFocus
                highlighted: ListView.isCurrentItem && ListView.view.activeFocus
                Accessible.name: row.modelData.name
                Controls.ToolTip.visible: hovered && resultName.truncated
                Controls.ToolTip.text: row.modelData.name
                Controls.ToolTip.delay: Kirigami.Units.toolTipDelay
                onClicked: dialog.applyResult(index)

                contentItem: RowLayout {
                    spacing: Kirigami.Units.largeSpacing

                    // The provider's artwork, when the server gave an https URL on a known provider domain
                    Rectangle {
                        Layout.preferredWidth: Kirigami.Units.gridUnit * 3
                        Layout.preferredHeight: Kirigami.Units.gridUnit * 4.5
                        color: Kirigami.Theme.alternateBackgroundColor
                        radius: Kirigami.Units.smallSpacing
                        border.width: 1
                        border.color: Qt.alpha(Kirigami.Theme.textColor, 0.2)
                        clip: true

                        Image {
                            id: resultPoster
                            anchors.fill: parent
                            asynchronous: true
                            fillMode: Image.PreserveAspectCrop
                            sourceSize.width: width * Screen.devicePixelRatio
                            source: row.modelData.imageUrl
                        }
                        Kirigami.Icon {
                            anchors.centerIn: parent
                            visible: resultPoster.status !== Image.Ready
                            width: Kirigami.Units.iconSizes.large
                            height: width
                            source: "video-x-generic"
                            opacity: 0.7
                        }
                    }

                    ColumnLayout {
                        spacing: 0

                        Controls.Label {
                            id: resultName
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                            maximumLineCount: 1
                            textFormat: Text.PlainText
                            text: row.modelData.name
                        }
                        Controls.Label {
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                            maximumLineCount: 1
                            opacity: 0.7
                            textFormat: Text.PlainText
                            text: {
                                const parts = [];
                                if (row.modelData.year > 0)
                                    parts.push(String(row.modelData.year));
                                if (row.modelData.provider.length > 0)
                                    parts.push(row.modelData.provider);
                                return parts.join(" · ");
                            }
                        }
                    }
                }
            }
        }
    }
}
