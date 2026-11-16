// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// The player docked in the bottom trailing corner of the content area while another page is in front: it floats over the pages and reserves no space, and the shell's video item sits in its slot
Item {
    id: mini
    objectName: "miniPlayer"

    // The shell window: the mode, the player and the actions the controls call
    required property var shell
    readonly property var player: shell.player
    // The video item of the shell sits here while the mini player shows
    readonly property Item videoSlot: slot
    readonly property real margin: Kirigami.Units.largeSpacing
    readonly property bool mirrored: Qt.application.layoutDirection === Qt.RightToLeft
    // A tap on a touch screen shows the controls for a few seconds: a press on a control that is not shown only does that, so a touch never acts on what it cannot see
    property bool touchRevealed: false
    // The controls show under the pointer, after a touch tap and while the keyboard is on one of them
    readonly property bool controlsShown: touchRevealed || hover.hovered || playButton.visualFocus || expandButton.visualFocus || closeButton.visualFocus
    // The page in front may keep a bar at its bottom (the Save bar of Settings): the dock sits above it
    readonly property real clearance: shell.pageStack.currentItem?.miniPlayerClearance ?? 0
    // How much of the item is played, 0 to 1
    readonly property real progress: player && player.durationTicks > 0 ? Math.max(0, Math.min(1, player.positionTicks / player.durationTicks)) : 0

    // Follows the mode at once, only the look fades: the video slot is laid out while the dock fades in, and no input reaches it while it fades out
    readonly property bool shown: shell.playbackMode === "mini"
    enabled: shown
    opacity: shown ? 1 : 0
    visible: shown || opacity > 0
    width: Math.round(Math.min(Math.max(shell.width * 0.35, Kirigami.Units.gridUnit * 12), Kirigami.Units.gridUnit * 20))
    height: Math.round(width * 9 / 16)
    x: mirrored ? margin : parent.width - width - margin
    y: parent.height - height - margin - clearance
    // Over the page stack, under the startup cover; the popups are in the window's overlay above both
    z: 10
    Accessible.role: Accessible.Grouping
    Accessible.name: i18nc("@info accessible name of the docked player", "Mini Player")
    onShownChanged: {
        if (!shown) {
            touchRevealed = false;
            revealTimer.stop();
        }
    }
    Behavior on opacity {
        NumberAnimation { duration: Kirigami.Units.longDuration; easing.type: mini.shown ? Easing.OutCubic : Easing.InCubic }
    }
    // The dock rises into place as it fades in and sinks as it fades out
    transform: Translate {
        y: mini.shown ? 0 : Kirigami.Units.gridUnit
        Behavior on y {
            NumberAnimation { duration: Kirigami.Units.longDuration; easing.type: mini.shown ? Easing.OutCubic : Easing.InCubic }
        }
    }

    // A touch also makes hover at the press itself: for a touch, only controls already shown a moment before the press count as seen
    property double shownAt: 0
    onControlsShownChanged: shownAt = controlsShown ? Date.now() : 0
    function seenBefore(time) {
        return controlsShown && shownAt > 0 && time - shownAt >= 200;
    }

    function reveal() {
        touchRevealed = true;
        revealTimer.restart();
    }
    // A control used while the touch reveal is up keeps it up for its full time again
    function touched() {
        if (touchRevealed)
            revealTimer.restart();
    }
    Timer {
        id: revealTimer
        objectName: "miniPlayerRevealTimer"
        interval: 4000
        onTriggered: mini.touchRevealed = false
    }

    // A button of the controls: pressed while they are not shown it only shows them, whatever the device; otherwise it activates. The state is read at the press, since the pointer hover may change while a button holds the mouse. No tooltip: it would spill over the small player (the text stays the accessible name)
    component MiniButton: OsdButton {
        id: miniButton
        Controls.ToolTip.visible: false
        property bool touchPress: false
        property bool shownAtPress: false
        signal activated
        // Handlers see the press before the button does: it tells a touch from a mouse
        PointHandler {
            acceptedDevices: PointerDevice.TouchScreen
            onActiveChanged: if (active) miniButton.touchPress = true
        }
        onPressed: shownAtPress = !touchPress || mini.seenBefore(Date.now())
        onCanceled: touchPress = false
        onClicked: {
            const shown = shownAtPress;
            shownAtPress = false;
            touchPress = false;
            if (!shown) {
                mini.reveal();
                return;
            }
            mini.touched();
            activated();
        }
    }

    Rectangle {
        anchors.fill: parent
        color: "black"
    }
    Item {
        id: slot
        objectName: "miniPlayerVideoSlot"
        anchors.fill: parent
        // On the video, beside the controls: a press on a button never reaches it, and a tap does not take the keyboard from the page. A click expands the player; a touch tap that finds the controls hidden only shows them
        TapHandler {
            objectName: "miniPlayerTap"
            property bool seenAtPress: false
            onPressedChanged: if (pressed) seenAtPress = mini.seenBefore(Date.now())
            onTapped: (eventPoint, button) => {
                if (eventPoint.device.type === PointerDevice.TouchScreen && !seenAtPress)
                    mini.reveal();
                else
                    mini.shell.expandPlayer();
            }
        }
    }
    HoverHandler {
        id: hover
        objectName: "miniPlayerPointer"
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
    }

    // The controls stay in the focus chain while hidden, so Tab reaches them and shows them
    Item {
        id: scrim
        objectName: "miniPlayerScrim"
        anchors.fill: parent
        opacity: mini.controlsShown ? 1 : 0
        Kirigami.Theme.colorSet: Kirigami.Theme.Complementary
        Kirigami.Theme.inherit: false
        Behavior on opacity {
            NumberAnimation { duration: Kirigami.Units.longDuration; easing.type: Easing.OutCubic }
        }

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: topRow.implicitHeight + Kirigami.Units.largeSpacing
            gradient: Gradient {
                GradientStop { position: 0; color: Qt.alpha(Kirigami.Theme.backgroundColor, 0.8) }
                GradientStop { position: 1; color: Qt.alpha(Kirigami.Theme.backgroundColor, 0) }
            }
        }
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: bottomColumn.implicitHeight + Kirigami.Units.largeSpacing
            gradient: Gradient {
                GradientStop { position: 0; color: Qt.alpha(Kirigami.Theme.backgroundColor, 0) }
                GradientStop { position: 1; color: Qt.alpha(Kirigami.Theme.backgroundColor, 0.85) }
            }
        }

        RowLayout {
            id: topRow
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.leftMargin: Kirigami.Units.smallSpacing
            anchors.rightMargin: Kirigami.Units.smallSpacing
            anchors.topMargin: Kirigami.Units.smallSpacing
            spacing: Kirigami.Units.smallSpacing
            Controls.Label {
                objectName: "miniPlayerTitle"
                Layout.fillWidth: true
                Layout.leftMargin: Kirigami.Units.smallSpacing
                textFormat: Text.PlainText
                elide: Text.ElideRight
                font.bold: true
                color: Kirigami.Theme.textColor
                text: mini.player?.title || i18n("Now Playing")
            }
            MiniButton {
                id: closeButton
                objectName: "miniPlayerCloseButton"
                icon.name: "window-close"
                text: i18nc("@action:button stops playback and closes the mini player", "Close Mini Player")
                onActivated: mini.shell.stopPlayback()
            }
        }

        ColumnLayout {
            id: bottomColumn
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.leftMargin: Kirigami.Units.smallSpacing
            anchors.rightMargin: Kirigami.Units.smallSpacing
            anchors.bottomMargin: Kirigami.Units.smallSpacing
            spacing: Kirigami.Units.smallSpacing
            RowLayout {
                Layout.fillWidth: true
                spacing: Kirigami.Units.smallSpacing
                MiniButton {
                    id: playButton
                    objectName: "miniPlayerPauseButton"
                    text: mini.player?.paused ? i18n("Resume") : i18n("Pause")
                    icon.name: mini.player?.paused ? "media-playback-start" : "media-playback-pause"
                    enabled: !!mini.player?.active && !mini.player.loading
                    onActivated: mini.shell.playerAction("playpause")
                }
                Item { Layout.fillWidth: true }
                MiniButton {
                    id: expandButton
                    objectName: "miniPlayerExpandButton"
                    icon.name: "window-restore-pip"
                    text: i18nc("@action:button", "Expand Player")
                    onActivated: mini.shell.expandPlayer()
                }
            }
            // Read only: seeking belongs to the full player; the bar gives screen readers the value, and the fill is drawn by hand to be thin and to follow the layout direction
            Controls.ProgressBar {
                id: progressBar
                objectName: "miniPlayerProgress"
                Layout.fillWidth: true
                Layout.preferredHeight: Kirigami.Units.smallSpacing / 2
                padding: 0
                from: 0
                to: 1
                value: mini.progress
                Accessible.name: i18n("Playback position")
                background: Rectangle {
                    color: Qt.alpha(Kirigami.Theme.textColor, 0.3)
                }
                contentItem: Item {
                    Rectangle {
                        height: parent.height
                        width: Math.round(parent.width * progressBar.position)
                        x: mini.mirrored ? parent.width - width : 0
                        color: Kirigami.Theme.highlightColor
                    }
                }
            }
        }
    }
}
