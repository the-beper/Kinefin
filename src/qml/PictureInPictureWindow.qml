// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// The player in a small frameless window of its own, over the other windows: the shell's video item sits in its slot while the mode is picture in picture. Closing it gives the video back to the full player, as a browser's picture in picture does. An ApplicationWindow rather than a Window because the tooltips of the buttons need a popup overlay
Controls.ApplicationWindow {
    id: pip
    objectName: "pictureInPictureWindow"

    // The shell window: the mode, the player and the actions the controls call
    required property var shell
    readonly property var player: shell.player
    // The video item of the shell sits here while the window shows
    readonly property Item videoSlot: slot
    readonly property bool mirrored: Qt.application.layoutDirection === Qt.RightToLeft
    // The strips along the edges that resize the window; the controls keep clear of them
    readonly property int edgeWidth: Kirigami.Units.largeSpacing
    // Set by a touch tap that found the controls hidden: they show for a while, as on the mini player
    property bool touchRevealed: false
    // The controls show under the pointer, after a touch tap, while the keyboard is on one of them and while the timeline is dragged
    readonly property bool controlsShown: touchRevealed || hover.hovered || timeline.pressed || playButton.visualFocus || timeline.visualFocus || closeButton.visualFocus
    function reveal() {
        touchRevealed = true;
        revealTimer.restart();
    }
    // A control used while the touch reveal is up keeps it up for its full time again
    function touched() {
        if (touchRevealed)
            revealTimer.restart();
    }
    // Back to the full player, which takes the video and plays on; when it cannot (no session left to show it in) the playback ends with the window
    function returnToPlayer() {
        shell.expandPlayer();
        if (shell.playbackMode === "pip")
            shell.stopPlayback();
    }
    function seekRelative(seconds) {
        if (player?.canSeek)
            player.seekRelative(seconds);
    }

    // Not transient, or minimising the main window would take this one with it
    transientParent: null
    // No frame: the window moves by a drag on the video and resizes from its edges. Staying on top is a no-op on Wayland, honoured on X11
    flags: Qt.Window | Qt.FramelessWindowHint | Qt.WindowStaysOnTopHint
    // Shown once the picture-in-picture role is settled
    visible: false
    color: "black"
    minimumWidth: Kirigami.Units.gridUnit * 16
    minimumHeight: Math.round(pip.minimumWidth * 9 / 16)
    title: pip.player?.title || i18n("Now Playing")

    // Closing the window, from its own button or the window manager, returns to the full player: the playback goes on there. A window the shell already gave up closes
    onClosing: close => {
        if (shell.playbackMode !== "pip")
            return;
        close.accepted = false;
        returnToPlayer();
    }

    // KWin's picture-in-picture surface, where the compositor offers it: it keeps the window above the others and places, moves and resizes it. The role is given before the window first shows; elsewhere the window stays an ordinary frameless one
    PipSurface {
        id: pipSurface
    }
    // The size is set once, not bound: after the first configure only the compositor sizes the window, and a size the window set itself past its bounds would end the connection
    Component.onCompleted: {
        pip.width = Math.round(Math.max(Screen.width * 0.3, pip.minimumWidth));
        pip.height = Math.round(pip.width * 9 / 16);
        pipSurface.assign(pip);
        pip.visible = true;
    }

    Shortcut {
        sequences: ["Ctrl+Shift+P"]
        onActivated: pip.returnToPlayer()
    }
    // Quit works from here as from the main window, which stops the playback on its way
    Shortcut {
        sequences: [StandardKey.Quit]
        onActivated: pip.shell.close()
    }
    Timer {
        id: revealTimer
        objectName: "pictureInPictureRevealTimer"
        interval: 4000
        onTriggered: pip.touchRevealed = false
    }

    // A strip along an edge or a corner that starts the window manager's resize, as the frame would
    component ResizeArea: MouseArea {
        id: area
        required property int edges
        readonly property bool diagonal: (area.edges & (Qt.LeftEdge | Qt.RightEdge)) && (area.edges & (Qt.TopEdge | Qt.BottomEdge))
        width: pip.edgeWidth
        height: pip.edgeWidth
        hoverEnabled: true
        cursorShape: !area.diagonal ? ((area.edges & (Qt.LeftEdge | Qt.RightEdge)) ? Qt.SizeHorCursor : Qt.SizeVerCursor)
                     : (area.edges === (Qt.TopEdge | Qt.LeftEdge) || area.edges === (Qt.BottomEdge | Qt.RightEdge)) ? Qt.SizeFDiagCursor : Qt.SizeBDiagCursor
        onPressed: pip.startSystemResize(area.edges)
    }

    Item {
        anchors.fill: parent
        focus: true
        // The full player's keys: Space pauses, the arrows seek
        Keys.onSpacePressed: pip.shell.playerAction("playpause")
        Keys.onLeftPressed: pip.seekRelative(-10)
        Keys.onRightPressed: pip.seekRelative(10)
        Keys.onUpPressed: pip.seekRelative(60)
        Keys.onDownPressed: pip.seekRelative(-60)

        Item {
            id: slot
            objectName: "pictureInPictureVideoSlot"
            anchors.fill: parent
            // As in the full player: a click pauses or resumes after a short wait, so a double click, which returns to the full player, never pauses; a slower double click puts the pause back as the click before it changed it
            TapHandler {
                id: videoTap
                objectName: "pictureInPictureTap"
                // Whether the last click of this run paused or resumed: the double click undoes it
                property bool toggledPause: false
                onTapped: (eventPoint, button) => {
                    // A touch tap only shows the controls, as on the mini player
                    if (eventPoint.device.type === PointerDevice.TouchScreen) {
                        pip.reveal();
                        return;
                    }
                    // Only the first click of a run pauses: the second is a double click
                    if (tapCount > 1)
                        return;
                    videoTap.toggledPause = false;
                    if (pip.player?.active && !pip.player.loading)
                        clickPauseTimer.restart();
                }
                onDoubleTapped: (eventPoint, button) => {
                    if (eventPoint.device.type === PointerDevice.TouchScreen)
                        return;
                    if (clickPauseTimer.running) {
                        clickPauseTimer.stop();
                    } else if (videoTap.toggledPause) {
                        pip.shell.playerAction("playpause");
                        videoTap.toggledPause = false;
                    }
                    pip.returnToPlayer();
                }
            }
            // A drag on the video moves the window; a click stays a click
            DragHandler {
                objectName: "pictureInPictureMove"
                target: null
                onActiveChanged: {
                    if (active) {
                        clickPauseTimer.stop();
                        pip.startSystemMove();
                    }
                }
            }
            // The full player's wait: shorter than Plasma's double-click interval (400 ms), long enough for a usual double click
            Timer {
                id: clickPauseTimer
                objectName: "pictureInPictureClickPauseTimer"
                interval: 250
                onTriggered: {
                    if (!pip.player?.active || pip.player.loading)
                        return;
                    pip.shell.playerAction("playpause");
                    videoTap.toggledPause = true;
                }
            }
        }
        HoverHandler {
            id: hover
            objectName: "pictureInPicturePointer"
            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        }

        // The controls stay in the focus chain while hidden, so Tab reaches them and shows them
        Item {
            id: scrim
            objectName: "pictureInPictureScrim"
            anchors.fill: parent
            opacity: pip.controlsShown ? 1 : 0
            Kirigami.Theme.colorSet: Kirigami.Theme.Complementary
            Kirigami.Theme.inherit: false
            Behavior on opacity {
                NumberAnimation { duration: Kirigami.Units.longDuration; easing.type: Easing.OutCubic }
            }

            // Taller than what it carries, so the controls sit on the dark part over a bright picture
            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                height: closeButton.height + pip.edgeWidth * 2 + Kirigami.Units.gridUnit
                gradient: Gradient {
                    GradientStop { position: 0; color: Qt.alpha(Kirigami.Theme.backgroundColor, 0.8) }
                    GradientStop { position: 1; color: Qt.alpha(Kirigami.Theme.backgroundColor, 0) }
                }
            }
            OsdButton {
                id: closeButton
                objectName: "pictureInPictureCloseButton"
                // The trailing corner, mirrored in right-to-left
                x: pip.mirrored ? pip.edgeWidth : parent.width - width - pip.edgeWidth
                y: pip.edgeWidth
                icon.name: "window-close"
                text: i18nc("@action:button closes the picture-in-picture window; the video goes back to the full player", "Close Picture-in-Picture")
                onClicked: pip.returnToPlayer()
            }

            Rectangle {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: controls.implicitHeight + pip.edgeWidth * 2 + Kirigami.Units.gridUnit
                gradient: Gradient {
                    GradientStop { position: 0; color: Qt.alpha(Kirigami.Theme.backgroundColor, 0) }
                    GradientStop { position: 1; color: Qt.alpha(Kirigami.Theme.backgroundColor, 0.85) }
                }
            }
            RowLayout {
                id: controls
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: pip.edgeWidth
                spacing: Kirigami.Units.smallSpacing
                OsdButton {
                    id: playButton
                    objectName: "pictureInPicturePauseButton"
                    text: pip.player?.paused ? i18n("Resume") : i18n("Pause")
                    icon.name: pip.player?.paused ? "media-playback-start" : "media-playback-pause"
                    enabled: !!pip.player?.active && !pip.player.loading
                    onClicked: {
                        pip.touched();
                        pip.shell.playerAction("playpause");
                    }
                }
                // The full player's timeline without its previews: a drag seeks where it is let go, the arrow keys seek as the full player's do
                Controls.Slider {
                    id: timeline
                    objectName: "pictureInPictureTimeline"
                    Layout.fillWidth: true
                    focusPolicy: Qt.TabFocus
                    enabled: !!pip.player?.canSeek && pip.player.durationTicks > 0
                    from: 0
                    to: Math.max(1, (pip.player?.durationTicks ?? 0) / 10000000)
                    property real dragSeconds: 0
                    value: pressed ? dragSeconds : (pip.player?.positionTicks ?? 0) / 10000000
                    Keys.onLeftPressed: pip.seekRelative(-10)
                    Keys.onRightPressed: pip.seekRelative(10)
                    Keys.onUpPressed: pip.seekRelative(60)
                    Keys.onDownPressed: pip.seekRelative(-60)
                    onMoved: {
                        pip.touched();
                        if (!pip.player)
                            return;
                        if (!timeline.pressed) {
                            pip.player.seek(Math.round(timeline.value * 10000000));
                            return;
                        }
                        timeline.dragSeconds = timeline.value;
                    }
                    onPressedChanged: {
                        if (!pip.player)
                            return;
                        if (timeline.pressed)
                            timeline.dragSeconds = pip.player.positionTicks / 10000000;
                        else
                            pip.player.seek(Math.round(timeline.dragSeconds * 10000000));
                    }
                    Accessible.name: i18n("Playback position")
                }
                Controls.Label {
                    id: timeLabel
                    objectName: "pictureInPictureTime"
                    // As wide as its widest text and never otherwise, so the timeline does not move while the numbers change
                    Layout.preferredWidth: widestTime.implicitWidth
                    horizontalAlignment: Text.AlignRight
                    textFormat: Text.PlainText
                    font.pointSize: Kirigami.Theme.smallFont.pointSize
                    font.features: { "tnum": 1 }
                    readonly property real shownTicks: timeline.pressed ? timeline.dragSeconds * 10000000 : (pip.player?.positionTicks ?? 0)
                    text: pip.player ? i18nc("@info:status elapsed and total time of the video", "%1 / %2", pip.player.timeText(timeLabel.shownTicks), pip.player.timeText(pip.player.durationTicks)) : ""
                    Accessible.name: pip.player ? i18nc("@info accessible name of the elapsed and total time", "%1 of %2", pip.player.timeText(timeLabel.shownTicks), pip.player.timeText(pip.player.durationTicks)) : ""
                }
                // The time label with its widest text, never shown: the total stands for every time, and with tabular digits every digit is as wide as 0
                Controls.Label {
                    id: widestTime
                    visible: false
                    font: timeLabel.font
                    text: pip.player ? i18nc("@info:status elapsed and total time of the video", "%1 / %2", pip.player.timeText(pip.player.durationTicks), pip.player.timeText(pip.player.durationTicks)).replace(/[0-9]/g, "0") : ""
                }
            }
        }

        // While the controls are hidden, a touch only shows them: nothing under it acts unseen (a touch drag moves the window once they show)
        Item {
            objectName: "pictureInPictureTouchGuard"
            anchors.fill: parent
            visible: !pip.controlsShown
            TapHandler {
                acceptedDevices: PointerDevice.TouchScreen
                gesturePolicy: TapHandler.WithinBounds
                onTapped: pip.reveal()
            }
        }

        ResizeArea { edges: Qt.LeftEdge; anchors.left: parent.left; anchors.top: parent.top; anchors.bottom: parent.bottom }
        ResizeArea { edges: Qt.RightEdge; anchors.right: parent.right; anchors.top: parent.top; anchors.bottom: parent.bottom }
        ResizeArea { edges: Qt.TopEdge; anchors.top: parent.top; anchors.left: parent.left; anchors.right: parent.right }
        ResizeArea { edges: Qt.BottomEdge; anchors.bottom: parent.bottom; anchors.left: parent.left; anchors.right: parent.right }
        ResizeArea { edges: Qt.TopEdge | Qt.LeftEdge; anchors.top: parent.top; anchors.left: parent.left }
        ResizeArea { edges: Qt.TopEdge | Qt.RightEdge; anchors.top: parent.top; anchors.right: parent.right }
        ResizeArea { edges: Qt.BottomEdge | Qt.LeftEdge; anchors.bottom: parent.bottom; anchors.left: parent.left }
        ResizeArea { edges: Qt.BottomEdge | Qt.RightEdge; anchors.bottom: parent.bottom; anchors.right: parent.right }
    }
}
