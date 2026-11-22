// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// Watching together through the server's SyncPlay groups: outside a group the groups to join and a new one to create, in a group its name, its members and the way out. Group and member names are server text: plain-text labels only, never an action's text or tooltip
Kirigami.Dialog {
    id: dialog
    objectName: "syncPlayDialogContent"

    required property ConnectionController connection
    required property var syncPlay
    // The signed-in user's name, for the name a new group gets; empty when unknown
    property string userName: ""
    readonly property SyncPlayGroupsModel groups: syncPlay?.groups ?? null
    readonly property bool joined: syncPlay?.status === SyncPlayController.Joined
    readonly property bool joining: syncPlay?.status === SyncPlayController.Joining
    readonly property bool following: syncPlay?.following ?? false
    readonly property SyncPlayQueueModel queue: syncPlay?.queue ?? null
    // The last failure since the dialog opened, -1 for none: a box says it until the next try
    property int failure: -1
    readonly property bool listing: !joined && !joining
    readonly property bool groupsAnswered: groups?.status === SyncPlayGroupsModel.Ready || groups?.status === SyncPlayGroupsModel.Failed

    title: i18nc("@title:window", "SyncPlay")
    preferredWidth: Kirigami.Units.gridUnit * 28
    maximumWidth: Kirigami.Units.gridUnit * 34
    maximumHeight: absoluteMaximumHeight - Kirigami.Units.gridUnit * 4
    customFooterActions: [
        Kirigami.Action {
            objectName: "syncPlayRefreshAction"
            text: i18nc("@action:button", "Refresh")
            icon.name: "view-refresh"
            visible: dialog.listing
            onTriggered: dialog.groups?.reload()
        },
        Kirigami.Action {
            objectName: "syncPlayFollowAction"
            // Not following, the group plays on without waiting for this player
            text: dialog.following ? i18nc("@action:button", "Stop Following") : i18nc("@action:button", "Follow Again")
            icon.name: dialog.following ? "network-disconnect" : "network-connect"
            visible: dialog.joined
            enabled: !(dialog.syncPlay?.offline ?? false)
            onTriggered: dialog.following ? dialog.syncPlay.stopFollowing() : dialog.syncPlay.resumeFollowing()
        },
        Kirigami.Action {
            objectName: "syncPlayLeaveAction"
            // While joining it gives up waiting for the server
            text: dialog.joining ? i18nc("@action:button", "Cancel") : i18nc("@action:button", "Leave Group")
            icon.name: dialog.joining ? "dialog-cancel" : "im-user-offline"
            visible: dialog.joined || dialog.joining
            onTriggered: dialog.syncPlay.leaveGroup()
        }
    ]

    header: DialogTitleBar {
        title: dialog.title
        onCloseRequested: dialog.close()
    }

    // What a failure means for the user: the dialog's box and the shell's notification say the same
    function failureText(failure) {
        switch (failure) {
        case SyncPlayController.CreateDenied:
            return i18n("Missing permission to create SyncPlay groups. Ask the server administrator");
        case SyncPlayController.JoinDenied:
            return i18n("Missing permission to join this SyncPlay group. Ask the server administrator");
        case SyncPlayController.GroupGone:
            return i18n("The SyncPlay group no longer exists. Choose another group or create one");
        case SyncPlayController.LibraryAccessDenied:
            return i18n("Missing access to what the SyncPlay group plays. Ask the server administrator");
        case SyncPlayController.NoAnswer:
            return i18n("The server did not answer. Try again");
        case SyncPlayController.RequestFailed:
            return i18n("The SyncPlay group did not take the request. Try again");
        }
        return "";
    }

    // At most the 128 UTF-16 units a group name may have, never half a character
    function defaultGroupName() {
        let name = userName.length > 0 ? i18nc("@info the name of a new SyncPlay group, %1 user name", "%1’s group", userName) : i18nc("@info the name of a new SyncPlay group", "Watching together");
        if (name.length > 128) {
            const cut = name.charCodeAt(127) >= 0xd800 && name.charCodeAt(127) <= 0xdbff ? 127 : 128;
            name = name.slice(0, cut);
        }
        return name;
    }

    // Whether the keyboard focus is somewhere in the dialog: a row rebuilt under it (a reload, a join) leaves it nowhere
    function focusInside() {
        for (let item = body.Window.window?.activeFocusItem ?? null; item; item = item.parent) {
            if (item === dialog.contentItem || item === body)
                return true;
        }
        return false;
    }

    // The first Join, else the name of a new group, else the footer's first button
    function focusPrimary() {
        if (!dialog.opened || focusInside())
            return;
        let target = null;
        for (let i = 0; dialog.listing && i < rows.count && !target; ++i)
            target = rows.itemAt(i)?.primary ?? null;
        if (!target && newGroup.visible)
            target = nameField;
        (target ?? footer?.nextItemInFocusChain(true))?.forceActiveFocus(Qt.TabFocusReason);
    }

    onOpened: {
        failure = -1;
        nameField.text = defaultGroupName();
        if (listing)
            groups?.reload();
        Qt.callLater(focusPrimary);
    }

    // A session that ended, or SyncPlay no longer available, must not leave the dialog behind
    Connections {
        target: dialog.connection
        function onStateChanged() {
            if (dialog.connection.state !== ConnectionController.Authenticated)
                dialog.close();
        }
    }

    Connections {
        target: dialog.syncPlay
        function onFailed(failure) {
            dialog.failure = failure;
        }
        // The status itself, not the bindings on it, which may not have followed yet
        function onStatusChanged() {
            if (dialog.syncPlay.status !== SyncPlayController.Idle)
                dialog.failure = -1;
            // Back out of a group: the list may have changed meanwhile
            else if (dialog.opened)
                dialog.groups?.reload();
            Qt.callLater(dialog.focusPrimary);
        }
        function onAvailabilityChanged() {
            if (!dialog.syncPlay.available && dialog.syncPlay.status === SyncPlayController.Idle)
                dialog.close();
        }
    }

    Connections {
        target: dialog.groups
        function onCountChanged() {
            Qt.callLater(dialog.focusPrimary);
        }
        function onStatusChanged() {
            Qt.callLater(dialog.focusPrimary);
        }
    }

    QQC2.Control {
        id: body
        padding: Kirigami.Units.largeSpacing
        implicitHeight: Math.max(implicitContentHeight + topPadding + bottomPadding, Kirigami.Units.gridUnit * 14)

        contentItem: ColumnLayout {
            spacing: Kirigami.Units.largeSpacing

            MessageBox {
                objectName: "syncPlayFailure"
                Layout.fillWidth: true
                visible: dialog.failure >= 0
                type: Kirigami.MessageType.Error
                text: dialog.failureText(dialog.failure)
            }

            // Joining: the server's answer comes on the socket
            RowLayout {
                objectName: "syncPlayJoining"
                Layout.alignment: Qt.AlignHCenter
                visible: dialog.joining
                spacing: Kirigami.Units.largeSpacing
                FadeInOnShow {}
                QQC2.BusyIndicator {
                    running: dialog.joining
                }
                QQC2.Label {
                    text: i18n("Joining the group…")
                }
            }

            // In a group
            ColumnLayout {
                objectName: "syncPlayGroup"
                Layout.fillWidth: true
                visible: dialog.joined
                spacing: Kirigami.Units.smallSpacing

                Kirigami.Heading {
                    objectName: "syncPlayGroupName"
                    Layout.fillWidth: true
                    level: 3
                    elide: Text.ElideRight
                    textFormat: Text.PlainText
                    text: (dialog.syncPlay?.groupName ?? "").length > 0 ? dialog.syncPlay.groupName : i18n("Unnamed group")
                }
                QQC2.Label {
                    Layout.fillWidth: true
                    opacity: 0.7
                    text: i18np("%1 member", "%1 members", dialog.syncPlay?.participants.length ?? 0)
                }
                Repeater {
                    model: dialog.joined ? dialog.syncPlay.participants : []
                    delegate: QQC2.Label {
                        required property string modelData
                        objectName: "syncPlayMember"
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                        textFormat: Text.PlainText
                        text: modelData
                    }
                }
                QQC2.Label {
                    objectName: "syncPlayWaiting"
                    Layout.fillWidth: true
                    Layout.topMargin: Kirigami.Units.largeSpacing
                    visible: dialog.following && (dialog.syncPlay?.waiting ?? false)
                    wrapMode: Text.Wrap
                    text: i18n("Waiting for the other members to be ready")
                }
                // One box at a time, the first that applies: a failed request, offline, then not following
                MessageBox {
                    objectName: "syncPlayOffline"
                    Layout.fillWidth: true
                    Layout.topMargin: Kirigami.Units.largeSpacing
                    visible: (dialog.syncPlay?.offline ?? false) && dialog.failure < 0
                    type: Kirigami.MessageType.Warning
                    text: i18n("The connection to the server was lost. The group is joined again once it is back")
                }
                MessageBox {
                    objectName: "syncPlayNotFollowing"
                    Layout.fillWidth: true
                    Layout.topMargin: Kirigami.Units.largeSpacing
                    visible: dialog.joined && !dialog.following && !(dialog.syncPlay?.offline ?? false) && dialog.failure < 0
                    type: Kirigami.MessageType.Information
                    text: i18n("Playback here is independent of the group. Use Follow Again to keep in step with it")
                }

                Kirigami.Heading {
                    Layout.fillWidth: true
                    Layout.topMargin: Kirigami.Units.largeSpacing
                    level: 4
                    text: i18nc("@title the SyncPlay group's queue", "Queue")
                    visible: (dialog.queue?.count ?? 0) > 0
                }
                Repeater {
                    model: dialog.joined ? dialog.queue : null
                    delegate: QQC2.ItemDelegate {
                        id: entryRow
                        objectName: "syncPlayQueueRow"
                        Layout.fillWidth: true
                        required property int index
                        required property string playlistItemId
                        required property string name
                        required property string seriesName
                        required property int seasonNumber
                        required property int episodeNumber
                        required property bool current

                        // An episode under its series, else the item's name; the server's text, plain only
                        readonly property string title: name.length === 0 ? i18n("Unknown item") : seriesName.length > 0 && seasonNumber >= 0 && episodeNumber >= 0 ? i18nc("@info %1 series, %2 season, %3 episode, %4 episode name", "%1 S%2:E%3 — %4", seriesName, seasonNumber, episodeNumber, name) : name
                        readonly property int lastIndex: (dialog.queue?.count ?? 1) - 1

                        highlighted: current
                        // The style's row background reads this hint, not Kirigami.Theme.useAlternateBackgroundColor, and the row's index
                        Kirigami.StyleHints.useAlternateBackgroundColor: true
                        // The row itself does nothing: its buttons do
                        hoverEnabled: false
                        focusPolicy: Qt.NoFocus
                        down: false
                        readonly property bool editable: !(dialog.syncPlay?.offline ?? true)
                        Accessible.name: current ? i18nc("@info %1 the title of what the group plays", "Playing: %1", title) : title

                        contentItem: RowLayout {
                            spacing: Kirigami.Units.smallSpacing
                            Kirigami.Icon {
                                Layout.preferredWidth: Kirigami.Units.iconSizes.small
                                Layout.preferredHeight: Kirigami.Units.iconSizes.small
                                source: "media-playback-start"
                                opacity: entryRow.current ? 1 : 0
                                Accessible.ignored: true
                            }
                            QQC2.Label {
                                objectName: "syncPlayQueueTitle"
                                Layout.fillWidth: true
                                elide: Text.ElideRight
                                textFormat: Text.PlainText
                                text: entryRow.title
                                font.bold: entryRow.current
                            }
                            QQC2.ToolButton {
                                objectName: "syncPlayQueuePlay"
                                // Disabled rather than hidden, so the buttons of the rows stay aligned
                                enabled: !entryRow.current && dialog.following && entryRow.editable
                                icon.name: "media-playback-start"
                                text: i18nc("@action:button", "Play")
                                display: QQC2.AbstractButton.IconOnly
                                Accessible.name: i18nc("@action:button %1 title", "Play %1", entryRow.title)
                                QQC2.ToolTip.text: text
                                QQC2.ToolTip.visible: hovered || visualFocus
                                QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                                onClicked: dialog.syncPlay.playEntry(entryRow.playlistItemId)
                            }
                            // The HIG allows two buttons in a row at most: Play and the menu, as the Downloads rows have
                            QQC2.ToolButton {
                                id: rowMore
                                objectName: "syncPlayQueueMore"
                                enabled: entryRow.editable
                                icon.name: "overflow-menu"
                                text: i18n("More Actions")
                                display: QQC2.AbstractButton.IconOnly
                                Accessible.name: i18nc("@action:button %1 is the item", "More Actions for %1", entryRow.title)
                                QQC2.ToolTip.text: text
                                QQC2.ToolTip.visible: hovered || visualFocus
                                QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                                onClicked: rowMenu.open()

                                // Below the button, above it when there is no room below
                                PopupPlacement { popup: rowMenu; anchor: rowMore }
                                QQC2.Menu {
                                    id: rowMenu
                                    objectName: "syncPlayQueueMenu"
                                    QQC2.MenuItem {
                                        objectName: "syncPlayQueueUp"
                                        text: i18n("Move Up")
                                        icon.name: "arrow-up"
                                        enabled: entryRow.index > 0 && entryRow.editable
                                        onTriggered: dialog.syncPlay.moveEntry(entryRow.playlistItemId, entryRow.index - 1)
                                    }
                                    QQC2.MenuItem {
                                        objectName: "syncPlayQueueDown"
                                        text: i18n("Move Down")
                                        icon.name: "arrow-down"
                                        enabled: entryRow.index < entryRow.lastIndex && entryRow.editable
                                        onTriggered: dialog.syncPlay.moveEntry(entryRow.playlistItemId, entryRow.index + 1)
                                    }
                                    QQC2.MenuItem {
                                        objectName: "syncPlayQueueRemove"
                                        text: i18n("Remove")
                                        icon.name: "edit-delete-remove"
                                        enabled: entryRow.editable
                                        onTriggered: dialog.syncPlay.removeEntry(entryRow.playlistItemId)
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // Not in a group: the groups to join and a new one
            QQC2.BusyIndicator {
                FadeInOnShow {}
                objectName: "syncPlayGroupsBusy"
                Layout.alignment: Qt.AlignHCenter
                running: dialog.listing && !dialog.groupsAnswered
                visible: running
            }

            Kirigami.PlaceholderMessage {
                FadeInOnShow {}
                objectName: "syncPlayNoGroups"
                Layout.fillWidth: true
                visible: dialog.listing && dialog.groups?.status === SyncPlayGroupsModel.Ready && dialog.groups.count === 0
                icon.name: "system-users"
                text: i18n("No groups to join")
                explanation: dialog.syncPlay?.canCreate ? i18n("Create one below, then others can join it from their own apps") : i18n("Ask someone to create a group, then refresh")
            }

            MessageBox {
                objectName: "syncPlayGroupsFailed"
                Layout.fillWidth: true
                visible: dialog.listing && dialog.groups?.status === SyncPlayGroupsModel.Failed && dialog.failure < 0
                type: Kirigami.MessageType.Error
                text: i18n("Could not load the SyncPlay groups")
                actions: [
                    Kirigami.Action {
                        text: i18n("Retry")
                        icon.name: "view-refresh"
                        onTriggered: dialog.groups.reload()
                    }
                ]
            }

            Repeater {
                id: rows
                model: dialog.listing ? dialog.groups : null
                delegate: QQC2.Frame {
                    id: groupFrame
                    objectName: "syncPlayGroupRow"
                    Layout.fillWidth: true
                    required property string groupId
                    required property string name
                    required property var members
                    required property int groupState

                    readonly property string shownName: name.length > 0 ? name : i18n("Unnamed group")
                    readonly property Item primary: joinButton
                    readonly property string stateText: {
                        switch (groupState) {
                        case SyncPlayGroupsModel.Playing:
                            return i18nc("@info a SyncPlay group's state", "Playing");
                        case SyncPlayGroupsModel.Paused:
                            return i18nc("@info a SyncPlay group's state", "Paused");
                        case SyncPlayGroupsModel.Waiting:
                            return i18nc("@info a SyncPlay group's state", "Waiting");
                        }
                        return i18nc("@info a SyncPlay group's state", "Nothing playing");
                    }

                    contentItem: RowLayout {
                        spacing: Kirigami.Units.largeSpacing

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0
                            Kirigami.Heading {
                                objectName: "syncPlayRowName"
                                Layout.fillWidth: true
                                level: 4
                                elide: Text.ElideRight
                                textFormat: Text.PlainText
                                text: groupFrame.shownName
                            }
                            QQC2.Label {
                                objectName: "syncPlayRowMembers"
                                Layout.fillWidth: true
                                elide: Text.ElideRight
                                textFormat: Text.PlainText
                                opacity: 0.7
                                font: Kirigami.Theme.smallFont
                                text: groupFrame.members.join(", ")
                            }
                            QQC2.Label {
                                Layout.fillWidth: true
                                opacity: 0.7
                                font: Kirigami.Theme.smallFont
                                text: groupFrame.stateText
                            }
                        }
                        QQC2.Button {
                            id: joinButton
                            objectName: "syncPlayJoinButton"
                            text: i18nc("@action:button", "Join")
                            Accessible.name: i18nc("@action:button %1 group name", "Join %1", groupFrame.shownName)
                            onClicked: dialog.syncPlay.joinGroup(groupFrame.groupId)
                        }
                    }
                }
            }

            RowLayout {
                id: newGroup
                objectName: "syncPlayNewGroup"
                Layout.fillWidth: true
                Layout.topMargin: Kirigami.Units.largeSpacing
                visible: dialog.listing && (dialog.syncPlay?.canCreate ?? false)
                spacing: Kirigami.Units.smallSpacing

                QQC2.TextField {
                    id: nameField
                    objectName: "syncPlayNameField"
                    Layout.fillWidth: true
                    maximumLength: 128
                    placeholderText: i18n("Enter a group name")
                    Accessible.name: i18n("Name of the new group")
                    onAccepted: createButton.clicked()
                }
                QQC2.Button {
                    id: createButton
                    objectName: "syncPlayCreateButton"
                    text: i18nc("@action:button", "Create Group")
                    icon.name: "group-new"
                    enabled: nameField.text.trim().length > 0
                    onClicked: {
                        if (enabled)
                            dialog.syncPlay.createGroup(nameField.text.trim());
                    }
                }
            }
        }
    }
}
