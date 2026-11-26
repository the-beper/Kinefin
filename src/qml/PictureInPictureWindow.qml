// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

// The player in a small frameless window of its own, over the other windows: the shell's video item sits in its slot while the mode is picture in picture. Closing it stops the playback, and a button at its leading corner gives the video back to the full player, as a browser's picture in picture does. An ApplicationWindow rather than a Window because the tooltips of the buttons need a popup overlay
Controls.ApplicationWindow {
    id: pip
    objectName: "pictureInPictureWindow"

    // The shell window: the mode, the player and the actions the controls call
    required property var shell
    readonly property var player: shell.player
    // The video item of the shell sits here while the window shows
    readonly property Item videoSlot: slot
    // The shape of the window: the video's, kept from the last one known since mpv has no size between two files, 16:9 until one is known; never wider than 3:1 nor taller than 3:4, which the controls need room for
    property real aspect: 16 / 9
    readonly property real videoAspect: player?.videoAspect ?? 0
    function shapeOf(ratio) {
        return Math.max(3 / 4, Math.min(3, ratio));
    }
    onVideoAspectChanged: if (videoAspect > 0) aspect = shapeOf(videoAspect)
    readonly property bool mirrored: Qt.application.layoutDirection === Qt.RightToLeft
    // The strips along the edges that resize the window; the controls keep clear of them
    readonly property int edgeWidth: Kirigami.Units.largeSpacing
    // The side of a button, and the room around it on its panel: the least window at its widest shape is 16 grid units by 5.3, which holds a panel at the top, another at the bottom and the strips along the edges
    readonly property int buttonSize: Math.round(Kirigami.Units.gridUnit * 1.75)
    readonly property int panelPadding: Math.round(Kirigami.Units.smallSpacing / 2)
    readonly property int panelSize: buttonSize + panelPadding * 2
    // The speaker of the full player: muted, low, medium or high
    readonly property string volumeIcon: {
        const volume = player?.volume ?? 100;
        return player?.muted || volume === 0 ? "audio-volume-muted" : volume < 34 ? "audio-volume-low" : volume < 67 ? "audio-volume-medium" : "audio-volume-high";
    }
    // The total time next to the elapsed one only where the timeline keeps room beside them: in a narrower window the time is the elapsed one alone
    readonly property bool totalTimeShown: width >= Kirigami.Units.gridUnit * 20
    // Set by a touch tap that found the controls hidden: they show for a while, as on the mini player
    property bool touchRevealed: false
    // The controls show under the pointer, after a touch tap, while the keyboard is on one of them and while the timeline is dragged
    readonly property bool controlsShown: touchRevealed || hover.hovered || timeline.pressed || backButton.visualFocus || closeButton.visualFocus || playButton.visualFocus || muteButton.visualFocus || timeline.visualFocus
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
    readonly property int minimumShapeWidth: Kirigami.Units.gridUnit * 16
    // With the compositor's surface the least width is the surface's own: a minimum of the window itself would be kept even where it is past the compositor's bounds
    minimumWidth: pipSurface.available ? 0 : pip.minimumShapeWidth
    minimumHeight: pipSurface.available ? 0 : Math.round(pip.minimumShapeWidth / pip.aspect)
    title: pip.player?.title || i18n("Now Playing")

    // Closing the window, from its own X or the window manager, stops the playback, which takes the window with it. A window the shell already gave up closes
    onClosing: close => {
        if (shell.playbackMode !== "pip")
            return;
        close.accepted = false;
        shell.stopPlayback();
    }

    // KWin's picture-in-picture surface, where the compositor offers it: it keeps the window above the others and places, moves and resizes it. The role is given before the window first shows; elsewhere the window stays an ordinary frameless one
    PipSurface {
        id: pipSurface
        aspectRatio: pip.aspect
        minimumWidth: pip.minimumShapeWidth
        // The mini player's shadow has the same values (MiniPlayer.qml)
        shadowSize: Kirigami.Units.gridUnit
        shadowYOffset: Kirigami.Units.smallSpacing
        shadowOpacity: 0.45
    }
    // The size is set once, not bound: after the first configure only the compositor sizes the window, and a size the window set itself past its bounds would end the connection
    Component.onCompleted: {
        if (pip.videoAspect > 0)
            pip.aspect = pip.shapeOf(pip.videoAspect);
        pip.width = Math.round(Math.max(Screen.width * 0.3, pip.minimumShapeWidth));
        pip.height = Math.round(pip.width / pip.aspect);
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

    // A solid rounded panel of the controls, as the full player's: the complementary colours a little see-through and an outline, and a click on it is not a click on the video
    component PipPanel: OsdBox {
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
        }
    }
    component PipButton: OsdButton {
        implicitWidth: pip.buttonSize
        implicitHeight: pip.buttonSize
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
            id: osd
            objectName: "pictureInPictureOsd"
            anchors.fill: parent
            opacity: pip.controlsShown ? 1 : 0
            Kirigami.Theme.colorSet: Kirigami.Theme.Complementary
            Kirigami.Theme.inherit: false
            Behavior on opacity {
                NumberAnimation { duration: Kirigami.Units.longDuration; easing.type: Easing.OutCubic }
            }

            // The way back to the full player at the leading corner and the X at the trailing one, mirrored in right-to-left, each a box that is the button itself
            RowLayout {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: pip.edgeWidth
                layoutDirection: pip.mirrored ? Qt.RightToLeft : Qt.LeftToRight
                PipButton {
                    id: backButton
                    objectName: "pictureInPictureBackButton"
                    Layout.preferredWidth: pip.panelSize
                    Layout.preferredHeight: pip.panelSize
                    icon.name: "window-restore-pip"
                    text: i18nc("@action:button gives the video back to the full player, which plays on", "Back to Player")
                    background: OsdBox { control: backButton }
                    onClicked: pip.returnToPlayer()
                }
                Item { Layout.fillWidth: true }
                PipButton {
                    id: closeButton
                    objectName: "pictureInPictureCloseButton"
                    Layout.preferredWidth: pip.panelSize
                    Layout.preferredHeight: pip.panelSize
                    icon.name: "window-close"
                    text: i18nc("@action:button stops the playback and closes the picture-in-picture window", "Stop Playback and Close")
                    Accessible.name: text
                    background: OsdBox { control: closeButton }
                    onClicked: pip.close()
                }
            }

            // The controls and the timeline on one panel along the bottom
            PipPanel {
                objectName: "pictureInPictureBottomPanel"
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: pip.edgeWidth
                height: pip.panelSize
                RowLayout {
                    id: controls
                    anchors.fill: parent
                    anchors.margins: pip.panelPadding
                    spacing: Kirigami.Units.smallSpacing
                    PipButton {
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
                    // As the speaker button of the full player: a click mutes or unmutes
                    PipButton {
                        id: muteButton
                        objectName: "pictureInPictureMuteButton"
                        text: pip.player?.muted ? i18n("Unmute") : i18n("Mute")
                        icon.name: pip.volumeIcon
                        enabled: !!pip.player?.active
                        onClicked: {
                            pip.touched();
                            pip.shell.playerAction("togglemute");
                        }
                    }
                    // The full player's timeline without its previews: a drag seeks where it is let go, the arrow keys seek as the full player's do
                    Controls.Slider {
                        id: timeline
                        objectName: "pictureInPictureTimeline"
                        Layout.fillWidth: true
                        Layout.minimumWidth: Kirigami.Units.gridUnit * 5
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
                    // The time in the full player's text size, bold on the panel so it reads on any picture
                    Controls.Label {
                        id: timeLabel
                        objectName: "pictureInPictureTime"
                        // As wide as its widest text and never otherwise, so the timeline does not move while the numbers change
                        Layout.preferredWidth: widestTime.implicitWidth
                        Layout.rightMargin: Kirigami.Units.smallSpacing * 2
                        horizontalAlignment: Text.AlignRight
                        textFormat: Text.PlainText
                        color: Kirigami.Theme.textColor
                        font.weight: Font.DemiBold
                        font.features: { "tnum": 1 }
                        readonly property real shownTicks: timeline.pressed ? timeline.dragSeconds * 10000000 : (pip.player?.positionTicks ?? 0)
                        function spelled(elapsed, total) {
                            return pip.totalTimeShown ? i18nc("@info:status elapsed and total time of the video", "%1 / %2", elapsed, total) : elapsed;
                        }
                        text: pip.player ? spelled(pip.player.timeText(timeLabel.shownTicks), pip.player.timeText(pip.player.durationTicks)) : ""
                        Accessible.name: pip.player ? i18nc("@info accessible name of the elapsed and total time", "%1 of %2", pip.player.timeText(timeLabel.shownTicks), pip.player.timeText(pip.player.durationTicks)) : ""
                    }
                    // The time label with its widest text in the shown form, never shown: the total stands for every time, and with tabular digits every digit is as wide as 0
                    Controls.Label {
                        id: widestTime
                        visible: false
                        font: timeLabel.font
                        text: pip.player ? timeLabel.spelled(pip.player.timeText(pip.player.durationTicks), pip.player.timeText(pip.player.durationTicks)).replace(/[0-9]/g, "0") : ""
                    }
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
