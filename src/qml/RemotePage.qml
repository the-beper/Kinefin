// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as QQC2
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Kirigami.ScrollablePage {
    id: page
    objectName: "remotePage"
    required property ConnectionController connection
    readonly property RemoteSessionsModel remote: connection.remoteSessions

    title: i18nc("@title:window", "Remote")

    Component.onCompleted: remote.open()
    Component.onDestruction: remote.close()

    function focusPrimary() {
        refreshButton.forceActiveFocus(Qt.TabFocusReason);
    }

    ColumnLayout {
        spacing: Kirigami.Units.largeSpacing

        RowLayout {
            Layout.fillWidth: true

            QQC2.Label {
                Layout.fillWidth: true
                text: i18n("Devices connected to this server")
                wrapMode: Text.Wrap
            }
            QQC2.Button {
                id: refreshButton
                objectName: "remoteRefreshButton"
                text: i18nc("@action:button", "Refresh")
                icon.name: "view-refresh"
                enabled: page.remote.status !== RemoteSessionsModel.Loading
                onClicked: page.remote.reload()
            }
        }

        QQC2.BusyIndicator {
            Layout.alignment: Qt.AlignHCenter
            running: page.remote.status === RemoteSessionsModel.Loading
            visible: running
        }

        Kirigami.PlaceholderMessage {
            Layout.fillWidth: true
            visible: page.remote.status === RemoteSessionsModel.Ready && page.remote.count === 0
            text: i18n("No connected devices found")
            icon.name: "network-disconnect"
        }

        Kirigami.PlaceholderMessage {
            Layout.fillWidth: true
            visible: page.remote.status === RemoteSessionsModel.Failed
            text: i18n("Could not load connected devices. Try again")
            icon.name: "dialog-error"
        }

        QQC2.Label {
            Layout.fillWidth: true
            visible: page.remote.truncated
            wrapMode: Text.Wrap
            text: i18n("Only the first 100 connected devices are shown")
        }

        QQC2.Label {
            Layout.fillWidth: true
            visible: page.remote.commandFailed
            color: Kirigami.Theme.negativeTextColor
            wrapMode: Text.Wrap
            text: i18n("The remote command failed. Refresh the devices and try again")
        }

        Repeater {
            model: page.remote
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

                contentItem: ColumnLayout {
                    spacing: Kirigami.Units.smallSpacing

                    QQC2.Label {
                        Layout.fillWidth: true
                        font.bold: true
                        elide: Text.ElideRight
                        textFormat: Text.PlainText
                        text: deviceFrame.deviceName.length > 0 ? deviceFrame.deviceName : i18n("Unknown device")
                    }
                    QQC2.Label {
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                        textFormat: Text.PlainText
                        opacity: 0.7
                        text: [deviceFrame.clientName, deviceFrame.userName].filter(value => value.length > 0).join(" · ")
                    }
                    QQC2.Label {
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        textFormat: Text.PlainText
                        text: deviceFrame.playing ? i18n("Playing: %1", deviceFrame.nowPlaying) : i18n("Nothing playing")
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        visible: deviceFrame.canControl

                        QQC2.Button {
                            objectName: "remotePauseButton"
                            text: deviceFrame.paused ? i18nc("@action:button", "Resume") : i18nc("@action:button", "Pause")
                            icon.name: deviceFrame.paused ? "media-playback-start" : "media-playback-pause"
                            enabled: page.remote.status === RemoteSessionsModel.Ready && !page.remote.commandPending
                            onClicked: page.remote.control(deviceFrame.sessionId, deviceFrame.paused ? "Unpause" : "Pause")
                        }
                        QQC2.Button {
                            objectName: "remoteStopButton"
                            text: i18nc("@action:button", "Stop")
                            icon.name: "media-playback-stop"
                            enabled: page.remote.status === RemoteSessionsModel.Ready && !page.remote.commandPending
                            onClicked: page.remote.control(deviceFrame.sessionId, "Stop")
                        }
                    }
                }
            }
        }
    }
}
