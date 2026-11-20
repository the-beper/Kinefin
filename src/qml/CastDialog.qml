// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// Where a video plays: This Computer first, then the DLNA devices a search finds while the dialog is open. One window of one size: the search, the devices and the empty state share the room under This Computer. A pick closes the dialog
Kirigami.Dialog {
    id: dialog
    objectName: "castDialog"

    required property var player
    property string itemId: ""
    property real startTicks: 0

    readonly property var cast: player ? player.cast : null
    readonly property var targets: cast ? cast.targets : null
    readonly property bool searching: cast ? cast.searching : false
    readonly property string searchError: cast ? cast.error : ""
    readonly property string currentTarget: player?.castTargetId ?? ""
    // An empty list means "no devices" only once a search has ended: until then it is still searching
    property bool searchedOnce: false

    // `udn` is "local" for This Computer; `name` is the device's name, empty for This Computer
    signal targetChosen(string udn, string name)

    title: i18nc("@title:window", "Play On")
    // One size in every state: the dialog sizes a list by its rows, and the search, the devices and the empty state must not move it
    preferredWidth: Kirigami.Units.gridUnit * 22
    preferredHeight: Kirigami.Units.gridUnit * 20
    standardButtons: Kirigami.Dialog.Cancel
    padding: 0
    customFooterActions: [
        Kirigami.Action {
            objectName: "castSearchAgain"
            text: i18nc("@action:button", "Search Again")
            icon.name: "view-refresh"
            enabled: !!dialog.cast && !dialog.searching
            onTriggered: dialog.startSearch()
        }
    ]

    // While devices are listed the search goes on beside the buttons; an empty list shows it in its own room
    footerLeadingComponent: RowLayout {
        spacing: Kirigami.Units.smallSpacing
        visible: dialog.searching && list.count > 0
        Controls.BusyIndicator {
            Layout.preferredWidth: Kirigami.Units.iconSizes.smallMedium
            Layout.preferredHeight: Kirigami.Units.iconSizes.smallMedium
            running: parent.visible
        }
        Controls.Label {
            objectName: "castStillSearching"
            text: i18nc("@info:status", "Searching for devices…")
        }
    }

    function startSearch() {
        // Without a cast controller there is nothing to search: the empty state, never an endless search
        if (!cast) {
            searchedOnce = true;
            return;
        }
        searchedOnce = false;
        cast.search();
        // A search that could not start has ended already: its error shows
        if (!cast.searching)
            searchedOnce = true;
    }

    onSearchingChanged: {
        if (!searching)
            searchedOnce = true;
    }
    // Before the first frame: the dialog opens on the search, never on an empty list
    onAboutToShow: startSearch()
    onOpened: {
        // The keys start on the device the video plays on, else on This Computer
        list.currentIndex = targets && currentTarget !== "" ? targets.indexOf(currentTarget) : -1;
        list.forceActiveFocus(Qt.PopupFocusReason);
    }
    onClosed: {
        if (cast)
            cast.stopSearch();
    }

    function choose(udn, name) {
        close();
        targetChosen(udn, name);
    }

    ListView {
        id: list
        objectName: "castTargets"

        implicitWidth: Kirigami.Units.gridUnit * 22
        clip: true
        model: dialog.targets
        currentIndex: -1
        keyNavigationEnabled: true
        activeFocusOnTab: true
        Accessible.role: Accessible.List
        Accessible.name: dialog.title

        // Up from the first device reaches This Computer, the header the list marks with no current row
        Keys.onUpPressed: event => {
            if (list.currentIndex <= 0)
                list.currentIndex = -1;
            else
                event.accepted = false;
        }
        function activateCurrent() {
            if (list.currentItem)
                dialog.choose(list.currentItem.udn, list.currentItem.text);
            else
                dialog.choose("local", "");
        }
        Keys.onReturnPressed: activateCurrent()
        Keys.onEnterPressed: activateCurrent()
        Keys.onSpacePressed: activateCurrent()

        header: ColumnLayout {
            width: list.width
            spacing: 0

            CastTargetRow {
                objectName: "castThisComputer"
                Layout.fillWidth: true
                text: i18nc("@item:inlistbox the local player", "This Computer")
                iconName: "computer"
                current: dialog.currentTarget === ""
                highlighted: list.activeFocus && list.currentIndex < 0
                onClicked: dialog.choose("local", "")
            }
            Kirigami.Separator {
                Layout.fillWidth: true
            }
            // The search could not start on this network
            MessageBox {
                objectName: "castError"
                Layout.fillWidth: true
                Layout.margins: Kirigami.Units.largeSpacing
                visible: dialog.searchError.length > 0
                type: Kirigami.MessageType.Error
                text: dialog.searchError
            }
        }

        delegate: CastTargetRow {
            required property string udn
            required property string name
            required property string detail
            width: ListView.view.width
            text: name
            subtitle: detail
            iconName: "video-television"
            current: dialog.currentTarget === udn
            highlighted: ListView.isCurrentItem && ListView.view.activeFocus
            onClicked: dialog.choose(udn, name)
        }

        // The room under This Computer while nothing is listed: the search, then the empty state, in the same place
        ColumnLayout {
            objectName: "castSearching"
            anchors.centerIn: parent
            anchors.verticalCenterOffset: list.headerItem ? list.headerItem.height / 2 : 0
            width: parent.width - Kirigami.Units.gridUnit * 2
            visible: list.count === 0 && dialog.searchError.length === 0 && (dialog.searching || !dialog.searchedOnce)
            spacing: Kirigami.Units.largeSpacing
            Controls.BusyIndicator {
                Layout.alignment: Qt.AlignHCenter
                running: parent.visible
            }
            Controls.Label {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                text: i18nc("@info:status", "Searching for devices…")
            }
        }

        Kirigami.PlaceholderMessage {
            FadeInOnShow {}
            objectName: "castNoDevices"
            anchors.centerIn: parent
            anchors.verticalCenterOffset: list.headerItem ? list.headerItem.height / 2 : 0
            width: parent.width - Kirigami.Units.gridUnit * 2
            visible: list.count === 0 && dialog.searchError.length === 0 && !dialog.searching && dialog.searchedOnce
            icon.name: "video-television"
            text: i18nc("@info:placeholder", "No devices found")
            explanation: i18nc("@info:placeholder", "TVs and players that accept video over DLNA appear here when they are on this network.")
        }
    }
}
