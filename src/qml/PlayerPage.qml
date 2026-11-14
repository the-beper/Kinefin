// SPDX-FileCopyrightText: 2026 Kinefin contributors
// SPDX-License-Identifier: GPL-3.0-or-later
import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami

Kirigami.Page {
    id: page
    objectName: "playerPage"
    required property var player
    required property ConnectionController connection
    title: player?.title || i18n("Now Playing")
    padding: 0
    // The video fills the page and the bars float above it: the page row's toolbar would be a strip that never hides
    globalToolBarStyle: Kirigami.ApplicationHeaderStyle.None
    readonly property bool isShowing: page.Window.window?.pageStack.currentItem === page
    property bool showChrome: true

    // The page keeps the keyboard: the controls of the bars take it only through Tab, so the keys work whatever was clicked
    function focusPrimary() { page.forceActiveFocus(Qt.TabFocusReason); }
    function wakeChrome() { showChrome = true; chromeTimer.restart(); }
    function closePlayer() { page.player.stop(); page.Window.window.pageStack.pop(); }
    function focusIsInOsd() {
        for (let item = page.Window.window?.activeFocusItem; item; item = item.parent) {
            if (item === osd)
                return true;
        }
        return false;
    }

    // Esc, Alt+Left and the mouse back button close the queue panel first, then leave a full screen the player entered, and only then leave the player
    onBackRequested: event => {
        if (queuePanel.opened) {
            queuePanel.close();
            event.accepted = true;
        } else if (page.Window.window.playerFullscreen) {
            page.Window.window.togglePlayerFullscreen();
            page.wakeChrome();
            event.accepted = true;
        }
    }
    Keys.onPressed: event => {
        // Tab goes on to the focus chain, through the bars it shows first
        if (event.key === Qt.Key_Tab || event.key === Qt.Key_Backtab) {
            page.wakeChrome();
            return;
        }
        // A combination belongs to the window's shortcuts (Ctrl+Q, Alt+Left)
        if (!page.isShowing || (event.modifiers & (Qt.ControlModifier | Qt.AltModifier | Qt.MetaModifier)))
            return;
        // A held key repeats the seeks only: a toggle would flicker
        if (event.isAutoRepeat && (event.key === Qt.Key_Space || event.key === Qt.Key_M || event.key === Qt.Key_F)) {
            event.accepted = true;
            return;
        }
        switch (event.key) {
        case Qt.Key_Space: page.player.togglePause(); break;
        case Qt.Key_M: page.player.toggleMute(); break;
        case Qt.Key_F: page.Window.window.togglePlayerFullscreen(); break;
        case Qt.Key_Left: page.player.seekRelative(-10); break;
        case Qt.Key_Right: page.player.seekRelative(10); break;
        case Qt.Key_Up: page.player.seekRelative(60); break;
        case Qt.Key_Down: page.player.seekRelative(-60); break;
        default: return;
        }
        page.wakeChrome();
        event.accepted = true;
    }
    Shortcut {
        sequences: [StandardKey.FullScreen]
        enabled: page.isShowing
        autoRepeat: false
        onActivated: {
            page.Window.window.togglePlayerFullscreen();
            page.wakeChrome();
        }
    }
    Timer {
        id: chromeTimer
        objectName: "playerChromeTimer"
        interval: 5000
        onTriggered: {
            if (!page.player.active || queuePanel.opened)
                return;
            // A control of a hidden bar would keep the keys: the page takes them back
            if (page.focusIsInOsd())
                page.focusPrimary();
            page.showChrome = false;
        }
    }
    // Moving the keyboard through the bars keeps them up
    Connections {
        target: page.Window.window
        function onActiveFocusItemChanged() {
            if (page.focusIsInOsd())
                page.wakeChrome();
        }
    }
    focus: true
    Component.onCompleted: { wakeChrome(); Qt.callLater(focusPrimary); }
    Connections {
        target: page.player
        function onFinished() {
            if (page.isShowing && !page.player.active)
                Qt.callLater(() => { if (page.isShowing) page.Window.window.pageStack.pop(); });
        }
    }
    Rectangle { anchors.fill: parent; color: "black" }
    MpvVideoItem {
        id: video
        objectName: "videoRenderer"
        anchors.fill: parent
        player: page.player
    }
    // Only a move of the pointer wakes the bars: the scene repeats the last hover position on every frame that changes something under it
    HoverHandler {
        property point lastPosition: Qt.point(-1, -1)
        onPointChanged: {
            if (point.position.x === lastPosition.x && point.position.y === lastPosition.y)
                return;
            lastPosition = point.position;
            page.wakeChrome();
        }
    }
    TapHandler { onTapped: page.wakeChrome() }

    // The loading and error overlay stays visible when the bars hide
    ColumnLayout {
        anchors.centerIn: parent
        visible: page.player.loading || !!page.player.errorText
        spacing: Kirigami.Units.largeSpacing
        Controls.BusyIndicator {
            Layout.alignment: Qt.AlignHCenter
            running: visible
            visible: page.player.loading
        }
        Controls.Label {
            color: "white"
            textFormat: Text.PlainText
            text: page.player.errorText || i18n("Opening video…")
        }
        Controls.Button {
            visible: !!page.player.errorText
            text: i18n("Close Player")
            onClicked: page.closePlayer()
        }
    }
    Controls.Button {
        id: skipButton
        objectName: "skipSegmentButton"
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: Kirigami.Units.largeSpacing
        anchors.bottomMargin: bottomBar.height + Kirigami.Units.largeSpacing
        visible: page.player.skipLabel.length > 0
        text: page.player.skipLabel
        icon.name: "media-skip-forward"
        onClicked: { page.player.skipSegment(); page.wakeChrome(); }
    }

    Timer {
        id: previewTimer
        interval: 160
        onTriggered: if (timeline.pressed)
            page.player.previewAt(Math.round(timeline.dragSeconds * 10000000))
    }
    Rectangle {
        id: previewPanel
        anchors.bottom: parent.bottom
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottomMargin: bottomBar.height + Kirigami.Units.largeSpacing
        width: Kirigami.Units.gridUnit * 16
        height: previewImage.implicitHeight > 0 ? Kirigami.Units.gridUnit * 11 : 0
        radius: Kirigami.Units.smallSpacing
        color: "#e9000000"
        visible: timeline.pressed && !!page.player.previewImage
        ColumnLayout {
            anchors.fill: parent
            anchors.margins: Kirigami.Units.smallSpacing
            Image {
                id: previewImage
                Layout.fillWidth: true
                Layout.fillHeight: true
                fillMode: Image.PreserveAspectFit
                source: page.player.previewImage
                asynchronous: true
            }
            Controls.Label {
                Layout.alignment: Qt.AlignHCenter
                color: "white"
                text: page.player.timeText(Math.floor(timeline.dragSeconds * 10000000))
            }
        }
    }

    // The bars float above the video and fade together; the complementary colour set gives their text, icons and the shade behind them, which fades into the video
    Item {
        id: osd
        objectName: "playerOsd"
        anchors.fill: parent
        readonly property bool wanted: page.showChrome || !page.player.active || queuePanel.opened
        enabled: wanted
        opacity: wanted ? 1 : 0
        // Visible as soon as it is wanted, so the Tab that wakes the bars finds their controls while they fade in
        visible: wanted || opacity > 0
        Kirigami.Theme.colorSet: Kirigami.Theme.Complementary
        Kirigami.Theme.inherit: false
        Behavior on opacity {
            NumberAnimation { duration: Kirigami.Units.longDuration; easing.type: Easing.OutCubic }
        }

        Rectangle {
            id: topBar
            objectName: "playerTopBar"
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: topRow.implicitHeight + Kirigami.Units.largeSpacing * 2 + Kirigami.Units.gridUnit * 2
            gradient: Gradient {
                GradientStop { position: 0; color: Qt.alpha(Kirigami.Theme.backgroundColor, 0.8) }
                GradientStop { position: 1; color: Qt.alpha(Kirigami.Theme.backgroundColor, 0) }
            }
            RowLayout {
                id: topRow
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: Kirigami.Units.largeSpacing
                spacing: Kirigami.Units.largeSpacing
                Controls.ToolButton {
                    objectName: "playerBackButton"
                    Layout.alignment: Qt.AlignTop
                    focusPolicy: Qt.TabFocus
                    display: Controls.AbstractButton.IconOnly
                    icon.name: page.mirrored ? "go-previous-symbolic-rtl" : "go-previous-symbolic"
                    text: i18nc("@action:button", "Back")
                    Controls.ToolTip.text: text
                    Controls.ToolTip.visible: hovered || visualFocus
                    Controls.ToolTip.delay: Kirigami.Units.toolTipDelay
                    onClicked: page.closePlayer()
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 0
                    Kirigami.Heading {
                        objectName: "playerTitle"
                        Layout.fillWidth: true
                        level: 2
                        textFormat: Text.PlainText
                        text: page.player.title || i18n("Now Playing")
                        elide: Text.ElideRight
                    }
                    Controls.Label {
                        objectName: "playerSubtitle"
                        Layout.fillWidth: true
                        visible: text.length > 0
                        textFormat: Text.PlainText
                        text: page.player.subtitle
                        elide: Text.ElideRight
                    }
                }
            }
        }

        Rectangle {
            id: bottomBar
            objectName: "playerBottomBar"
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: controls.implicitHeight + Kirigami.Units.largeSpacing * 2 + Kirigami.Units.gridUnit * 2
            gradient: Gradient {
                GradientStop { position: 0; color: Qt.alpha(Kirigami.Theme.backgroundColor, 0) }
                GradientStop { position: 1; color: Qt.alpha(Kirigami.Theme.backgroundColor, 0.85) }
            }
            ColumnLayout {
                id: controls
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: Kirigami.Units.largeSpacing
                spacing: Kirigami.Units.smallSpacing
                RowLayout {
                    Layout.fillWidth: true
                    Controls.Label {
                        text: page.player.timeText(page.player.positionTicks)
                    }
                    Controls.Slider {
                        id: timeline
                        objectName: "playbackTimeline"
                        Layout.fillWidth: true
                        focusPolicy: Qt.TabFocus
                        enabled: page.player.canSeek && page.player.durationTicks > 0
                        from: 0
                        to: Math.max(1, page.player.durationTicks / 10000000)
                        // The arrow keys move the timeline by ten seconds once Tab reached it
                        stepSize: 10
                        property real dragSeconds: 0
                        value: pressed ? dragSeconds : page.player.positionTicks / 10000000
                        onMoved: {
                            page.wakeChrome();
                            if (!pressed) {
                                page.player.seek(Math.round(value * 10000000));
                                return;
                            }
                            dragSeconds = value;
                            previewTimer.restart();
                        }
                        onPressedChanged: {
                            if (pressed) {
                                dragSeconds = page.player.positionTicks / 10000000;
                                page.wakeChrome();
                            } else {
                                previewTimer.stop();
                                page.player.seek(Math.round(dragSeconds * 10000000));
                                page.player.clearPreview();
                                page.wakeChrome();
                            }
                        }
                        Accessible.name: i18n("Playback position")
                    }
                    Controls.Label {
                        text: page.player.timeText(page.player.durationTicks)
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Kirigami.Units.smallSpacing
                    Controls.ToolButton {
                        focusPolicy: Qt.TabFocus
                        icon.name: "media-skip-backward"
                        text: i18n("Previous")
                        onClicked: { page.player.previous(); page.wakeChrome(); }
                    }
                    Controls.ToolButton {
                        id: playButton
                        objectName: "playerPauseButton"
                        focusPolicy: Qt.TabFocus
                        text: page.player.paused ? i18n("Resume") : i18n("Pause")
                        icon.name: page.player.paused ? "media-playback-start" : "media-playback-pause"
                        enabled: page.player.active && !page.player.loading
                        onClicked: { page.player.togglePause(); page.wakeChrome(); }
                    }
                    Controls.ToolButton {
                        focusPolicy: Qt.TabFocus
                        icon.name: "media-playback-stop"
                        text: i18n("Stop")
                        onClicked: page.player.stop()
                    }
                    Controls.ToolButton {
                        focusPolicy: Qt.TabFocus
                        icon.name: "media-skip-forward"
                        text: i18n("Next")
                        onClicked: { page.player.next(); page.wakeChrome(); }
                    }
                    Controls.ToolButton {
                        focusPolicy: Qt.TabFocus
                        checkable: true
                        checked: page.player.shuffle
                        icon.name: "media-playlist-shuffle"
                        text: i18n("Shuffle")
                        onClicked: page.player.setShuffle(!page.player.shuffle)
                    }
                    Controls.ToolButton {
                        focusPolicy: Qt.TabFocus
                        icon.name: "media-playlist-repeat"
                        text: [i18n("Repeat Off"), i18n("Repeat All"), i18n("Repeat One")][page.player.repeatMode]
                        onClicked: page.player.setRepeatMode((page.player.repeatMode + 1) % 3)
                    }
                    Item { Layout.fillWidth: true }
                    Controls.ToolButton {
                        focusPolicy: Qt.TabFocus
                        icon.name: page.player.muted ? "audio-volume-muted" : "audio-volume-high"
                        text: page.player.muted ? i18n("Unmute") : i18n("Mute")
                        onClicked: page.player.toggleMute()
                    }
                    Controls.Slider {
                        objectName: "playerVolumeSlider"
                        Layout.preferredWidth: Kirigami.Units.gridUnit * 5
                        focusPolicy: Qt.TabFocus
                        from: 0
                        to: 100
                        stepSize: 5
                        value: page.player.volume
                        onMoved: page.player.setVolume(Math.round(value))
                        Accessible.name: i18n("Volume")
                    }
                    Controls.ToolButton {
                        focusPolicy: Qt.TabFocus
                        text: i18n("Tracks")
                        icon.name: "audio-headphones"
                        onClicked: tracksMenu.popup()
                        Controls.Menu {
                            id: tracksMenu
                            Kirigami.Theme.colorSet: Kirigami.Theme.Window
                            Kirigami.Theme.inherit: false
                            title: i18n("Audio and Subtitles")
                            Controls.MenuItem { text: i18n("Audio tracks"); enabled: false }
                            Instantiator {
                                model: page.player.audioTracks
                                delegate: Controls.MenuItem {
                                    text: modelData.name
                                    checkable: true
                                    checked: modelData.selected
                                    onTriggered: page.player.selectAudioTrack(modelData.id)
                                }
                                onObjectAdded: (index, object) => tracksMenu.insertItem(index + 1, object)
                                onObjectRemoved: (index, object) => tracksMenu.removeItem(object)
                            }
                            Controls.MenuSeparator {}
                            Controls.MenuItem { text: i18n("No subtitles"); onTriggered: page.player.selectSubtitleTrack(0) }
                            Instantiator {
                                model: page.player.subtitleTracks
                                delegate: Controls.MenuItem {
                                    text: modelData.name
                                    checkable: true
                                    checked: modelData.selected
                                    onTriggered: page.player.selectSubtitleTrack(modelData.id)
                                }
                                onObjectAdded: (index, object) => tracksMenu.addItem(object)
                                onObjectRemoved: (index, object) => tracksMenu.removeItem(object)
                            }
                            Controls.MenuSeparator { visible: page.player.externalSubtitles.length > 0 }
                            Controls.MenuItem {
                                text: i18n("Download external subtitles")
                                enabled: false
                                visible: page.player.externalSubtitles.length > 0
                            }
                            Instantiator {
                                model: page.player.externalSubtitles
                                delegate: Controls.MenuItem {
                                    text: modelData.name
                                    onTriggered: page.player.downloadSubtitle(modelData.index)
                                }
                                onObjectAdded: (index, object) => tracksMenu.addItem(object)
                                onObjectRemoved: (index, object) => tracksMenu.removeItem(object)
                            }
                        }
                    }
                    Controls.ComboBox {
                        id: speed
                        Layout.preferredWidth: Kirigami.Units.gridUnit * 6
                        focusPolicy: Qt.TabFocus
                        model: ["0.5×", "0.75×", "1×", "1.25×", "1.5×", "2×", "3×", "4×"]
                        currentIndex: {
                            const values = [0.5, 0.75, 1, 1.25, 1.5, 2, 3, 4];
                            return values.indexOf(page.player.rate);
                        }
                        onActivated: index => page.player.setRate([0.5, 0.75, 1, 1.25, 1.5, 2, 3, 4][index])
                        Accessible.name: i18n("Playback speed")
                    }
                    Controls.ToolButton {
                        focusPolicy: Qt.TabFocus
                        icon.name: "settings-configure"
                        text: i18n("AV Sync")
                        onClicked: delayMenu.popup()
                        Controls.Menu {
                            id: delayMenu
                            Kirigami.Theme.colorSet: Kirigami.Theme.Window
                            Kirigami.Theme.inherit: false
                            Controls.MenuItem { text: i18n("Audio delay: %1 s", page.player.audioDelay.toFixed(1)); enabled: false }
                            Controls.MenuItem { text: i18n("Audio earlier (-0.1 s)"); onTriggered: page.player.adjustAudioDelay(Math.max(-10, page.player.audioDelay - 0.1)) }
                            Controls.MenuItem { text: i18n("Audio later (+0.1 s)"); onTriggered: page.player.adjustAudioDelay(Math.min(10, page.player.audioDelay + 0.1)) }
                            Controls.MenuSeparator {}
                            Controls.MenuItem { text: i18n("Subtitle delay: %1 s", page.player.subtitleDelay.toFixed(1)); enabled: false }
                            Controls.MenuItem { text: i18n("Subtitles earlier (-0.1 s)"); onTriggered: page.player.adjustSubtitleDelay(Math.max(-10, page.player.subtitleDelay - 0.1)) }
                            Controls.MenuItem { text: i18n("Subtitles later (+0.1 s)"); onTriggered: page.player.adjustSubtitleDelay(Math.min(10, page.player.subtitleDelay + 0.1)) }
                        }
                    }
                    Controls.ToolButton {
                        focusPolicy: Qt.TabFocus
                        icon.name: "view-list-details"
                        text: i18n("Now Playing")
                        onClicked: queuePanel.open()
                    }
                    Controls.ToolButton {
                        focusPolicy: Qt.TabFocus
                        text: page.player.sleepRemainingMinutes > 0 ? i18n("Sleep: %1 min", page.player.sleepRemainingMinutes) : i18n("Sleep Timer")
                        icon.name: "chronometer"
                        onClicked: sleepMenu.popup()
                        Controls.Menu {
                            id: sleepMenu
                            Kirigami.Theme.colorSet: Kirigami.Theme.Window
                            Kirigami.Theme.inherit: false
                            Instantiator {
                                model: [0, 15, 30, 45, 60, 90, 120]
                                delegate: Controls.MenuItem {
                                    required property int modelData
                                    text: modelData === 0 ? i18n("Off") : i18n("%1 minutes", modelData)
                                    onTriggered: page.player.setSleepTimer(modelData)
                                }
                                onObjectAdded: (index, object) => sleepMenu.insertItem(index, object)
                                onObjectRemoved: (index, object) => sleepMenu.removeItem(object)
                            }
                        }
                    }
                    Controls.ToolButton {
                        focusPolicy: Qt.TabFocus
                        text: i18n("Screenshot")
                        // The full-colour camera-photo of breeze has path data Qt's SVG renderer truncates, with a warning on every draw
                        icon.name: "camera-photo-symbolic"
                        onClicked: {
                            page.player.screenshot();
                            page.showChrome = true;
                            screenshotNotice.restart();
                        }
                    }
                }
                Controls.Label {
                    visible: screenshotNotice.running && !!page.player.screenshotPath
                    Layout.fillWidth: true
                    textFormat: Text.PlainText
                    elide: Text.ElideMiddle
                    text: i18n("Screenshot saved to %1", page.player.screenshotPath)
                }
                Timer { id: screenshotNotice; interval: 6000 }
            }
        }
    }
    Controls.Popup {
        id: queuePanel
        objectName: "nowPlayingPanel"
        modal: false
        focus: true
        width: Math.min(page.width, Kirigami.Units.gridUnit * 27)
        height: Math.min(page.height * 0.75, Kirigami.Units.gridUnit * 32)
        x: page.width - width
        y: page.height - height
        closePolicy: Controls.Popup.CloseOnEscape | Controls.Popup.CloseOnPressOutside
        // The hide timer gives way while the panel is open: it starts again when the panel goes
        onClosed: page.wakeChrome()
        ColumnLayout {
            anchors.fill: parent
            Controls.Label { text: i18n("Now Playing Queue"); font.bold: true }
            ListView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                model: page.connection.queue
                delegate: Controls.ItemDelegate {
                    required property int index
                    required property string itemId
                    required property string name
                    width: ListView.view.width
                    text: name
                    highlighted: page.player.currentIndex === index
                    onClicked: { page.player.playQueue(index); queuePanel.close(); }
                }
            }
        }
    }
}
