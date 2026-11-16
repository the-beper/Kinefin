// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// The devices connected to the server, with pause, resume and stop for the ones that are playing and controllable. The model polls while the dialog is open and keeps the rows of the sessions that stay, so a focused button survives a poll
Kirigami.Dialog {
    id: dialog
    objectName: "remoteSessionsDialogContent"

    required property ConnectionController connection
    readonly property RemoteSessionsModel remote: connection.remoteSessions
    // Set while the focus still waits for the first answer, which decides whether a row or the footer takes it
    property bool focusPending: false
    // Set when the focus left what focusPrimary gave it: the dialog never takes it back
    property bool focusMoved: false
    property bool focusing: false
    // The item focusPrimary gave the focus to: the window handing it back after being inactive is no move
    property Item focusGiven: null
    // The last answer since the dialog opened (Idle before the first): the boxes follow it, so a poll that runs does not make them flicker
    property int answer: RemoteSessionsModel.Idle
    readonly property bool hasAnswered: answer !== RemoteSessionsModel.Idle
    // Set by Retry, cleared by the next answer: the error box gives way to the spinner meanwhile, while a poll in flight changes nothing on screen
    property bool retrying: false
    readonly property bool showsEmpty: answer === RemoteSessionsModel.Ready && remote.count === 0

    title: i18nc("@title:window", "Remote Sessions")
    preferredWidth: Kirigami.Units.gridUnit * 30
    maximumWidth: Kirigami.Units.gridUnit * 34
    maximumHeight: absoluteMaximumHeight - Kirigami.Units.gridUnit * 4
    customFooterActions: [
        Kirigami.Action {
            id: refreshAction
            objectName: "remoteRefreshAction"
            text: i18nc("@action:button", "Refresh")
            icon.name: "view-refresh"
            // Always shown, also under the empty placeholder: a footer that came and went would resize the dialog
            onTriggered: dialog.remote.reload()
        }
    ]

    // The first Pause or Resume button of the rows, else the first button of the footer
    function focusPrimary() {
        focusing = true;
        let target = null;
        for (let i = 0; i < rows.count && !target; ++i) {
            const primary = rows.itemAt(i)?.primary;
            if (primary?.visible)
                target = primary;
        }
        (target ?? footer.nextItemInFocusChain(true))?.forceActiveFocus(Qt.TabFocusReason);
        focusGiven = body.Window.window?.activeFocusItem ?? null;
        focusing = false;
    }

    function focusAfterAnswer() {
        if (!focusMoved)
            focusPrimary();
    }

    onOpened: {
        answer = RemoteSessionsModel.Idle;
        retrying = false;
        focusMoved = false;
        remote.open();
        focusPending = remote.status !== RemoteSessionsModel.Ready;
        focusPrimary();
    }
    onClosed: {
        focusPending = false;
        remote.close();
    }

    Connections {
        target: body.Window.window
        function onActiveFocusItemChanged() {
            // A window that loses or regains the focus (alt-tab) is no choice of the user
            const item = body.Window.window?.activeFocusItem;
            if (dialog.opened && !dialog.focusing && body.Window.active && item && item !== dialog.focusGiven)
                dialog.focusMoved = true;
        }
    }

    // A session that ended must not leave the dialog over the server the user left
    Connections {
        target: dialog.connection
        function onStateChanged() {
            if (dialog.connection.state !== ConnectionController.Authenticated)
                dialog.close();
        }
    }

    Connections {
        target: dialog.remote
        function onStatusChanged() {
            const status = dialog.remote.status;
            if (status === RemoteSessionsModel.Ready || status === RemoteSessionsModel.Failed) {
                dialog.answer = status;
                dialog.retrying = false;
            }
            if (dialog.focusPending && status !== RemoteSessionsModel.Loading) {
                dialog.focusPending = false;
                Qt.callLater(dialog.focusAfterAnswer);
            }
        }
    }

    QQC2.Control {
        id: body

        padding: Kirigami.Units.largeSpacing
        // A floor that fits the empty placeholder and the error box, so the dialog opens at the size it keeps when the first answer arrives; only more rows than fit make it taller
        implicitHeight: Math.max(implicitContentHeight + topPadding + bottomPadding, Kirigami.Units.gridUnit * 16)

        contentItem: ColumnLayout {
            spacing: Kirigami.Units.largeSpacing

            QQC2.BusyIndicator {
                FadeInOnShow {}
                objectName: "remoteSessionsBusy"
                Layout.alignment: Qt.AlignHCenter
                running: !dialog.hasAnswered || dialog.retrying
                visible: running
            }

            Kirigami.PlaceholderMessage {
                FadeInOnShow {}
                objectName: "remoteSessionsEmpty"
                Layout.fillWidth: true
                visible: dialog.showsEmpty
                icon.name: "input-tvremote"
                text: i18n("No connected devices found")
            }

            MessageBox {
                objectName: "remoteSessionsFailed"
                Layout.fillWidth: true
                visible: dialog.answer === RemoteSessionsModel.Failed && !dialog.retrying
                type: Kirigami.MessageType.Error
                text: i18n("Could not load connected devices")
                actions: [
                    Kirigami.Action {
                        text: i18n("Retry")
                        icon.name: "view-refresh"
                        onTriggered: {
                            dialog.retrying = true;
                            dialog.remote.reload();
                        }
                    }
                ]
            }

            MessageBox {
                Layout.fillWidth: true
                visible: dialog.remote.commandFailed
                type: Kirigami.MessageType.Error
                text: i18n("The remote command failed. Refresh the devices and try again")
            }

            QQC2.Label {
                Layout.fillWidth: true
                visible: dialog.remote.truncated
                wrapMode: Text.Wrap
                text: i18n("Only the first 100 connected devices are shown")
            }

            Repeater {
                id: rows
                model: dialog.remote
                delegate: QQC2.Frame {
                    id: deviceFrame
                    objectName: "remoteSessionRow"
                    Layout.fillWidth: true
                    required property string sessionId
                    required property string deviceName
                    required property string clientName
                    required property string userName
                    required property string nowPlaying
                    required property bool playing
                    required property bool paused
                    required property bool canControl
                    required property string deviceKind

                    // The name for the screen reader of the buttons: a tooltip never carries it
                    readonly property string shownName: deviceName.length > 0 ? deviceName : i18n("Unknown device")
                    readonly property Item primary: pauseButton
                    readonly property string stateText: paused ? i18nc("@info a session that is paused, %1 title", "Paused: %1", nowPlaying) : playing ? i18n("Playing: %1", nowPlaying) : i18n("Nothing playing")

                    contentItem: RowLayout {
                        spacing: Kirigami.Units.largeSpacing

                        // The kind of device, guessed from its client and name
                        Kirigami.Icon {
                            objectName: "remoteDeviceIcon"
                            Layout.preferredWidth: Kirigami.Units.iconSizes.medium
                            Layout.preferredHeight: Kirigami.Units.iconSizes.medium
                            Layout.alignment: Qt.AlignVCenter
                            source: {
                                switch (deviceFrame.deviceKind) {
                                case "tv":
                                    return "video-television";
                                case "tablet":
                                    return "tablet";
                                case "phone":
                                    return "smartphone";
                                case "browser":
                                    return "internet-web-browser";
                                case "computer":
                                    return "computer";
                                }
                                return "network-server";
                            }
                            Accessible.ignored: true
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0

                            Kirigami.Heading {
                                Layout.fillWidth: true
                                level: 4
                                elide: Text.ElideRight
                                textFormat: Text.PlainText
                                text: deviceFrame.shownName
                            }
                            QQC2.Label {
                                Layout.fillWidth: true
                                elide: Text.ElideRight
                                textFormat: Text.PlainText
                                opacity: 0.7
                                font: Kirigami.Theme.smallFont
                                text: [deviceFrame.clientName, deviceFrame.userName].filter(value => value.length > 0).join(" · ")
                            }
                            // What plays, behind its state's icon; a screen reader hears the state in words
                            RowLayout {
                                Layout.fillWidth: true
                                Layout.topMargin: Kirigami.Units.smallSpacing
                                spacing: Kirigami.Units.smallSpacing

                                Kirigami.Icon {
                                    Layout.preferredWidth: Kirigami.Units.iconSizes.small
                                    Layout.preferredHeight: Kirigami.Units.iconSizes.small
                                    visible: deviceFrame.playing
                                    // The status icons, boxed, not the buttons' action icons beside them
                                    source: deviceFrame.paused ? "media-playback-paused" : "media-playback-playing"
                                    Accessible.ignored: true
                                }
                                QQC2.Label {
                                    objectName: "remoteNowPlaying"
                                    Layout.fillWidth: true
                                    elide: Text.ElideRight
                                    maximumLineCount: 1
                                    textFormat: Text.PlainText
                                    opacity: deviceFrame.playing ? 1 : 0.7
                                    text: deviceFrame.playing ? deviceFrame.nowPlaying : i18n("Nothing playing")
                                    Accessible.name: deviceFrame.stateText
                                }
                            }
                        }

                        // The buttons stay enabled while a poll or a command runs: a disabled button would drop the keyboard focus, and the model refuses a command it cannot take
                        QQC2.ToolButton {
                            id: pauseButton
                            objectName: "remotePauseButton"
                            visible: deviceFrame.canControl
                            text: deviceFrame.paused ? i18nc("@action:button", "Resume") : i18nc("@action:button", "Pause")
                            icon.name: deviceFrame.paused ? "media-playback-start" : "media-playback-pause"
                            display: QQC2.AbstractButton.IconOnly
                            Accessible.name: deviceFrame.paused
                                ? i18nc("@action:button %1 device", "Resume %1", deviceFrame.shownName)
                                : i18nc("@action:button %1 device", "Pause %1", deviceFrame.shownName)
                            QQC2.ToolTip.text: text
                            QQC2.ToolTip.visible: hovered || visualFocus
                            QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                            onClicked: dialog.remote.control(deviceFrame.sessionId, deviceFrame.paused ? "Unpause" : "Pause")
                        }
                        QQC2.ToolButton {
                            objectName: "remoteStopButton"
                            visible: deviceFrame.canControl
                            text: i18nc("@action:button", "Stop")
                            icon.name: "media-playback-stop"
                            display: QQC2.AbstractButton.IconOnly
                            Accessible.name: i18nc("@action:button %1 device", "Stop %1", deviceFrame.shownName)
                            QQC2.ToolTip.text: text
                            QQC2.ToolTip.visible: hovered || visualFocus
                            QQC2.ToolTip.delay: Kirigami.Units.toolTipDelay
                            onClicked: dialog.remote.control(deviceFrame.sessionId, "Stop")
                        }
                    }
                }
            }
        }
    }
}
