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
    // Below the window's wide limit the controls row shows icons only, each with its text as tooltip and accessible name
    readonly property bool narrow: width < (page.Window.window?.wideLimit ?? 0)
    // The time button shows the remaining time instead of the total, for as long as this page lives
    property bool showRemaining: false
    readonly property bool fullScreen: page.Window.window?.visibility === Window.FullScreen
    readonly property string timeLabel: {
        const elapsed = page.player.timeText(page.player.positionTicks);
        // An unknown length has neither a total nor a remaining time
        if (page.player.durationTicks <= 0)
            return elapsed;
        const remaining = i18nc("@action:button remaining playback time, with a minus sign", "−%1", page.player.timeText(Math.max(0, page.player.durationTicks - page.player.positionTicks)));
        if (page.narrow)
            return page.showRemaining ? remaining : elapsed;
        return i18nc("@action:button elapsed playback time, then the total or the remaining time", "%1 / %2", elapsed, page.showRemaining ? remaining : page.player.timeText(page.player.durationTicks));
    }
    // An open menu or the queue panel keeps the bars up
    readonly property bool menuOpen: queuePanel.opened || audioMenu.opened || subtitlesMenu.opened || moreMenu.opened

    // The page keeps the keyboard: the controls of the bars take it only through Tab, so the keys work whatever was clicked
    function focusPrimary() { page.forceActiveFocus(Qt.TabFocusReason); }
    function wakeChrome() { showChrome = true; chromeTimer.restart(); }
    function closePlayer() { page.player.stop(); page.Window.window.pageStack.pop(); }
    // A menu of the bottom bar opens above its button, aligned to the button's far edge
    function openMenu(menu, button) {
        const corner = button.mapToItem(page, page.mirrored ? 0 : button.width, 0);
        menu.popup(page.mirrored ? corner.x : corner.x - menu.implicitWidth, corner.y - menu.implicitHeight);
    }
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
            if (!page.player.active || page.menuOpen)
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
        readonly property bool wanted: page.showChrome || !page.player.active || page.menuOpen
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
                OsdButton {
                    objectName: "playerBackButton"
                    Layout.alignment: Qt.AlignTop
                    icon.name: page.mirrored ? "go-previous-symbolic-rtl" : "go-previous-symbolic"
                    text: i18nc("@action:button", "Back")
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
                RowLayout {
                    objectName: "playerControlRow"
                    Layout.fillWidth: true
                    spacing: Kirigami.Units.smallSpacing
                    OsdButton {
                        objectName: "playerPreviousButton"
                        visible: page.player.hasPrevious
                        icon.name: "media-skip-backward"
                        text: i18n("Previous")
                        onClicked: { page.player.previous(); page.wakeChrome(); }
                    }
                    OsdButton {
                        id: playButton
                        objectName: "playerPauseButton"
                        text: page.player.paused ? i18n("Resume") : i18n("Pause")
                        icon.name: page.player.paused ? "media-playback-start" : "media-playback-pause"
                        enabled: page.player.active && !page.player.loading
                        onClicked: { page.player.togglePause(); page.wakeChrome(); }
                    }
                    OsdButton {
                        objectName: "playerNextButton"
                        visible: page.player.hasNext
                        icon.name: "media-skip-forward"
                        text: i18n("Next")
                        onClicked: { page.player.next(); page.wakeChrome(); }
                    }
                    OsdButton {
                        objectName: "playerMuteButton"
                        icon.name: page.player.muted ? "audio-volume-muted" : "audio-volume-high"
                        text: page.player.muted ? i18n("Unmute") : i18n("Mute")
                        onClicked: { page.player.toggleMute(); page.wakeChrome(); }
                    }
                    Controls.Slider {
                        objectName: "playerVolumeSlider"
                        Layout.preferredWidth: Kirigami.Units.gridUnit * 5
                        // A narrow row has no room for it
                        visible: !page.narrow
                        focusPolicy: Qt.TabFocus
                        from: 0
                        to: 100
                        stepSize: 5
                        value: page.player.volume
                        onMoved: { page.player.setVolume(Math.round(value)); page.wakeChrome(); }
                        Accessible.name: i18n("Volume")
                    }
                    Controls.ToolButton {
                        objectName: "playerTimeButton"
                        focusPolicy: Qt.TabFocus
                        text: page.timeLabel
                        Accessible.description: Controls.ToolTip.text
                        Controls.ToolTip.text: !page.showRemaining ? i18nc("@info:tooltip", "Show remaining time") : page.narrow ? i18nc("@info:tooltip", "Show elapsed time") : i18nc("@info:tooltip", "Show total time")
                        Controls.ToolTip.visible: hovered || visualFocus
                        Controls.ToolTip.delay: Kirigami.Units.toolTipDelay
                        onClicked: { page.showRemaining = !page.showRemaining; page.wakeChrome(); }
                    }
                    Item { Layout.fillWidth: true }
                    OsdButton {
                        id: audioButton
                        objectName: "playerAudioButton"
                        display: page.narrow ? Controls.AbstractButton.IconOnly : Controls.AbstractButton.TextBesideIcon
                        enabled: page.player.audioTracks.length > 0
                        // The menu role draws the style's menu arrow beside the label
                        Accessible.role: Accessible.ButtonMenu
                        icon.name: "audio-headphones"
                        text: i18nc("@action:button audio tracks", "Audio")
                        onClicked: page.openMenu(audioMenu, audioButton)
                    }
                    OsdButton {
                        id: subtitlesButton
                        objectName: "playerSubtitlesButton"
                        display: page.narrow ? Controls.AbstractButton.IconOnly : Controls.AbstractButton.TextBesideIcon
                        Accessible.role: Accessible.ButtonMenu
                        icon.name: "media-view-subtitles-symbolic"
                        text: i18nc("@action:button subtitle tracks", "Subtitles")
                        onClicked: page.openMenu(subtitlesMenu, subtitlesButton)
                    }
                    OsdButton {
                        objectName: "playerFullScreenButton"
                        icon.name: page.fullScreen ? "view-restore" : "view-fullscreen"
                        text: page.fullScreen ? i18nc("@action:button", "Exit Full Screen Mode") : i18nc("@action:button", "Full Screen Mode")
                        onClicked: { page.Window.window.togglePlayerFullscreen(); page.wakeChrome(); }
                    }
                    OsdButton {
                        id: moreButton
                        objectName: "playerMoreButton"
                        icon.name: "overflow-menu"
                        text: i18nc("@action:button", "More Actions")
                        onClicked: page.openMenu(moreMenu, moreButton)
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
    // The menus of the bottom bar belong to the page, not to the bars: they keep the window's colours over the dark bars
    Controls.Menu {
        id: audioMenu
        objectName: "playerAudioMenu"
        onClosed: page.wakeChrome()
        Instantiator {
            model: page.player.audioTracks
            delegate: Controls.MenuItem {
                text: modelData.name
                checkable: true
                autoExclusive: true
                checked: modelData.selected
                onTriggered: page.player.selectAudioTrack(modelData.id)
            }
            onObjectAdded: (index, object) => audioMenu.insertItem(index, object)
            onObjectRemoved: (index, object) => audioMenu.removeItem(object)
        }
    }
    Controls.Menu {
        id: subtitlesMenu
        objectName: "playerSubtitlesMenu"
        onClosed: page.wakeChrome()
        Controls.MenuItem {
            objectName: "playerSubtitlesOff"
            text: i18nc("@item:inmenu no subtitles", "Off")
            checkable: true
            autoExclusive: true
            checked: !page.player.subtitleTracks.some(track => track.selected)
            onTriggered: page.player.selectSubtitleTrack(0)
        }
        Instantiator {
            model: page.player.subtitleTracks
            delegate: Controls.MenuItem {
                text: modelData.name
                checkable: true
                autoExclusive: true
                checked: modelData.selected
                onTriggered: page.player.selectSubtitleTrack(modelData.id)
            }
            onObjectAdded: (index, object) => subtitlesMenu.insertItem(index + 1, object)
            onObjectRemoved: (index, object) => subtitlesMenu.removeItem(object)
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
            onObjectAdded: (index, object) => subtitlesMenu.addItem(object)
            onObjectRemoved: (index, object) => subtitlesMenu.removeItem(object)
        }
    }
    Controls.Menu {
        id: moreMenu
        objectName: "playerMoreMenu"
        onClosed: page.wakeChrome()
        Controls.MenuItem {
            objectName: "playerShuffleItem"
            text: i18n("Shuffle")
            icon.name: "media-playlist-shuffle"
            checkable: true
            checked: page.player.shuffle
            // A series started from one of its episodes plays in order: only a queue shuffles, and a shuffle that is on can always be turned off
            enabled: page.player.queueCount > 1 || page.player.shuffle
            onTriggered: page.player.setShuffle(!page.player.shuffle)
        }
        Controls.Menu {
            objectName: "playerRepeatMenu"
            title: i18n("Repeat")
            icon.name: "media-playlist-repeat"
            // An item played alone has nothing to repeat into; a repeat that is on can always be turned off
            enabled: page.player.queueCount > 0 || page.player.hasNext || page.player.hasPrevious || page.player.repeatMode !== 0
            Controls.MenuItem {
                objectName: "playerRepeatOff"
                text: i18nc("@item:inmenu repeat", "Off")
                checkable: true
                autoExclusive: true
                checked: page.player.repeatMode === 0
                onTriggered: page.player.setRepeatMode(0)
            }
            Controls.MenuItem {
                objectName: "playerRepeatAll"
                text: i18nc("@item:inmenu repeat", "All")
                checkable: true
                autoExclusive: true
                checked: page.player.repeatMode === 1
                onTriggered: page.player.setRepeatMode(1)
            }
            Controls.MenuItem {
                objectName: "playerRepeatOne"
                text: i18nc("@item:inmenu repeat", "Current Item")
                checkable: true
                autoExclusive: true
                checked: page.player.repeatMode === 2
                onTriggered: page.player.setRepeatMode(2)
            }
        }
        Controls.Menu {
            id: speedMenu
            objectName: "playerSpeedMenu"
            title: i18n("Playback Speed")
            icon.name: "speedometer"
            Instantiator {
                model: [0.5, 0.75, 1, 1.25, 1.5, 2, 3, 4]
                delegate: Controls.MenuItem {
                    required property real modelData
                    text: i18nc("@item:inmenu playback speed, %1 is the factor", "%1×", modelData)
                    checkable: true
                    autoExclusive: true
                    checked: page.player.rate === modelData
                    onTriggered: page.player.setRate(modelData)
                }
                onObjectAdded: (index, object) => speedMenu.insertItem(index, object)
                onObjectRemoved: (index, object) => speedMenu.removeItem(object)
            }
        }
        Controls.Menu {
            title: i18n("AV Sync")
            icon.name: "settings-configure"
            Controls.MenuItem { text: i18n("Audio delay: %1 s", page.player.audioDelay.toFixed(1)); enabled: false }
            Controls.MenuItem { text: i18n("Audio earlier (-0.1 s)"); onTriggered: page.player.adjustAudioDelay(Math.max(-10, page.player.audioDelay - 0.1)) }
            Controls.MenuItem { text: i18n("Audio later (+0.1 s)"); onTriggered: page.player.adjustAudioDelay(Math.min(10, page.player.audioDelay + 0.1)) }
            Controls.MenuSeparator {}
            Controls.MenuItem { text: i18n("Subtitle delay: %1 s", page.player.subtitleDelay.toFixed(1)); enabled: false }
            Controls.MenuItem { text: i18n("Subtitles earlier (-0.1 s)"); onTriggered: page.player.adjustSubtitleDelay(Math.max(-10, page.player.subtitleDelay - 0.1)) }
            Controls.MenuItem { text: i18n("Subtitles later (+0.1 s)"); onTriggered: page.player.adjustSubtitleDelay(Math.min(10, page.player.subtitleDelay + 0.1)) }
        }
        Controls.Menu {
            id: sleepMenu
            title: page.player.sleepRemainingMinutes > 0 ? i18n("Sleep: %1 min", page.player.sleepRemainingMinutes) : i18n("Sleep Timer")
            icon.name: "chronometer"
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
        Controls.MenuSeparator {}
        Controls.MenuItem {
            objectName: "playerQueueItem"
            text: i18n("Now Playing Queue")
            icon.name: "view-media-playlist"
            onTriggered: queuePanel.open()
        }
        Controls.MenuItem {
            text: i18n("Screenshot")
            // The full-colour camera-photo of breeze has path data Qt's SVG renderer truncates, with a warning on every draw
            icon.name: "camera-photo-symbolic"
            onTriggered: {
                page.player.screenshot();
                page.wakeChrome();
                screenshotNotice.restart();
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

    // A button of the bars: icon only unless it says otherwise, its text as tooltip and accessible name, and no keyboard focus from a click, so the page keeps the keys
    component OsdButton: Controls.ToolButton {
        focusPolicy: Qt.TabFocus
        display: Controls.AbstractButton.IconOnly
        Controls.ToolTip.text: text
        Controls.ToolTip.visible: display === Controls.AbstractButton.IconOnly && (hovered || visualFocus)
        Controls.ToolTip.delay: Kirigami.Units.toolTipDelay
    }
}
